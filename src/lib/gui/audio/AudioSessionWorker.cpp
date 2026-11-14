/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "AudioSessionWorker.h"
#include <stdexcept>
#include <thread>

namespace {
// 跨控件共享一个租约：旧窗口关闭后，后台释放声卡未结束时，新窗口也不能叠加音频任务。
// 租约本身由任务持有，避免退出阶段访问已析构的静态 mutex/QObject。
std::shared_ptr<std::atomic<bool>> workerLease()
{
  static const auto lease = std::make_shared<std::atomic<bool>>(false);
  return lease;
}
} // namespace

struct AudioSessionWorker::State
{
  std::atomic<bool> stop{false};
  std::atomic<bool> localPlayback{false};
  std::mutex mutex;
  Update pending;
};

AudioSessionWorker::Update::~Update()
{
  if (offer)
    preview::erase(*offer);
}

AudioSessionWorker::AudioSessionWorker(preview::SessionOptions options, bool localPlayback)
    : m_state(std::make_shared<State>())
{
  // 凭据由后台任务持有并最终擦除；异常启动同样走此删除器，不能留下邀请副本。
  auto ownedOptions = std::shared_ptr<preview::SessionOptions>(
      new preview::SessionOptions(std::move(options)),
      [](preview::SessionOptions *value) {
        if (value->invitation)
          preview::erase(*value->invitation);
        delete value;
      }
  );
  if (options.invitation)
    preview::erase(*options.invitation);
  const auto state = m_state;
  state->localPlayback = localPlayback;
  ownedOptions->localPlayback = ownedOptions->receive ? nullptr : &state->localPlayback;
  // 三个单槽分别合并邀请、恢复消息和可见状态，另保留至多八条转换日志。
  // 所有回调只接触共享状态，界面繁忙或已销毁时不积压 Qt 事件，也不调用 GUI 日志对象。
  // 回调由任务统一持有，先清除外部回调以免保留控件引用。只有接收端发布邀请；
  // 发送端必须保持 onReady 为空，否则会违反真实 Session 的方向校验，无法开始握手。
  ownedOptions->onReady = {};
  if (ownedOptions->receive)
    ownedOptions->onReady = [state](const preview::Invitation &offer) {
      std::lock_guard lock(state->mutex);
      if (state->pending.offer)
        preview::erase(*state->pending.offer);
      state->pending.offer = offer;
    };
  // 恢复回调与邮箱必须成对存在。未协商恢复能力时二者都为空，继续使用旧端兼容路径。
  ownedOptions->onRecovery = {};
  if (ownedOptions->recovery)
    ownedOptions->onRecovery = [state](const preview::RecoveryMessage &message) {
      std::lock_guard lock(state->mutex);
      state->pending.recovery = message;
    };
  const auto lease = workerLease();
  if (lease->exchange(true))
    throw std::runtime_error("音频仍在后台清理，稍后重试");
  try {
    std::thread([state, lease, ownedOptions = std::move(ownedOptions)]() mutable {
      std::string failure;
      try {
        preview::runSession(*ownedOptions, state->stop, [state](const preview::SessionEvent &event) {
          std::lock_guard lock(state->mutex);
          // 日志同样有界，且只由界面线程输出；后台清理不会在应用退出时访问已析构的 Qt Logger。
          // 正常每秒统计不入日志；禁止记录密钥、nonce 或完整控制消息。
          if (event.state == preview::SessionState::Failed || event.state == preview::SessionState::Diagnostic ||
              event.state == preview::SessionState::Recovering || event.state == preview::SessionState::Recovered ||
              event.state == preview::SessionState::Renewing || event.state == preview::SessionState::Stopped) {
            if (state->pending.logs.size() == 8)
              state->pending.logs.erase(state->pending.logs.begin());
            state->pending.logs.push_back(event);
          }
          // 本机播放接管和密钥退休是黏性状态，不被后续的普通 Connected 统计覆盖。
          state->pending.playbackOverridden |= event.state == preview::SessionState::LocalPlaybackOverridden;
          state->pending.renewing |= event.state == preview::SessionState::Renewing;
          if (event.state == preview::SessionState::Recovering ||
              event.state == preview::SessionState::WaitingForMedia ||
              event.state == preview::SessionState::Connected || event.state == preview::SessionState::Recovered)
            state->pending.status = event;
        });
      } catch (const std::exception &error) {
        failure = error.what();
      } catch (...) {
        failure = "音频后台任务异常";
      }
      // runSession 返回前已释放声卡/守护进程；继续销毁选项后才开放下一次音频启动。
      // finished 是最终屏障，界面只消费邮箱，不 join，也不访问后台任务的堆栈。
      ownedOptions.reset();
      lease->store(false);
      std::lock_guard lock(state->mutex);
      // 失败原因与完成标志一起发布，防止界面提前消费原因，最终误显示成正常停止。
      state->pending.failure = std::move(failure);
      state->pending.finished = true;
    }).detach();
  } catch (...) {
    lease->store(false);
    throw;
  }
}

AudioSessionWorker::~AudioSessionWorker()
{
  requestStop();
}

void AudioSessionWorker::requestStop() noexcept
{
  m_state->stop = true;
}

void AudioSessionWorker::setLocalPlayback(bool enabled) noexcept
{
  m_state->localPlayback = enabled;
}

AudioSessionWorker::Update AudioSessionWorker::takeUpdate()
{
  std::lock_guard lock(m_state->mutex);
  Update result = std::move(m_state->pending);
  // 保留完成标志和两项黏性状态；消费后清空单槽并擦除已转交邀请在源槽中的密钥。
  if (m_state->pending.offer)
    preview::erase(*m_state->pending.offer);
  m_state->pending.offer.reset();
  m_state->pending.recovery.reset();
  m_state->pending.status.reset();
  m_state->pending.logs.clear();
  m_state->pending.failure.clear();
  return result;
}
