/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "Invitation.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <aclapi.h>
#include <bcrypt.h>
#include <windows.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ws2_32.lib")

namespace preview {
namespace {
constexpr std::size_t maxFileBytes = 4096, signedBytes = 37, controlBytes = 69;
constexpr std::string_view magic = "DSHAPV01";
[[noreturn]] void fail(const char *message)
{
  throw std::runtime_error(message);
}
struct Handle
{
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Handle()
  {
    if (value != INVALID_HANDLE_VALUE && value != nullptr)
      CloseHandle(value);
  }
};
struct WipeString
{
  std::string value;
  ~WipeString()
  {
    if (!value.empty())
      SecureZeroMemory(value.data(), value.size());
  }
};
struct WipeBytes
{
  std::vector<std::uint8_t> value;
  ~WipeBytes()
  {
    if (!value.empty())
      SecureZeroMemory(value.data(), value.size());
  }
};
struct InvitationGuard
{
  Invitation &value;
  bool active = true;
  ~InvitationGuard()
  {
    if (active)
      erase(value);
  }
};
void randomBytes(std::span<std::uint8_t> bytes)
{
  if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    fail("Audio invitation random generation failed");
}
bool validType(ControlType type)
{
  return type == ControlType::Hello || type == ControlType::Ack || type == ControlType::Stop;
}
void validate(const Invitation &invite)
{
  const auto now = unixSeconds();
  if (invite.port == 0 || invite.ssrc == 0 || invite.expiresUnixSeconds < now ||
      invite.expiresUnixSeconds - now > 660 || invite.addresses.empty() || invite.addresses.size() > 8)
    fail("Invalid or expired audio invitation");
  for (std::size_t i = 0; i < invite.addresses.size(); ++i) {
    const auto &address = invite.addresses[i];
    IN_ADDR ipv4{};
    if (address.empty() || address.size() > 15 || InetPtonA(AF_INET, address.c_str(), &ipv4) != 1)
      fail("Invalid invitation IPv4 address");
    const auto ip = ntohl(ipv4.S_un.S_addr);
    if (ip == 0 || ip == 0xffffffffU || (ip & 0xf0000000U) == 0xe0000000U)
      fail("Unusable invitation IPv4 address");
    char canonical[INET_ADDRSTRLEN]{};
    if (!InetNtopA(AF_INET, &ipv4, canonical, sizeof(canonical)) || address != canonical)
      fail("Noncanonical invitation IPv4 address");
    for (std::size_t j = 0; j < i; ++j)
      if (invite.addresses[j] == address)
        fail("Duplicate invitation IPv4 address");
  }
}
std::array<std::uint8_t, 32> hmac(std::span<const std::uint8_t> key, std::span<const std::uint8_t> bytes)
{
  struct Algorithm
  {
    BCRYPT_ALG_HANDLE value = nullptr;
    ~Algorithm()
    {
      if (value)
        BCryptCloseAlgorithmProvider(value, 0);
    }
  } algorithm;
  if (BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0)
    fail("Cannot open invitation HMAC provider");
  DWORD objectSize = 0, copied = 0;
  if (BCryptGetProperty(
          algorithm.value, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &copied, 0
      ) < 0 ||
      copied != sizeof(objectSize) || objectSize > 65536)
    fail("Cannot query invitation HMAC storage");
  WipeBytes object{std::vector<std::uint8_t>(objectSize)};
  struct Hash
  {
    BCRYPT_HASH_HANDLE value = nullptr;
    ~Hash()
    {
      if (value)
        BCryptDestroyHash(value);
    }
  } hash;
  if (BCryptCreateHash(
          algorithm.value, &hash.value, object.value.data(), objectSize, const_cast<PUCHAR>(key.data()),
          static_cast<ULONG>(key.size()), 0
      ) < 0 ||
      BCryptHashData(hash.value, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) < 0)
    fail("Cannot compute invitation HMAC");
  std::array<std::uint8_t, 32> result{};
  if (BCryptFinishHash(hash.value, result.data(), static_cast<ULONG>(result.size()), 0) < 0)
    fail("Cannot finish invitation HMAC");
  return result;
}
void appendHex(std::string &output, std::span<const std::uint8_t> bytes)
{
  constexpr char alphabet[] = "0123456789abcdef";
  for (const auto byte : bytes) {
    output.push_back(alphabet[byte >> 4]);
    output.push_back(alphabet[byte & 15]);
  }
}
void parseHex(std::string_view text, std::span<std::uint8_t> bytes)
{
  if (text.size() != bytes.size() * 2)
    fail("Invalid invitation key encoding");
  const auto digit = [](char c) -> unsigned {
    if (c >= '0' && c <= '9')
      return static_cast<unsigned>(c - '0');
    if (c >= 'a' && c <= 'f')
      return static_cast<unsigned>(c - 'a' + 10);
    fail("Invalid invitation key encoding");
  };
  for (std::size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<std::uint8_t>((digit(text[i * 2]) << 4) | digit(text[i * 2 + 1]));
}
template <class T> T number(std::string_view text)
{
  T result{};
  if (text.empty() || (text.size() > 1 && text.front() == '0'))
    fail("Invalid invitation integer");
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    fail("Invalid invitation integer");
  return result;
}
void putBig(std::span<std::uint8_t> output, std::uint64_t value)
{
  for (auto it = output.rbegin(); it != output.rend(); ++it) {
    *it = static_cast<std::uint8_t>(value);
    value >>= 8;
  }
}
std::uint64_t getBig(std::span<const std::uint8_t> input)
{
  std::uint64_t value = 0;
  for (const auto byte : input)
    value = (value << 8) | byte;
  return value;
}
} // namespace

std::uint64_t unixSeconds()
{
  const auto value =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  if (value < 0)
    fail("Invalid system time for audio invitation");
  return static_cast<std::uint64_t>(value);
}
void erase(Invitation &invite) noexcept
{
  SecureZeroMemory(invite.mediaKey.data(), invite.mediaKey.size());
  SecureZeroMemory(invite.controlKey.data(), invite.controlKey.size());
  invite.ssrc = 0;
  invite.expiresUnixSeconds = 0;
  invite.port = 0;
  invite.addresses.clear();
}
Invitation createInvitation(std::uint16_t port, std::vector<std::string> addresses)
{
  Invitation result;
  InvitationGuard guard{result};
  result.port = port;
  result.addresses = std::move(addresses);
  result.expiresUnixSeconds = unixSeconds() + 600;
  do {
    randomBytes(std::span(reinterpret_cast<std::uint8_t *>(&result.ssrc), sizeof(result.ssrc)));
  } while (!result.ssrc);
  randomBytes(result.mediaKey);
  randomBytes(result.controlKey);
  validate(result);
  guard.active = false;
  return result;
}
void saveInvitation(const std::filesystem::path &path, const Invitation &invite)
{
  validate(invite);
  WipeString text;
  text.value = "DSH-AUDIO-PREVIEW-1\nmedia=";
  appendHex(text.value, invite.mediaKey);
  text.value += "\ncontrol=";
  appendHex(text.value, invite.controlKey);
  text.value += "\nssrc=" + std::to_string(invite.ssrc) + "\nexpires=" + std::to_string(invite.expiresUnixSeconds) +
                "\nport=" + std::to_string(invite.port) + "\naddresses=" + std::to_string(invite.addresses.size()) +
                "\n";
  for (const auto &address : invite.addresses)
    text.value += "address=" + address + "\n";
  if (text.value.size() > maxFileBytes)
    fail("Invitation file exceeds limit");
  Handle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value))
    fail("Cannot query invitation file owner");
  DWORD tokenSize = 0;
  GetTokenInformation(token.value, TokenUser, nullptr, 0, &tokenSize);
  if (tokenSize == 0 || tokenSize > 65536)
    fail("Cannot query invitation file owner");
  std::vector<std::uint8_t> user(tokenSize);
  if (!GetTokenInformation(token.value, TokenUser, user.data(), tokenSize, &tokenSize))
    fail("Cannot query invitation file owner");
  const auto sid = reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid;
  EXPLICIT_ACCESSW access{};
  access.grfAccessPermissions = FILE_ALL_ACCESS;
  access.grfAccessMode = SET_ACCESS;
  access.grfInheritance = NO_INHERITANCE;
  access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  access.Trustee.TrusteeType = TRUSTEE_IS_USER;
  access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
  struct Acl
  {
    PACL value = nullptr;
    ~Acl()
    {
      if (value)
        LocalFree(value);
    }
  } acl;
  if (SetEntriesInAclW(1, &access, nullptr, &acl.value) != ERROR_SUCCESS)
    fail("Cannot protect invitation file");
  SECURITY_DESCRIPTOR descriptor{};
  if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
      !SetSecurityDescriptorOwner(&descriptor, sid, FALSE) ||
      !SetSecurityDescriptorDacl(&descriptor, TRUE, acl.value, FALSE) ||
      !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
    fail("Cannot protect invitation file");
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), &descriptor, FALSE};
  Handle file;
  file.value =
      CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file.value == INVALID_HANDLE_VALUE)
    fail("Cannot create invitation file (existing paths are not overwritten)");
  DWORD written = 0;
  if (!WriteFile(file.value, text.value.data(), static_cast<DWORD>(text.value.size()), &written, nullptr) ||
      written != text.value.size() || !FlushFileBuffers(file.value)) {
    FILE_DISPOSITION_INFO disposition{TRUE};
    SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition));
    fail("Cannot write invitation file");
  }
}
Invitation loadInvitation(const std::filesystem::path &path)
{
  Handle file;
  file.value = CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr
  );
  if (file.value == INVALID_HANDLE_VALUE)
    fail("Cannot open invitation file");
  BY_HANDLE_FILE_INFORMATION info{};
  LARGE_INTEGER size{};
  if (!GetFileInformationByHandle(file.value, &info) ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
      !GetFileSizeEx(file.value, &size) || size.QuadPart <= 0 || size.QuadPart > maxFileBytes)
    fail("Invalid invitation file type or size");
  WipeString text{std::string(static_cast<std::size_t>(size.QuadPart), '\0')};
  DWORD read = 0;
  if (!ReadFile(file.value, text.value.data(), static_cast<DWORD>(text.value.size()), &read, nullptr) ||
      read != text.value.size())
    fail("Cannot read invitation file");
  for (const unsigned char c : text.value)
    if ((c < 32 && c != '\n') || c > 126)
      fail("Invitation file must be strict ASCII with LF lines");
  std::string_view remaining(text.value);
  const auto line = [&](std::string_view prefix) -> std::string_view {
    const auto end = remaining.find('\n');
    if (end == std::string_view::npos)
      fail("Invalid invitation line termination");
    const auto current = remaining.substr(0, end);
    remaining.remove_prefix(end + 1);
    if (!current.starts_with(prefix))
      fail("Unexpected invitation field");
    return current.substr(prefix.size());
  };
  if (line("") != "DSH-AUDIO-PREVIEW-1")
    fail("Unsupported invitation format");
  Invitation invite;
  InvitationGuard guard{invite};
  parseHex(line("media="), invite.mediaKey);
  parseHex(line("control="), invite.controlKey);
  invite.ssrc = number<std::uint32_t>(line("ssrc="));
  invite.expiresUnixSeconds = number<std::uint64_t>(line("expires="));
  invite.port = number<std::uint16_t>(line("port="));
  const auto count = number<unsigned>(line("addresses="));
  if (count == 0 || count > 8)
    fail("Invalid invitation address count");
  for (unsigned i = 0; i < count; ++i)
    invite.addresses.emplace_back(line("address="));
  if (!remaining.empty())
    fail("Trailing invitation fields");
  validate(invite);
  guard.active = false;
  return invite;
}
Nonce randomNonce()
{
  Nonce nonce{};
  randomBytes(nonce);
  return nonce;
}
std::array<std::uint8_t, 30> sessionMediaKey(const Invitation &invite, const Nonce &nonce)
{
  validate(invite);
  constexpr std::string_view domain = "DSHAPV01-SRTP-SESSION";
  std::array<std::uint8_t, domain.size() + 16> message{};
  std::copy(domain.begin(), domain.end(), message.begin());
  std::copy(nonce.begin(), nonce.end(), message.begin() + domain.size());
  auto digest = hmac(invite.mediaKey, message);
  std::array<std::uint8_t, 30> result{};
  std::copy_n(digest.begin(), result.size(), result.begin());
  SecureZeroMemory(digest.data(), digest.size());
  return result;
}
std::vector<std::uint8_t> controlPacket(const Invitation &invite, ControlType type, const Nonce &nonce)
{
  validate(invite);
  if (!validType(type))
    fail("Invalid audio control packet type");
  std::vector<std::uint8_t> result(controlBytes);
  std::copy(magic.begin(), magic.end(), result.begin());
  result[8] = static_cast<std::uint8_t>(type);
  std::copy(nonce.begin(), nonce.end(), result.begin() + 9);
  putBig(std::span(result).subspan(25, 4), invite.ssrc);
  putBig(std::span(result).subspan(29, 8), invite.expiresUnixSeconds);
  auto tag = hmac(invite.controlKey, std::span(result).first(signedBytes));
  std::copy(tag.begin(), tag.end(), result.begin() + signedBytes);
  SecureZeroMemory(tag.data(), tag.size());
  return result;
}
bool verifyControl(
    const Invitation &invite, std::span<const std::uint8_t> packet, ControlType &type, Nonce &nonce
) noexcept
{
  try {
    if (packet.size() != controlBytes || packet.size() > 128)
      return false;
    validate(invite);
    if (!std::equal(magic.begin(), magic.end(), packet.begin()))
      return false;
    const auto candidateType = static_cast<ControlType>(packet[8]);
    if (!validType(candidateType) || getBig(packet.subspan(25, 4)) != invite.ssrc ||
        getBig(packet.subspan(29, 8)) != invite.expiresUnixSeconds)
      return false;
    auto expected = hmac(invite.controlKey, packet.first(signedBytes));
    volatile unsigned difference = 0;
    for (std::size_t i = 0; i < expected.size(); ++i)
      difference = difference | unsigned(expected[i] ^ packet[signedBytes + i]);
    SecureZeroMemory(expected.data(), expected.size());
    if (difference != 0)
      return false;
    Nonce candidateNonce{};
    std::copy_n(packet.begin() + 9, candidateNonce.size(), candidateNonce.begin());
    type = candidateType;
    nonce = candidateNonce;
    return true;
  } catch (...) {
    return false;
  }
}
} // namespace preview
