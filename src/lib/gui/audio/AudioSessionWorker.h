/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "Session.h"
#include <memory>
#include <vector>

// 音频任务独立持有停止标志、本机播放策略和回调邮箱；析构只请求停止。
// 声卡、静音守护进程及会话的阻塞清理全部留在后台，不引用任何 QWidget/QObject。
class AudioSessionWorker final
{
public:
  struct Update
  {
    std::optional<preview::Invitation> offer;
    std::optional<preview::RecoveryMessage> recovery;
    std::optional<preview::SessionEvent> status;
    std::vector<preview::SessionEvent> logs;
    bool playbackOverridden = false;
    bool finished = false;
    bool renewing = false;
    std::string failure;

    // 邀请只在内存中短暂转交；包括界面已停止而丢弃更新的路径，都必须擦除密钥副本。
    ~Update();
    Update() = default;
    Update(Update &&) = default;
    Update &operator=(Update &&) = default;
    Update(const Update &) = delete;
    Update &operator=(const Update &) = delete;
  };

  explicit AudioSessionWorker(preview::SessionOptions options, bool localPlayback);
  ~AudioSessionWorker();
  AudioSessionWorker(const AudioSessionWorker &) = delete;
  AudioSessionWorker &operator=(const AudioSessionWorker &) = delete;

  // 以下方法均为短操作，不等待后台线程或声卡。
  void requestStop() noexcept;
  void setLocalPlayback(bool enabled) noexcept;
  Update takeUpdate();

private:
  struct State;
  std::shared_ptr<State> m_state;
};
