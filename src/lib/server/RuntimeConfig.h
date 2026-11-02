/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#pragma once

#include "Config.h"
#include "common/CoreRuntimeIdentity.h"
#include <sstream>
#include <string>

// Config::read commits only after the entire document parses successfully.
// Update the identity from the exact same original bytes only after that commit.
inline void parseRuntimeConfig(deskflow::server::Config &config, QJsonObject &identity, const std::string &bytes)
{
  std::istringstream input(bytes);
  input >> config;
  setCoreRuntimeLayout(identity, QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())));
}
