/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include <string>

#include <kodi/AddonBase.h>

namespace iptvsimple
{
  static const int DEFAULT_CONNECTION_CHECK_TIMEOUT_SECS = 10;
  static const int DEFAULT_CONNECTION_CHECK_INTERVAL_SECS = 5;

  // Bitrate table: index -> kbps (matches settings.xml options 0-15)
  static const int BITRATE_TABLE[] = {
    1000, 2000, 3000, 5000, 8000, 10000, 15000, 20000,
    25000, 30000, 40000, 60000, 80000, 100000, 120000, 0 // 0 = unlimited
  };
  static const int BITRATE_TABLE_SIZE = 16;

  class InstanceSettings
  {
  public:
    InstanceSettings();

    void ReadSettings();

    const std::string GetUserPath() const { return kodi::addon::GetUserPath(); }

    // Server
    const std::string& GetJellyfinServerAddress() const { return m_jellyfinServerAddress; }
    std::string GetJellyfinBaseUrl() const;
    std::string GetJellyfinHost() const;
    std::string GetConnectionCheckUrl() const { return GetJellyfinBaseUrl() + "/System/Ping"; }

    void SetJellyfinServerAddress(const std::string& address)
    {
      m_jellyfinServerAddress = address;
      kodi::addon::SetSettingString("jellyfinServerAddress", address);
    }

    // Account state
    bool GetIsLoggedIn() const { return m_isLoggedIn; }
    void SetIsLoggedIn(bool loggedIn)
    {
      m_isLoggedIn = loggedIn;
      kodi::addon::SetSettingBoolean("isLoggedIn", loggedIn);
    }

    const std::string& GetJellyfinServerName() const { return m_jellyfinServerName; }
    void SetJellyfinServerName(const std::string& name)
    {
      m_jellyfinServerName = name;
      kodi::addon::SetSettingString("jellyfinServerName", name);
    }

    const std::string& GetJellyfinDisplayUsername() const { return m_jellyfinDisplayUsername; }
    void SetJellyfinDisplayUsername(const std::string& username)
    {
      m_jellyfinDisplayUsername = username;
      kodi::addon::SetSettingString("jellyfinDisplayUsername", username);
    }

    // Device identity (unique per Kodi instance, generated on first run)
    const std::string& GetDeviceId() const { return m_deviceId; }

    // Authentication
    const std::string& GetJellyfinAccessToken() const { return m_jellyfinAccessToken; }
    const std::string& GetJellyfinUserId() const { return m_jellyfinUserId; }

    void SetJellyfinAccessToken(const std::string& token)
    {
      m_jellyfinAccessToken = token;
      kodi::addon::SetSettingString("jellyfinAccessToken", token);
    }
    void SetJellyfinUserId(const std::string& userId)
    {
      m_jellyfinUserId = userId;
      kodi::addon::SetSettingString("jellyfinUserId", userId);
    }

    // Transcoding
    bool GetForceTranscode() const { return m_forceTranscode; }
    bool GetTranscodeHi10P() const { return m_transcodeHi10P; }
    bool GetTranscodeHevcRext() const { return m_transcodeHevcRext; }
    int GetPreferredVideoCodec() const { return m_preferredVideoCodec; }
    int GetPreferredAudioCodec() const { return m_preferredAudioCodec; }
    int GetMaxStreamingBitrateKbps() const
    {
      if (m_maxStreamingBitrate >= 0 && m_maxStreamingBitrate < BITRATE_TABLE_SIZE)
        return BITRATE_TABLE[m_maxStreamingBitrate];
      return 0; // unlimited
    }
    int GetMaxBitrateBps() const
    {
      int kbps = GetMaxStreamingBitrateKbps();
      return (kbps > 0) ? kbps * 1000 : 1000000000;
    }

    // Input stream: 0 = ffmpegdirect, 1 = adaptive, 2 = kodi internal
    int GetInputStream() const { return m_inputStream; }
    bool GetTimeshiftEnabled() const { return m_timeshiftEnabled; }

    // Catchup
    bool CatchupEnabled() const { return m_catchupEnabled; }
    const std::string& GetCatchupM3UPath() const { return m_catchupM3UPath; }
    const std::string& GetCatchupQueryFormat() const { return m_catchupQueryFormat; }
    int GetCatchupDays() const { return m_catchupDays; }
    int GetAllChannelsCatchupMode() const { return m_allChannelsCatchupMode; }
    bool CatchupPlayEpgAsLive() const { return m_catchupPlayEpgAsLive; }
    int GetCatchupWatchEpgBeginBufferSecs() const { return m_catchupWatchEpgBeginBufferMins * 60; }
    int GetCatchupWatchEpgEndBufferSecs() const { return m_catchupWatchEpgEndBufferMins * 60; }
    bool CatchupOnlyOnFinishedProgrammes() const { return m_catchupOnlyOnFinishedProgrammes; }
    float GetCatchupCorrection() const { return m_catchupCorrection; }

    // Advanced
    int GetJellyfinUpdateIntervalHours() const { return m_jellyfinUpdateIntervalHours; }
    int GetConnectioncCheckTimeoutSecs() const { return m_connectioncCheckTimeoutSecs; }
    int GetConnectioncCheckIntervalSecs() const { return m_connectioncCheckIntervalSecs; }

    // Codec name helpers for device profile
    std::string GetPreferredVideoCodecName() const
    {
      switch (m_preferredVideoCodec)
      {
        case 1: return "hevc";
        case 2: return "av1";
        default: return "h264";
      }
    }

    std::string GetPreferredAudioCodecName() const
    {
      switch (m_preferredAudioCodec)
      {
        case 1: return "ac3";
        case 2: return "mp3";
        case 3: return "opus";
        default: return "aac";
      }
    }

  private:
    // Server
    std::string m_jellyfinServerAddress;

    // Account state
    bool m_isLoggedIn = false;
    std::string m_jellyfinServerName;
    std::string m_jellyfinDisplayUsername;

    // Device identity
    std::string m_deviceId;

    // Authentication
    std::string m_jellyfinAccessToken;
    std::string m_jellyfinUserId;

    // Transcoding
    bool m_forceTranscode = false;
    bool m_transcodeHi10P = true;
    bool m_transcodeHevcRext = true;
    int m_preferredVideoCodec = 0;  // 0=H264, 1=H265, 2=AV1
    int m_preferredAudioCodec = 0;  // 0=AAC, 1=AC3, 2=MP3, 3=Opus
    int m_maxStreamingBitrate = 15; // index into BITRATE_TABLE (15=unlimited)

    // Input stream
    int m_inputStream = 0;  // 0=ffmpegdirect, 1=adaptive, 2=kodi internal
    bool m_timeshiftEnabled = true;

    // Catchup
    bool m_catchupEnabled = false;
    std::string m_catchupM3UPath;
    std::string m_catchupQueryFormat;
    int m_catchupDays = 5;
    int m_allChannelsCatchupMode = 0;  // 0=disabled, 3=shift
    bool m_catchupPlayEpgAsLive = false;
    int m_catchupWatchEpgBeginBufferMins = 5;
    int m_catchupWatchEpgEndBufferMins = 15;
    bool m_catchupOnlyOnFinishedProgrammes = false;
    float m_catchupCorrection = 0.0f;

    // Advanced
    int m_jellyfinUpdateIntervalHours = 24;
    int m_connectioncCheckTimeoutSecs = DEFAULT_CONNECTION_CHECK_TIMEOUT_SECS;
    int m_connectioncCheckIntervalSecs = DEFAULT_CONNECTION_CHECK_INTERVAL_SECS;
  };
} //namespace iptvsimple
