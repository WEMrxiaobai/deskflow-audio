/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// Explicit standalone development test; no audio, network traffic, or persistent secrets.
#include "Invitation.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <aclapi.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <windows.h>

namespace {
void check(bool condition, const char *message)
{
  if (!condition)
    throw std::runtime_error(message);
}
template <class F> void rejects(F &&function)
{
  bool rejected = false;
  try {
    function();
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  check(rejected, "Invalid invitation was accepted");
}
} // namespace
int main()
{
  std::filesystem::path path;
  preview::Invitation invite;
  try {
    invite = preview::createInvitation(24818, {"127.0.0.1", "192.168.1.20"});
    check(invite.expiresUnixSeconds >= preview::unixSeconds() + 598, "Invitation expiration is wrong");
    const auto nonce = preview::randomNonce();
    path = std::filesystem::temp_directory_path() /
           ("deskflow-audio-invitation-test-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(GetTickCount64()) + ".txt");
    preview::saveInvitation(path, invite);
    auto loaded = preview::loadInvitation(path);
    check(
        loaded.mediaKey == invite.mediaKey && loaded.controlKey == invite.controlKey && loaded.ssrc == invite.ssrc &&
            loaded.addresses == invite.addresses,
        "Invitation did not round-trip"
    );
    preview::erase(loaded);
    rejects([&] { preview::saveInvitation(path, invite); });
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR security = nullptr;
    check(
        GetNamedSecurityInfoW(
            path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &security
        ) == ERROR_SUCCESS,
        "Cannot inspect test DACL"
    );
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision{};
    const bool protectedAcl = GetSecurityDescriptorControl(security, &control, &revision) &&
                              (control & SE_DACL_PROTECTED) != 0 && dacl && dacl->AceCount == 1;
    LocalFree(security);
    check(protectedAcl, "Invitation DACL is not protected owner-only");
    for (const auto type : {preview::ControlType::Hello, preview::ControlType::Ack, preview::ControlType::Stop}) {
      const auto packet = preview::controlPacket(invite, type, nonce);
      preview::ControlType received = preview::ControlType::Stop;
      preview::Nonce receivedNonce{};
      check(
          packet.size() == 69 && preview::verifyControl(invite, packet, received, receivedNonce) && received == type &&
              receivedNonce == nonce,
          "Valid HMAC packet failed"
      );
      for (std::size_t i = 0; i < packet.size(); ++i) {
        auto corrupt = packet;
        corrupt[i] ^= 1;
        received = preview::ControlType::Stop;
        receivedNonce.fill(91);
        const auto before = receivedNonce;
        check(!preview::verifyControl(invite, corrupt, received, receivedNonce), "Modified packet authenticated");
        check(received == preview::ControlType::Stop && receivedNonce == before, "Failed verify changed outputs");
      }
      check(
          !preview::verifyControl(invite, std::span(packet).first(68), received, receivedNonce), "Short packet accepted"
      );
      auto longPacket = packet;
      longPacket.push_back(0);
      check(!preview::verifyControl(invite, longPacket, received, receivedNonce), "Long packet accepted");
    }
    auto secondNonce = nonce;
    secondNonce[0] ^= 1;
    auto derived = preview::sessionMediaKey(invite, nonce);
    auto repeated = preview::sessionMediaKey(invite, nonce);
    auto different = preview::sessionMediaKey(invite, secondNonce);
    check(derived == repeated && derived != different && derived != invite.mediaKey, "Session key derivation failed");
    SecureZeroMemory(derived.data(), derived.size());
    SecureZeroMemory(repeated.data(), repeated.size());
    SecureZeroMemory(different.data(), different.size());
    for (const auto *address :
         {"0.0.0.0", "255.255.255.255", "224.0.0.1", "239.1.2.3", "127.1", "127.00.0.1", "1.2.3.4\n"})
      rejects([&] {
        auto invalid = preview::createInvitation(24818, {address});
        preview::erase(invalid);
      });
    rejects([&] {
      auto invalid = preview::createInvitation(0, {"127.0.0.1"});
      preview::erase(invalid);
    });
    rejects([&] {
      auto invalid = preview::createInvitation(24818, {});
      preview::erase(invalid);
    });
    auto expired = invite;
    expired.expiresUnixSeconds = preview::unixSeconds() - 1;
    rejects([&] { (void)preview::controlPacket(expired, preview::ControlType::Hello, nonce); });
    preview::erase(expired);
    // 缩短测试邀请寿命，真实跨过有效期：新建连接被拒绝，活动会话仍能验证并发出 STOP。
    auto shortLived = invite;
    shortLived.expiresUnixSeconds = preview::unixSeconds() + 1;
    {
      preview::EstablishedSessionControl active(shortLived, nonce);
      const auto stop = active.stopPacket();
      check(
          stop == preview::controlPacket(shortLived, preview::ControlType::Stop, nonce),
          "Established STOP is not wire compatible"
      );
      const auto hello = preview::controlPacket(shortLived, preview::ControlType::Hello, nonce);
      check(!active.verifyStop(hello), "Active session accepted a new HELLO as STOP");
      std::this_thread::sleep_for(std::chrono::milliseconds(2100));
      preview::ControlType received = preview::ControlType::Hello;
      preview::Nonce receivedNonce{};
      check(
          !preview::verifyControl(shortLived, hello, received, receivedNonce), "Expired invitation opened a new session"
      );
      rejects([&] { preview::EstablishedSessionControl invalid(shortLived, nonce); });
      check(
          active.verifyStop(stop) && active.verifyStop(active.stopPacket()),
          "Credential expiry disabled STOP for an established session"
      );
      for (std::size_t index = 0; index < stop.size(); ++index) {
        auto corrupt = stop;
        corrupt[index] ^= 1;
        check(!active.verifyStop(corrupt), "Tampered established STOP passed verification");
      }
      auto anotherNonce = nonce;
      anotherNonce[0] ^= 1;
      auto fresh = invite;
      preview::EstablishedSessionControl other(fresh, anotherNonce);
      check(!other.verifyStop(stop), "Old session STOP affected another nonce");
      preview::erase(fresh);
    }
    preview::erase(shortLived);
    // Deliberate malformed plaintext only after deleting the validated generated test file.
    std::filesystem::remove(path);
    {
      std::ofstream malformed(path, std::ios::binary);
      malformed << std::string(4097, 'X');
    }
    rejects([&] {
      auto invalid = preview::loadInvitation(path);
      preview::erase(invalid);
    });
    std::filesystem::remove(path);
    preview::erase(invite);
    std::cout << "Invitation tests passed: roundtrip, protected ACL, no overwrite, HMAC/tamper/length, session "
                 "derivation, input bounds\n";
    return 0;
  } catch (const std::exception &error) {
    preview::erase(invite);
    if (!path.empty()) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
    std::cerr << "Invitation tests failed: " << error.what() << '\n';
    return 1;
  }
}
