/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "InstanceSettings.h"

#include <cstdio>
#include <random>

using namespace iptvsimple;

namespace
{
  std::string GenerateUUID()
  {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    uint32_t a = dist(gen);
    uint16_t b = static_cast<uint16_t>(dist(gen));
    uint16_t c = static_cast<uint16_t>((dist(gen) & 0x0FFF) | 0x4000); // version 4
    uint16_t d = static_cast<uint16_t>((dist(gen) & 0x3FFF) | 0x8000); // variant 1
    uint32_t e1 = dist(gen);
    uint16_t e2 = static_cast<uint16_t>(dist(gen));

    char buf[37];
    std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%08x%04x",
                  a, b, c, d, e1, e2);
    return std::string(buf);
  }
} // anonymous namespace

InstanceSettings::InstanceSettings()
{
  ReadSettings();
}

void InstanceSettings::ReadSettings()
{
  // Device identity — generate once, persist forever
  m_deviceId = kodi::addon::GetSettingString("deviceId", "");
  if (m_deviceId.empty())
  {
    m_deviceId = GenerateUUID();
    kodi::addon::SetSettingString("deviceId", m_deviceId);
  }

  // Server
  m_jellyfinServerAddress = kodi::addon::GetSettingString("jellyfinServerAddress", "");

  // Account state
  m_isLoggedIn = kodi::addon::GetSettingBoolean("isLoggedIn", false);
  m_jellyfinServerName = kodi::addon::GetSettingString("jellyfinServerName", "");
  m_jellyfinDisplayUsername = kodi::addon::GetSettingString("jellyfinDisplayUsername", "");

  // Authentication
  m_jellyfinAccessToken = kodi::addon::GetSettingString("jellyfinAccessToken", "");
  m_jellyfinUserId = kodi::addon::GetSettingString("jellyfinUserId", "");

  // Transcoding
  m_forceTranscode = kodi::addon::GetSettingBoolean("forceTranscode", false);
  m_transcodeHi10P = kodi::addon::GetSettingBoolean("transcodeHi10P", true);
  m_transcodeHevcRext = kodi::addon::GetSettingBoolean("transcodeHevcRext", true);
  m_preferredVideoCodec = kodi::addon::GetSettingInt("preferredVideoCodec", 0);
  m_preferredAudioCodec = kodi::addon::GetSettingInt("preferredAudioCodec", 0);
  m_maxStreamingBitrate = kodi::addon::GetSettingInt("maxStreamingBitrate", 15);

  // Input stream
  m_inputStream = kodi::addon::GetSettingInt("inputStream", 0);
  m_timeshiftEnabled = kodi::addon::GetSettingBoolean("timeshiftEnabled", true);
  m_inProgressInputStream = kodi::addon::GetSettingInt("inProgressInputStream", 0);

  // Catchup
  m_catchupEnabled = kodi::addon::GetSettingBoolean("catchupEnabled", false);
  {
    int pathType = kodi::addon::GetSettingInt("catchupM3UPathType", 0);
    if (pathType == 0) // Local file
      m_catchupM3UPath = kodi::addon::GetSettingString("catchupM3UPath", "");
    else // Network URL
      m_catchupM3UPath = kodi::addon::GetSettingString("catchupM3UUrl", "");
  }
  m_catchupQueryFormat = kodi::addon::GetSettingString("catchupQueryFormat", "");
  m_catchupDays = kodi::addon::GetSettingInt("catchupDays", 5);
  m_allChannelsCatchupMode = kodi::addon::GetSettingInt("allChannelsCatchupMode", 0);
  m_catchupPlayEpgAsLive = kodi::addon::GetSettingBoolean("catchupPlayEpgAsLive", false);
  m_catchupWatchEpgBeginBufferMins = kodi::addon::GetSettingInt("catchupWatchEpgBeginBufferMins", 5);
  m_catchupWatchEpgEndBufferMins = kodi::addon::GetSettingInt("catchupWatchEpgEndBufferMins", 15);
  m_catchupOnlyOnFinishedProgrammes = kodi::addon::GetSettingBoolean("catchupOnlyOnFinishedProgrammes", false);

  // Advanced
  m_jellyfinUpdateIntervalHours = kodi::addon::GetSettingInt("jellyfinUpdateIntervalHours", 24);
  m_connectioncCheckTimeoutSecs = kodi::addon::GetSettingInt("connectionchecktimeout", DEFAULT_CONNECTION_CHECK_TIMEOUT_SECS);
  m_connectioncCheckIntervalSecs = kodi::addon::GetSettingInt("connectioncheckinterval", 60);
}

std::string InstanceSettings::GetJellyfinBaseUrl() const
{
  std::string input = m_jellyfinServerAddress;
  if (input.empty())
    return "";

  // Strip trailing slashes
  while (!input.empty() && input.back() == '/')
    input.pop_back();

  // Determine scheme
  std::string scheme;
  std::string remainder;
  if (input.substr(0, 8) == "https://")
  {
    scheme = "https";
    remainder = input.substr(8);
  }
  else if (input.substr(0, 7) == "http://")
  {
    scheme = "http";
    remainder = input.substr(7);
  }
  else
  {
    scheme = "http";
    remainder = input;
  }

  // Strip any path component (keep only host:port)
  auto slashPos = remainder.find('/');
  if (slashPos != std::string::npos)
    remainder = remainder.substr(0, slashPos);

  // Check if port is present
  // Handle IPv6 bracket notation [::1]:port
  bool hasPort = false;
  if (!remainder.empty() && remainder[0] == '[')
  {
    auto bracketEnd = remainder.find(']');
    if (bracketEnd != std::string::npos && bracketEnd + 1 < remainder.size() && remainder[bracketEnd + 1] == ':')
      hasPort = true;
  }
  else
  {
    hasPort = remainder.find(':') != std::string::npos;
  }

  if (!hasPort)
  {
    int defaultPort = (scheme == "https") ? 443 : 8096;
    remainder += ":" + std::to_string(defaultPort);
  }

  return scheme + "://" + remainder;
}

std::string InstanceSettings::GetJellyfinHost() const
{
  std::string baseUrl = GetJellyfinBaseUrl();
  if (baseUrl.empty())
    return "";

  // Strip scheme
  auto pos = baseUrl.find("://");
  if (pos == std::string::npos)
    return baseUrl;
  std::string hostPort = baseUrl.substr(pos + 3);

  // Strip port
  // Handle IPv6 bracket notation
  if (!hostPort.empty() && hostPort[0] == '[')
  {
    auto bracketEnd = hostPort.find(']');
    if (bracketEnd != std::string::npos)
      return hostPort.substr(0, bracketEnd + 1);
  }

  auto colonPos = hostPort.rfind(':');
  if (colonPos != std::string::npos)
    return hostPort.substr(0, colonPos);

  return hostPort;
}
