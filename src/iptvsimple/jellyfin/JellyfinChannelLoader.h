/*
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include "../Channels.h"
#include "../ChannelGroups.h"
#include "JellyfinClient.h"

#include <atomic>
#include <ctime>
#include <map>
#include <memory>
#include <string>

#include <kodi/addon-instance/pvr/EPG.h>

namespace iptvsimple
{
namespace jellyfin
{

class JellyfinChannelLoader
{
public:
  JellyfinChannelLoader(std::shared_ptr<JellyfinClient> client,
                         std::shared_ptr<iptvsimple::InstanceSettings> settings);

  bool LoadChannels(iptvsimple::Channels& channels, iptvsimple::ChannelGroups& channelGroups);
  PVR_ERROR LoadEpg(int channelUid, time_t start, time_t end,
                     kodi::addon::PVREPGTagsResultSet& results);
  std::string GetLiveStreamUrl(const std::string& channelId);
  std::string GetItemStreamUrl(const std::string& itemId);
  std::string GetRecordingStreamUrl(const std::string& recordingId);
  void CloseLiveStream();
  // Returns true if there's an active session AND it's been open long enough
  // for Kodi to have entered fullscreen video (grace period for window ID check).
  bool HasActiveSession() const
  {
    return !m_activePlaySessionId.empty()
      && (std::time(nullptr) - m_sessionStartTime) >= 5;
  }
  void ReportProgress();

  void SetClient(std::shared_ptr<JellyfinClient> client) { m_client = client; }

  const std::string& GetJellyfinId(int channelUid) const;
  const std::string& GetJellyfinProgramId(unsigned int epgBroadcastUid) const;

private:
  static int GenerateUid(const std::string& str);
  static std::string FormatIso8601(time_t time);
  static time_t ParseIso8601(const std::string& dateStr);
  Json::Value BuildDeviceProfile();
  Json::Value BuildRecordingDeviceProfile();
  std::string PostProcessTranscodingUrl(const std::string& transcodingUrl);
  void RewriteLocalhost(std::string& url);

  // Jellyfin channel ID <-> Kodi channel UID mappings
  std::map<std::string, int> m_jellyfinIdToUid;
  std::map<int, std::string> m_uidToJellyfinId;

  // EPG broadcast UID -> Jellyfin program ID (for timer creation)
  std::map<unsigned int, std::string> m_epgUidToJellyfinProgramId;

  // Active session for reporting and cleanup
  std::string m_activeLiveStreamId;
  std::string m_activeItemId;
  std::string m_activeMediaSourceId;
  std::string m_activePlaySessionId;
  std::string m_activePlayMethod;         // "DirectPlay" or "Transcode"
  std::atomic<uint32_t> m_sessionGen{0};  // generation counter — deferred threads check before sending
  time_t m_sessionStartTime{0};           // when session was opened — grace period for stop detection

  void ScheduleDeferredPlayingReport();
  Json::Value BuildSessionBody() const;

  std::shared_ptr<JellyfinClient> m_client;
  std::shared_ptr<iptvsimple::InstanceSettings> m_settings;
};

} // namespace jellyfin
} // namespace iptvsimple
