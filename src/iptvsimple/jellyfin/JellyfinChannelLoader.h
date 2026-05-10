/*
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include "../Channels.h"
#include "../ChannelGroups.h"
#include "../data/Channel.h"
#include "JellyfinClient.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <kodi/addon-instance/pvr/EPG.h>

namespace iptvsimple
{
namespace jellyfin
{

// Per-channel overrides extracted from M3U KofinProps. Forwarded to
// BuildDeviceProfile so a single channel can override the global
// transcode/bitrate policy.
struct ChannelOverrides
{
  std::optional<bool> forceRemux;        // kofin-force-remux
  std::optional<int>  bitrateBps;         // kofin-bitrate-limit (kbps × 1000)
  std::optional<bool> forceTranscode;     // kofin-force-transcode
  // inputstream override (PVR_STREAM_PROPERTY_INPUTSTREAM from M3U KodiProps).
  // Drives Jellyfin device-profile Container choice — inputstream.adaptive
  // can only consume fMP4 for HEVC/AV1, while ffmpegdirect prefers TS.
  std::optional<std::string> inputstream;

  bool Empty() const { return !forceRemux && !bitrateBps && !forceTranscode && !inputstream; }
  static ChannelOverrides FromChannel(const iptvsimple::data::Channel& channel);
};

class JellyfinChannelLoader
{
public:
  JellyfinChannelLoader(std::shared_ptr<JellyfinClient> client,
                         std::shared_ptr<iptvsimple::InstanceSettings> settings);

  bool LoadChannels(iptvsimple::Channels& channels, iptvsimple::ChannelGroups& channelGroups);
  PVR_ERROR LoadEpg(int channelUid, time_t start, time_t end,
                     kodi::addon::PVREPGTagsResultSet& results);
  std::string GetLiveStreamUrl(const std::string& channelId,
                                const ChannelOverrides& overrides = {});
  std::string GetItemStreamUrl(const std::string& itemId,
                                const ChannelOverrides& overrides = {});
  std::string GetRecordingStreamUrl(const std::string& recordingId);
  void CloseLiveStream();

  void SetClient(std::shared_ptr<JellyfinClient> client) { m_client = client; }

  const std::string& GetJellyfinId(int channelUid) const;
  const std::string& GetJellyfinProgramId(unsigned int epgBroadcastUid) const;
  int GetChannelUid(const std::string& jellyfinId) const;

private:
  static int GenerateUid(const std::string& str);
  static std::string FormatIso8601(time_t time);
  static time_t ParseIso8601(const std::string& dateStr);
  Json::Value BuildDeviceProfile(const ChannelOverrides& overrides);
  Json::Value BuildRecordingDeviceProfile();
  std::string PostProcessTranscodingUrl(const std::string& transcodingUrl, bool keepMaster);
  void RewriteLocalhost(std::string& url);

  // Jellyfin channel ID <-> Kodi channel UID mappings
  std::map<std::string, int> m_jellyfinIdToUid;
  std::map<int, std::string> m_uidToJellyfinId;

  // EPG broadcast UID -> Jellyfin program ID (for timer creation)
  std::map<unsigned int, std::string> m_epgUidToJellyfinProgramId;

  // Active session state (cleared on CloseLiveStream / new stream)
  std::string m_activeLiveStreamId;
  std::string m_activeItemId;
  std::string m_activeMediaSourceId;
  std::string m_activePlaySessionId;
  std::string m_activePlayMethod;         // "DirectPlay" or "Transcode"
  bool m_activeIsRecording{false};
  int m_activeMaxBitrateBps{0};            // bitrate ceiling used for current session (override-aware)

  std::shared_ptr<JellyfinClient> m_client;
  std::shared_ptr<iptvsimple::InstanceSettings> m_settings;
};

} // namespace jellyfin
} // namespace iptvsimple
