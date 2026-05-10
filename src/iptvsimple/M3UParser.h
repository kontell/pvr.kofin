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
#include <vector>

namespace iptvsimple
{

class InstanceSettings;

// Per-channel record extracted from a reference M3U.
// Keyed by channel name (case-insensitive) for matching with Jellyfin channels.
struct M3UChannelInfo
{
  // Catchup
  CatchupMode catchupMode = CatchupMode::DISABLED;
  int catchupDays = 0;
  std::string catchupSource;
  float catchupCorrectionHours = 0.0f;
  bool hasCatchup = false;

  // Channel groups (semicolon-split from group-title= and #EXTGRP:)
  std::vector<std::string> groupNames;

  // Optional logo override (tvg-logo)
  std::string tvgLogo;

  // Properties to forward to Kodi (inputstream.*, mimetype, etc.)
  std::map<std::string, std::string> kodiProps;

  // KofinProps consumed by BuildDeviceProfile (kofin-force-remux,
  // kofin-bitrate-limit, kofin-force-transcode). Stored separately so they
  // don't leak into Kodi PVRStreamProperty output.
  std::map<std::string, std::string> kofinProps;
};

class M3UParser
{
public:
  M3UParser(std::shared_ptr<InstanceSettings>& settings);

  // Parse the reference M3U and populate the channel-info map.
  // Supports local files and HTTP/HTTPS URLs.
  bool Parse();

  // Look up info for a channel by name (case-insensitive).
  const M3UChannelInfo* GetChannelInfo(const std::string& channelName) const;

  // All unique group names encountered in the playlist, in first-seen order.
  const std::vector<std::string>& GetAllGroupNames() const { return m_allGroupNames; }

private:
  std::map<std::string, M3UChannelInfo> m_channels;     // lowercase name → info
  std::vector<std::string> m_allGroupNames;             // unique, first-seen order
  std::shared_ptr<InstanceSettings> m_settings;
};

} // namespace iptvsimple
