/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#include "PairedAudioGroup.h"
#include "PairedAudioControl.h"
#include "common/Settings.h"
#include <QElapsedTimer>
#include <QHostAddress>
#include <QLoggingCategory>
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <thread>

namespace {
const QString kServer = QStringLiteral("server");
QString mediaAddress(const QString &address)
{
  const QHostAddress host(address);
  bool isV4 = false;
  const auto ip = host.toIPv4Address(&isV4);
  if (isV4)
    return QHostAddress(ip).toString();
  if (host.protocol() != QAbstractSocket::IPv6Protocol || !host.scopeId().isEmpty() || host.isMulticast() ||
      host == QHostAddress::AnyIPv6 || host.isInSubnet(QHostAddress(QStringLiteral("fe80::")), 10))
    return {};
  return host.toString();
}
bool token(const QString &value, int length)
{
  return value.size() == length && std::all_of(value.begin(), value.end(), [](QChar c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
QJsonObject offerJson(const preview::Invitation &value)
{
  return {
      {"media", QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(value.mediaKey.data()), 30).toBase64())},
      {"control",
       QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(value.controlKey.data()), 32).toBase64())},
      {"ssrc", QString::number(value.ssrc)},
      {"expires", QString::number(value.expiresUnixSeconds)},
      {"port", int(value.port)}
  };
}
bool readOffer(const QJsonObject &object, const QString &address, preview::Invitation &value)
{
  const auto media = QByteArray::fromBase64(object.value("media").toString().toLatin1());
  const auto control = QByteArray::fromBase64(object.value("control").toString().toLatin1());
  bool ssrc = false, expiry = false;
  value.ssrc = object.value("ssrc").toString().toUInt(&ssrc);
  value.expiresUnixSeconds = object.value("expires").toString().toULongLong(&expiry);
  const int port = object.value("port").toInt();
  const auto now = preview::unixSeconds();
  if (media.size() != 30 || control.size() != 32 || !ssrc || !value.ssrc || !expiry ||
      !preview::validInvitationExpiry(value.expiresUnixSeconds, now) || port < 1 || port > 65535 ||
      mediaAddress(address).isEmpty())
    return false;
  std::copy(media.begin(), media.end(), value.mediaKey.begin());
  std::copy(control.begin(), control.end(), value.controlKey.begin());
  value.port = static_cast<std::uint16_t>(port);
  value.addresses = {mediaAddress(address).toStdString()};
  return true;
}
} // namespace

struct PairedAudioGroup::Impl
{
  struct Peer
  {
    bool seen = false, capable = false, listen = false, available = false;
    QString name;
  };
  struct Route
  {
    QString id, source, sink, name, address;
    QJsonObject json() const
    {
      return {{"route", id}, {"source", source}, {"name", name}, {"address", address}};
    }
  };
  struct Worker
  {
    Route route;
    bool receive = false;
    bool overridden = false;
    std::uint64_t packets = 0;
    QString status;
    std::atomic<bool> stop{false};
    std::thread thread;
    preview::SessionOptions options;
    void join()
    {
      stop = true;
      if (thread.joinable()) {
        QElapsedTimer elapsed;
        elapsed.start();
        thread.join();
        // Route changes and disconnects can wait here on the owning Qt thread.
        // Report only unusually slow stops; never log from an audio callback.
        if (elapsed.elapsed() >= 100)
          qWarning("Audio worker shutdown took %lld ms", static_cast<long long>(elapsed.elapsed()));
      }
      options.sharedPlayback.reset();
      options.localOutputState.reset();
      if (options.invitation) {
        preview::erase(*options.invitation);
        options.invitation.reset();
      }
    }
    ~Worker()
    {
      join();
    }
  };
  PairedAudioGroup *q;
  PairedAudioControl *control;
  const std::atomic<bool> *localPlayback;
  QMap<QString, Peer> peers;
  QMap<QString, Route> routes;
  QMap<QString, std::shared_ptr<Worker>> receiving;
  std::shared_ptr<Worker> sending;
  std::shared_ptr<preview::SharedPlayback> playback;
  std::shared_ptr<preview::LocalOutputState> localOutput;
  Route sendRoute;
  QString sink;
  bool listen = false, available = true, active = false, destroying = false, test = false, testRender = false;
  QJsonObject lastPlan;
  QMap<QString, QJsonObject> sentPlans;

