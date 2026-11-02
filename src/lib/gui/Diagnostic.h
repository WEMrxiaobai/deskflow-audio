/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2024 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

namespace deskflow::gui::diagnostic {

bool startsAtLogin();
bool startupPointsToCurrentExecutable();
bool setStartsAtLogin(bool enabled);
bool clearSettings(bool enableRestart, bool clearStartup = true);

}
