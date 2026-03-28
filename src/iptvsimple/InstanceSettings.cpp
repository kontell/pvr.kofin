/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "InstanceSettings.h"

using namespace iptvsimple;

InstanceSettings::InstanceSettings()
{
  ReadSettings();
}

void InstanceSettings::ReadSettings()
{
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

  // Advanced
  m_jellyfinUpdateIntervalHours = kodi::addon::GetSettingInt("jellyfinUpdateIntervalHours", 24);
  m_connectioncCheckTimeoutSecs = kodi::addon::GetSettingInt("connectionchecktimeout", DEFAULT_CONNECTION_CHECK_TIMEOUT_SECS);
  m_connectioncCheckIntervalSecs = kodi::addon::GetSettingInt("connectioncheckinterval", DEFAULT_CONNECTION_CHECK_INTERVAL_SECS);
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
