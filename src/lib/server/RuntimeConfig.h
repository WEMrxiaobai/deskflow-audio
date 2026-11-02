/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#pragma once

#include "Config.h"
#include "common/CoreRuntimeIdentity.h"
#include <sstream>
#include <string>

// Config::read commits only after the entire document parses successfully.
// Update the identity from the exact same original bytes only after that commit.
inline bool parseRuntimeConfig(
    deskflow::server::Config &config, QJsonObject &identity, const std::string &bytes,
    const std::string &requiredPrimary = {}
)
{
  // Parsing and validation must not change the live configuration on failure.
  auto candidate = config;
  std::istringstream input(bytes);
  input >> candidate;
  if (!requiredPrimary.empty() && !candidate.isComputer(requiredPrimary))
    return false;
  config = candidate;
  setCoreRuntimeLayout(identity, QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())));
  return true;
}
