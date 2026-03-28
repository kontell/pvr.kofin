/*
 *  Copyright (C) 2005-2022 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "SettingsMigration.h"

using namespace iptvsimple;
using namespace iptvsimple::utilities;

bool SettingsMigration::MigrateSettings(kodi::addon::IAddonInstance& /*target*/)
{
  // No migration needed — single-instance addon using global settings
  return false;
}

bool SettingsMigration::IsMigrationSetting(const std::string& /*key*/)
{
  return false;
}
