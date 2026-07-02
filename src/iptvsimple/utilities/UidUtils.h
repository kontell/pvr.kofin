/*
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include <cstdint>
#include <string>

namespace iptvsimple
{
namespace utilities
{
  // djb2 hash reduced to a non-negative int, shared by every UID producer
  // (channels, EPG entries, timers, recordings). The values are persisted in
  // Kodi's PVR database, so they must stay bit-identical to the historical
  // implementation on each platform:
  // - bytes pass through plain `char` (sign extension on signed-char
  //   platforms is part of the historical values — do not "fix" to unsigned);
  // - iteration stops at an embedded NUL, like the old c_str() loop;
  // - arithmetic is uint32_t (defined wraparound) producing the same bit
  //   patterns the old signed-overflow code yielded in practice;
  // - the final reduction reproduces std::abs(), except INT32_MIN maps to 0
  //   instead of the old undefined behavior.
  inline int GenerateUid(const std::string& str)
  {
    uint32_t hash = 0;
    for (const char ch : str)
    {
      if (ch == '\0')
        break;
      const int c = ch;
      hash = ((hash << 5) + hash) + static_cast<uint32_t>(c); // hash * 33 + c
    }

    const int32_t signedHash = static_cast<int32_t>(hash);
    const uint32_t magnitude = signedHash < 0
        ? 0u - static_cast<uint32_t>(signedHash)
        : static_cast<uint32_t>(signedHash);
    return static_cast<int>(magnitude & 0x7FFFFFFFu);
  }

  // Kodi channel UIDs are hashed from channelName + streamURL, and the stream
  // URL for Jellyfin channels is baseUrl + "/LiveTv/Channels/" + jellyfinId
  // (set in JellyfinChannelLoader::LoadChannels). The recording manager
  // reconstructs the same UID from timer data — keep the recipe in one place
  // so the two can never drift.
  inline int GenerateChannelUid(const std::string& channelName,
                                const std::string& baseUrl,
                                const std::string& jellyfinChannelId)
  {
    return GenerateUid(channelName + baseUrl + "/LiveTv/Channels/" + jellyfinChannelId);
  }
} // namespace utilities
} // namespace iptvsimple
