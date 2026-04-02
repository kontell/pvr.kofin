/*
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include "data/Channel.h"

#include <map>
#include <memory>
#include <string>

namespace iptvsimple
{

class InstanceSettings;

// Catchup metadata extracted from an M3U reference file.
// Keyed by channel name for matching with Jellyfin channels.
struct M3UCatchupInfo
{
  CatchupMode mode = CatchupMode::DISABLED;
  int days = 0;
  std::string source;      // catchup-source format string
  float correctionHours = 0.0f;
  bool hasCatchup = false;
};

class M3UParser
{
public:
  M3UParser(std::shared_ptr<InstanceSettings>& settings);

  // Parse the reference M3U and populate the catchup map.
  // Supports local files and HTTP/HTTPS URLs.
  bool Parse();

  // Look up catchup info for a channel by name (case-insensitive).
  const M3UCatchupInfo* GetCatchupInfo(const std::string& channelName) const;

private:
  std::string ReadMarkerValue(const std::string& line, const std::string& marker) const;
  CatchupMode ParseCatchupMode(const std::string& modeStr) const;

  std::map<std::string, M3UCatchupInfo> m_catchupMap;  // lowercase name → info
  std::shared_ptr<InstanceSettings> m_settings;
};

} // namespace iptvsimple