  Impl(PairedAudioGroup *owner, PairedAudioControl *transport, const std::atomic<bool> *policy)
      : q(owner),
        control(transport),
        localPlayback(policy)
  {
  }
  QString preferenceId(const QString &source) const
  {
    // "server" is a route role, never a persistent identity.
    return source == kServer && !control->isServer() ? control->peerIds().value(0) : source;
  }
  void releaseLocalOutput()
  {
    if (localOutput && !preview::restoreLocalOutputState(localOutput))
      Q_EMIT q->localPlaybackRestoreFailed();
    localOutput.reset();
  }
  void clearRuntime()
  {
    for (auto &worker : receiving)
      worker->stop = true;
    if (sending)
      sending->stop = true;
    for (auto &worker : receiving)
      worker->join();
    if (sending)
      sending->join();
    receiving.clear();
    sending.reset();
    playback.reset();
    releaseLocalOutput();
    sendRoute = {};
    lastPlan = {};
    Q_EMIT q->senderChanged(false, false);
    Q_EMIT q->sourcesChanged();
  }
  void mode(bool value)
  {
    if (active == value)
      return;
    if (!value)
      clearRuntime();
    active = value;
    Q_EMIT q->modeChanged(value);
  }
  bool current(const std::shared_ptr<Worker> &worker) const
  {
    return !destroying && (worker->receive ? receiving.value(worker->route.id) == worker : sending == worker);
  }
  void dispatchOffer(const Route &route, const QJsonObject &offer)
  {
    if (!active)
      return;
    if (!control->isServer()) {
      const auto hub = control->peerIds().value(0);
      control->send(hub, {{"type", "group-offer"}, {"route", route.id}, {"offer", offer}});
      return;
    }
    const auto found = routes.constFind(route.id);
    if (found == routes.cend())
      return;
    if (found->source == kServer)
      startSender(sendRoute, offer);
    else
      control->send(found->source, {{"type", "group-offer"}, {"route", found->id}, {"offer", offer}});
  }
  void run(const std::shared_ptr<Worker> &worker)
  {
    worker->thread = std::thread([this, worker] {
      do {
        QString failure;
        try {
          preview::runSession(worker->options, worker->stop, [this, worker](const preview::SessionEvent &event) {
            if (event.state != preview::SessionState::Connected &&
                event.state != preview::SessionState::LocalPlaybackOverridden)
              return;
            const bool overridden = event.state == preview::SessionState::LocalPlaybackOverridden;
            const auto packets = event.packets;
            QMetaObject::invokeMethod(
                q,
                [this, worker, overridden, packets] {
                  if (!current(worker))
                    return;
                  worker->packets = std::max(worker->packets, packets);
                  worker->overridden = worker->overridden || overridden;
                  if (worker->receive) {
                    worker->status = QStringLiteral("正在接收");
                    Q_EMIT q->sourcesChanged();
                    Q_EMIT q->statusChanged(QStringLiteral("正在收听 %1 台电脑的声音").arg(receiving.size()));
                  } else {
                    Q_EMIT q->senderChanged(true, worker->overridden);
                    Q_EMIT q->statusChanged(
                        worker->overridden ? QStringLiteral("正在共享系统声音 · 本机播放遵循系统当前设置")
                                           : QStringLiteral("正在向「%1」共享系统声音").arg(worker->route.name)
                    );
                  }
                },
                Qt::QueuedConnection
            );
          });
        } catch (const std::exception &error) {
          failure = QString::fromUtf8(error.what());
        }
        if (worker->stop)
          break;
        QMetaObject::invokeMethod(
            q,
            [this, worker, failure] {
              if (!current(worker))
                return;
              worker->status = QStringLiteral("正在重连");
              Q_EMIT q->sourcesChanged();
              if (!worker->receive)
                Q_EMIT q->statusChanged(QStringLiteral("%1 · 正在重连，本机播放设置保持不变").arg(failure));
            },
            Qt::QueuedConnection
        );
        // A sender waits for the listener's fresh offer. A failed source never
        // rebuilds or blocks the other decoder or the shared output callback.
        if (!worker->receive)
          break;
        for (int i = 0; i < 100 && !worker->stop; ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
      } while (!worker->stop);
    });
  }
  void startReceiver(const Route &route, std::size_t slot)
  {
    if (!playback)
      playback = std::make_shared<preview::SharedPlayback>(test, testRender);
    auto worker = std::make_shared<Worker>();
    worker->route = route;
    worker->receive = true;
    worker->status = QStringLiteral("正在连接");
    auto &options = worker->options;
    options.receive = true;
    options.test = test;
    options.testRender = testRender;
    options.testDuration = std::chrono::seconds(30);
    options.controlOwnsLifetime = true;
    options.sharedPlayback = playback;
    options.playbackSlot = slot;
    options.expectedSenderAddress = route.address.toStdString();
    options.playbackControl = std::make_shared<preview::SourcePlaybackControl>();
    const auto saved =
        Settings::value(Settings::Gui::AudioSourceControls).toMap().value(preferenceId(route.source)).toMap();
    const float savedVolume = saved.value("volume", .35f).toFloat();
    options.playbackControl->volume = std::isfinite(savedVolume) ? std::clamp(savedVolume, 0.f, 1.f) : .35f;
    options.playbackControl->muted = saved.value("muted", false).toBool();
    std::weak_ptr<Worker> weak = worker;
    options.onReady = [this, weak, route](const preview::Invitation &offer) {
      const auto payload = offerJson(offer);
      QMetaObject::invokeMethod(
          q,
          [this, weak, route, payload] {
            if (auto worker = weak.lock(); worker && current(worker))
              dispatchOffer(route, payload);
          },
          Qt::QueuedConnection
      );
    };
    receiving.insert(route.id, worker);
    run(worker);
  }
  void startSender(const Route &route, const QJsonObject &payload)
  {
    if (sendRoute.id != route.id || !available || !receiving.isEmpty())
      return;
    preview::Invitation offer;
    if (!readOffer(payload, route.address, offer)) {
      preview::erase(offer);
      return;
    }
    if (sending)
      sending->join();
    sending.reset(); // Joins before reusing the retained native mute owner.
    auto worker = std::make_shared<Worker>();
    worker->route = route;
    auto &options = worker->options;
    options.invitation = offer;
    preview::erase(offer);
    options.test = test;
    options.testDuration = std::chrono::seconds(30);
    options.controlOwnsLifetime = true;
    if (!localOutput)
      localOutput = preview::createLocalOutputState();
    if (!test) {
      options.localPlayback = localPlayback;
      options.localOutputState = localOutput;
    }
    sending = worker;
    Q_EMIT q->senderChanged(true, false);
    run(worker);
  }
  void apply(const QJsonObject &plan)
  {
    if (lastPlan == plan)
      return;
    const auto role = plan.value("role").toString();
    const auto items = plan.value("sources").toArray();
    if (role != "idle" && role != "send" && role != "receive")
      return;
    if (role == "receive" && !listen)
      return;
    if ((role == "receive" && items.size() > 2) || (role != "receive" && !items.isEmpty()))
      return;
    QMap<QString, Route> desired;
    QSet<QString> identities;
    for (const auto &item : items) {
      const auto value = item.toObject();
      Route route{
          value.value("route").toString(),
          value.value("source").toString(),
          {},
          value.value("name").toString(),
          mediaAddress(value.value("address").toString())
      };
      if (!token(route.id, 32) || (route.source != kServer && !token(route.source, 64)) || route.name.isEmpty() ||
          route.name.size() > 255 || route.address.isEmpty() || desired.contains(route.id) ||
          identities.contains(route.source))
        return;
      identities.insert(route.source);
      desired.insert(route.id, route);
    }
    Route next;
    if (role == "send") {
      const auto value = plan.value("send").toObject();
      next = {
          value.value("route").toString(),
          {},
          value.value("sink").toString(),
          value.value("name").toString(),
          mediaAddress(value.value("address").toString())
      };
      if (!token(next.id, 32) || (next.sink != kServer && !token(next.sink, 64)) || next.name.isEmpty() ||
          next.name.size() > 255 || next.address.isEmpty())
        return;
    }
    // Remove only changed routes. An unchanged source keeps its decoder, gain,
    // jitter state and registration in the single WASAPI mixer.
    for (auto it = receiving.begin(); it != receiving.end();) {
      if (!desired.contains(it.key())) {
        it.value()->join();
        it = receiving.erase(it);
      } else
        ++it;
    }
    if (receiving.isEmpty() && role != "receive")
      playback.reset();
    if (sendRoute.id != next.id) {
      if (sending)
        sending->join();
      sending.reset();
      if (sendRoute.sink != next.sink || role != "send")
        releaseLocalOutput();
    }
    sendRoute = next;
    if (role != "send")
      Q_EMIT q->senderChanged(false, false);
    QSet<int> occupied;
    for (const auto &worker : receiving)
      occupied.insert(int(worker->options.playbackSlot));
    for (const auto &route : desired)
      if (!receiving.contains(route.id)) {
        const int slot = occupied.contains(0) ? 1 : 0;
        occupied.insert(slot);
        startReceiver(route, std::size_t(slot));
      }
    lastPlan = plan;
    Q_EMIT q->sourcesChanged();
    Q_EMIT q->statusChanged(
        role == "receive" ? QStringLiteral("正在连接 %1 个音频来源").arg(receiving.size())
        : role == "send"  ? QStringLiteral("等待收听电脑建立音频直连")
                          : QStringLiteral("已配对 · 在插耳机的电脑打开收听开关即可")
    );
  }
  QString name(const QString &node) const
  {
    return node == kServer ? Settings::value(Settings::Core::ComputerName).toString() : peers.value(node).name;
  }
  QString address(const QString &node, const QString &other) const
  {
    return mediaAddress(node == kServer ? control->localAddress(other) : control->peerAddress(node));
  }
  void rebuild()
  {
    if (destroying || !control->isServer())
      return;
    auto ids = control->peerIds();
    ids.sort();
    const bool capable =
        std::any_of(ids.begin(), ids.end(), [this](const auto &id) { return peers.value(id).capable; }) &&
        std::none_of(ids.begin(), ids.end(), [this](const auto &id) {
          const auto state = peers.value(id);
          return state.seen && !state.capable;
        });
    mode(capable);
    if (!capable) {
      for (const auto &id : ids)
        if (peers.value(id).capable)
          control->send(id, {{"type", "group-plan"}, {"role", "legacy"}});
      routes.clear();
      sentPlans.clear();
      sink.clear();
      return;
    }
    ids.removeIf([this](const auto &id) { return !peers.value(id).capable; });
    QString nextSink;
    if (listen)
      nextSink = kServer;
    else if (ids.contains(sink) && peers.value(sink).listen)
      nextSink = sink;
    else
      for (const auto &id : ids)
        if (peers.value(id).listen) {
          nextSink = id;
          break;
        }
    QMap<QString, Route> desired;
    auto nodes = ids;
    nodes.prepend(kServer);
    if (!nextSink.isEmpty())
      for (const auto &node : nodes) {
        if (node == nextSink || !(node == kServer ? available : peers.value(node).available) || desired.size() == 2)
          continue;
        const auto sourceAddress = address(node, nextSink);
        const auto sinkAddress = address(nextSink, node);
        if (sourceAddress.isEmpty() || sinkAddress.isEmpty())
          continue;
        Route route{QUuid::createUuid().toString(QUuid::Id128), node, nextSink, name(node), sourceAddress};
        for (const auto &old : routes)
          if (old.source == node && old.sink == nextSink && old.address == sourceAddress) {
            route.id = old.id;
            break;
          }
        desired.insert(route.id, route);
      }
    routes = desired;
    sink = nextSink;
    // Stop old listeners before starting any former listener's loopback capture.
    // Each endpoint applies a complete role, never both capture and playback.
    for (const auto &node : nodes) {
      QJsonObject plan{{"type", "group-plan"}, {"role", "idle"}};
      if (node == sink) {
        QJsonArray sourceList;
        for (const auto &route : routes)
          sourceList.append(route.json());
        plan["role"] = "receive";
        plan["sources"] = sourceList;
      } else
        for (const auto &route : routes)
          if (route.source == node) {
            plan["role"] = "send";
            plan["send"] = QJsonObject{
                {"route", route.id}, {"sink", sink}, {"name", name(sink)}, {"address", address(sink, node)}
            };
          }
      if (node == kServer)
        apply(plan);
      else if (sentPlans.value(node) != plan) {
        sentPlans[node] = plan;
        control->send(node, plan);
      }
    }
  }
};

PairedAudioGroup::PairedAudioGroup(PairedAudioControl *control, const std::atomic<bool> *policy, QObject *parent)
    : QObject(parent),
      d(std::make_unique<Impl>(this, control, policy))
{
  connect(control, &PairedAudioControl::peerConnected, this, [this](const QString &id, const QString &name) {
    d->peers[id].name = name;
  });
  connect(control, &PairedAudioControl::peerDisconnected, this, [this](const QString &id) {
    d->peers.remove(id);
    d->sentPlans.remove(id);
    if (d->control->isServer())
      d->rebuild();
    else
      d->mode(false);
  });
}
PairedAudioGroup::~PairedAudioGroup()
{
  shutdown();
}
void PairedAudioGroup::shutdown()
{
  d->destroying = true;
  disconnect(d->control, nullptr, this, nullptr);
  d->clearRuntime();
}
void PairedAudioGroup::setLocalState(bool listen, bool available)
{
  if (!available && d->sending) {
    d->sending->join();
    d->sending.reset();
    d->releaseLocalOutput();
    d->sendRoute = {};
    d->lastPlan = {};
    Q_EMIT senderChanged(false, false);
  }
  if (!listen && !d->receiving.isEmpty()) {
    for (auto &worker : d->receiving)
      worker->stop = true;
    for (auto &worker : d->receiving)
      worker->join();
    d->receiving.clear();
    d->playback.reset();
    d->lastPlan = {};
    Q_EMIT sourcesChanged();
  }
  d->listen = listen;
  d->available = available;
  d->rebuild();
}
bool PairedAudioGroup::handleMessage(const QString &peer, const QJsonObject &message)
{
  if (d->destroying || !d->control->peerIds().contains(peer))
    return false;
  const auto type = message.value("type").toString();
  if (type == "state") {
    if (!message.value("listen").isBool() || !message.value("available").isBool())
      return true;
    auto &state = d->peers[peer];
    state.seen = true;
    state.capable = message.value("audioVersion").toInt() == 2;
    state.listen = message.value("listen").toBool();
    state.available = message.value("available").toBool();
    if (d->control->isServer())
      d->rebuild();
    else if (!state.capable)
      d->mode(false);
    return state.capable;
  }
  if (!type.startsWith("group-"))
    return false;
  if (type == "group-plan") {
    if (!d->control->isServer()) {
      const auto role = message.value("role").toString();
      if (role == "legacy")
        d->mode(false);
      else if (role == "idle" || role == "send" || role == "receive") {
        d->mode(true);
        d->apply(message);
      }
    }
    return true;
  }
  if (!d->active)
    return true;
  if (type == "group-offer") {
    const auto id = message.value("route").toString();
    if (!token(id, 32))
      return true;
    if (d->control->isServer()) {
      const auto found = d->routes.constFind(id);
      // A client may publish credentials only for a route where it is the listener.
      if (found != d->routes.cend() && found->sink == peer)
        d->dispatchOffer(*found, message.value("offer").toObject());
    } else if (d->sendRoute.id == id)
      d->startSender(d->sendRoute, message.value("offer").toObject());
    return true;
  }
  return true;
}
bool PairedAudioGroup::enabled() const
{
  return d->active;
}
std::vector<PairedAudioGroup::Source> PairedAudioGroup::sources() const
{
  std::vector<Source> result;
  for (const auto &worker : d->receiving)
    result.push_back(
        {worker->route.source, worker->route.name, worker->status, worker->options.playbackControl, worker->packets,
         worker->options.playbackSlot}
    );
  return result;
}
void PairedAudioGroup::saveSourceControls()
{
  auto saved = Settings::value(Settings::Gui::AudioSourceControls).toMap();
  for (const auto &worker : d->receiving) {
    saved[d->preferenceId(worker->route.source)] = QVariantMap{
        {"volume", worker->options.playbackControl->volume.load()},
        {"muted", worker->options.playbackControl->muted.load()}
    };
  }
  Settings::setValue(Settings::Gui::AudioSourceControls, saved);
  Settings::save(false);
}
void PairedAudioGroup::setTestMode(bool render)
{
  if (!d->active) {
    d->test = true;
    d->testRender = render;
  }
}
std::shared_ptr<preview::SharedPlayback> PairedAudioGroup::playbackForTest() const
{
  return d->playback;
}
