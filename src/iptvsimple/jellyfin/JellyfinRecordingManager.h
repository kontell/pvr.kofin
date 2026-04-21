/*
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include "JellyfinClient.h"
#include "JellyfinChannelLoader.h"

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <kodi/addon-instance/pvr/Timers.h>
#include <kodi/addon-instance/pvr/Recordings.h>

namespace iptvsimple
{
namespace jellyfin
{

// Timer type IDs (adapted from pvr.hts pattern - simplified for Jellyfin)
enum TimerTypeId
{
  TIMER_ONCE_EPG = 1,                // One-shot recording from EPG programme
  TIMER_ONCE_CREATED_BY_SERIES = 2,  // Child of series timer (read-only)
  TIMER_SERIES = 3,                  // Series recording rule
  TIMER_ONCE_MANUAL = 4              // Manual recording (channel + time, no EPG)
};

class JellyfinRecordingManager
{
public:
  JellyfinRecordingManager(std::shared_ptr<JellyfinClient> client,
                            std::shared_ptr<JellyfinChannelLoader> channelLoader,
                            std::shared_ptr<iptvsimple::InstanceSettings> settings);

  // Timer types
  PVR_ERROR GetTimerTypes(std::vector<kodi::addon::PVRTimerType>& types);

  // Timers
  int GetTimersAmount();
  PVR_ERROR GetTimers(kodi::addon::PVRTimersResultSet& results);
  PVR_ERROR AddTimer(const kodi::addon::PVRTimer& timer);
  PVR_ERROR DeleteTimer(const kodi::addon::PVRTimer& timer, bool forceDelete);
  PVR_ERROR UpdateTimer(const kodi::addon::PVRTimer& timer);

  // Recordings
  int GetRecordingsAmount();
  PVR_ERROR GetRecordings(kodi::addon::PVRRecordingsResultSet& results);
  PVR_ERROR DeleteRecording(const kodi::addon::PVRRecording& recording);
  PVR_ERROR GetRecordingStreamProperties(const kodi::addon::PVRRecording& recording,
                                          std::vector<kodi::addon::PVRStreamProperty>& properties);
  PVR_ERROR SetRecordingPlayCount(const kodi::addon::PVRRecording& recording, int count);
  PVR_ERROR SetRecordingLastPlayedPosition(const kodi::addon::PVRRecording& recording, int lastplayedposition);
  PVR_ERROR GetRecordingLastPlayedPosition(const kodi::addon::PVRRecording& recording, int& position);

  void SetClient(std::shared_ptr<JellyfinClient> client) { m_client = client; }

  // Reload cached data from Jellyfin
  void Reload();

private:
  PVR_ERROR LoadTimers();
  PVR_ERROR LoadSeriesTimers();
  PVR_ERROR LoadRecordings();

  static int GenerateUid(const std::string& str);
  static time_t ParseIso8601(const std::string& dateStr);
  static std::string FormatIso8601(time_t time);

  // Jellyfin ID <-> Kodi client index mappings for timers/recordings
  std::map<int, std::string> m_timerUidToId;
  std::map<int, std::string> m_seriesTimerUidToId;
  std::map<int, std::string> m_recordingUidToId;

  // Jellyfin IDs of recordings that are currently in progress
  std::set<std::string> m_inProgressRecordingIds;

  // Timer name -> ProgramId and ChannelUid mappings (for EPG-linking in-progress recordings)
  std::map<std::string, std::string> m_timerNameToProgramId;
  std::map<std::string, int> m_timerNameToChannelUid;

  // Cached Kodi PVR objects
  std::vector<kodi::addon::PVRTimer> m_timers;
  std::vector<kodi::addon::PVRTimer> m_seriesTimers;
  std::vector<kodi::addon::PVRRecording> m_recordings;

  mutable std::mutex m_mutex;

  // Track recent SetRecordingPlayCount calls to distinguish
  // "mark watched/unwatched" (PlayCount then Position=0) from
  // "playback start" (PlayCount only, no Position=0)
  std::map<std::string, std::pair<int, std::chrono::steady_clock::time_point>> m_recentPlayCountCalls;

  std::shared_ptr<JellyfinClient> m_client;
  std::shared_ptr<JellyfinChannelLoader> m_channelLoader;
  std::shared_ptr<iptvsimple::InstanceSettings> m_settings;
};

} // namespace jellyfin
} // namespace iptvsimple
