/*
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "JellyfinRecordingManager.h"

#include "../utilities/JsonUtils.h"
#include "../utilities/Logger.h"
#include <kodi/General.h>
#include "../utilities/TimeUtils.h"
#include "../utilities/UidUtils.h"
#include "../utilities/WebUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>

using namespace iptvsimple;
using namespace iptvsimple::jellyfin;
using namespace iptvsimple::utilities;

namespace
{
// A recording that leaves the in-progress state more than this many seconds
// before its scheduled end is reported as stopped prematurely. The margin
// absorbs the 60s poll cadence and end-time jitter around a natural finish.
constexpr time_t PREMATURE_STOP_MARGIN_SECS = 120;

// FNV-1a, for the model fingerprints.
uint64_t HashString(const std::string& str)
{
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char c : str)
  {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}

void HashCombine(uint64_t& hash, uint64_t value)
{
  hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
}
} // unnamed namespace

JellyfinRecordingManager::JellyfinRecordingManager(
    std::shared_ptr<JellyfinClient> client,
    std::shared_ptr<JellyfinChannelLoader> channelLoader,
    std::shared_ptr<iptvsimple::InstanceSettings> settings)
  : m_client(client), m_channelLoader(channelLoader), m_settings(settings)
{
}

/***************************************************************************
 * Timer Types (adapted from pvr.hts Tvheadend.cpp GetTimerTypes pattern)
 **************************************************************************/

PVR_ERROR JellyfinRecordingManager::GetTimerTypes(std::vector<kodi::addon::PVRTimerType>& types)
{
  // Type 4: Manual recording (channel + time, no EPG) — listed first so it is the default
  {
    kodi::addon::PVRTimerType type;
    type.SetId(TIMER_ONCE_MANUAL);
    type.SetDescription("Manual recording");
    type.SetAttributes(
      PVR_TIMER_TYPE_IS_MANUAL |
      PVR_TIMER_TYPE_SUPPORTS_CHANNELS |
      PVR_TIMER_TYPE_SUPPORTS_START_TIME |
      PVR_TIMER_TYPE_SUPPORTS_END_TIME |
      PVR_TIMER_TYPE_SUPPORTS_START_MARGIN |
      PVR_TIMER_TYPE_SUPPORTS_END_MARGIN
    );
    types.emplace_back(type);
  }

  // Type 1: One-shot EPG recording
  {
    kodi::addon::PVRTimerType type;
    type.SetId(TIMER_ONCE_EPG);
    type.SetDescription("Record once (EPG)");
    type.SetAttributes(
      PVR_TIMER_TYPE_REQUIRES_EPG_TAG_ON_CREATE |
      PVR_TIMER_TYPE_SUPPORTS_CHANNELS |
      PVR_TIMER_TYPE_SUPPORTS_START_TIME |
      PVR_TIMER_TYPE_SUPPORTS_END_TIME |
      PVR_TIMER_TYPE_SUPPORTS_START_MARGIN |
      PVR_TIMER_TYPE_SUPPORTS_END_MARGIN
    );
    types.emplace_back(type);
  }

  // Type 2: One-shot created by series rule (read-only child)
  {
    kodi::addon::PVRTimerType type;
    type.SetId(TIMER_ONCE_CREATED_BY_SERIES);
    type.SetDescription("Record once (created by series rule)");
    type.SetAttributes(
      PVR_TIMER_TYPE_IS_READONLY |
      PVR_TIMER_TYPE_FORBIDS_NEW_INSTANCES |
      PVR_TIMER_TYPE_SUPPORTS_CHANNELS |
      PVR_TIMER_TYPE_SUPPORTS_START_TIME |
      PVR_TIMER_TYPE_SUPPORTS_END_TIME |
      PVR_TIMER_TYPE_SUPPORTS_READONLY_DELETE
    );
    types.emplace_back(type);
  }

  // Type 3: Series recording rule
  {
    kodi::addon::PVRTimerType type;
    type.SetId(TIMER_SERIES);
    type.SetDescription("Record series");
    type.SetAttributes(
      PVR_TIMER_TYPE_IS_REPEATING |
      PVR_TIMER_TYPE_REQUIRES_EPG_SERIES_ON_CREATE |
      PVR_TIMER_TYPE_SUPPORTS_CHANNELS |
      PVR_TIMER_TYPE_SUPPORTS_ANY_CHANNEL |
      PVR_TIMER_TYPE_SUPPORTS_RECORD_ONLY_NEW_EPISODES |
      PVR_TIMER_TYPE_SUPPORTS_WEEKDAYS
    );
    types.emplace_back(type);
  }

  Logger::Log(LEVEL_INFO, "%s - Registered %d timer types", __FUNCTION__,
              static_cast<int>(types.size()));

  return PVR_ERROR_NO_ERROR;
}

/***************************************************************************
 * Timers
 **************************************************************************/

int JellyfinRecordingManager::GetTimersAmount()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return static_cast<int>(m_timers.size() + m_seriesTimers.size());
}

PVR_ERROR JellyfinRecordingManager::GetTimers(kodi::addon::PVRTimersResultSet& results)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  for (const auto& timer : m_timers)
    results.Add(timer);

  for (const auto& timer : m_seriesTimers)
    results.Add(timer);

  Logger::Log(LEVEL_DEBUG, "%s - %d timers + %d series timers",
              __FUNCTION__, static_cast<int>(m_timers.size()),
              static_cast<int>(m_seriesTimers.size()));

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR JellyfinRecordingManager::AddTimer(const kodi::addon::PVRTimer& timer)
{
  // Exception firewall: AddTimer runs on a detached worker thread, where an
  // escaped jsoncpp exception means std::terminate for the whole process.
  try
  {
    return AddTimerInternal(timer);
  }
  catch (const std::exception& e)
  {
    Logger::Log(LEVEL_ERROR, "%s - Exception creating timer: %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

PVR_ERROR JellyfinRecordingManager::AddTimerInternal(const kodi::addon::PVRTimer& timer)
{
  if (timer.GetTimerType() == TIMER_ONCE_EPG)
  {
    // Look up the Jellyfin program ID from the EPG broadcast UID
    std::string programId = m_channelLoader->GetJellyfinProgramId(timer.GetEPGUid());

    // Fallback: if the map is empty (Kodi used cached EPG after restart),
    // query Jellyfin for programs on this channel around the timer's start time
    if (programId.empty() && timer.GetStartTime() > 0)
    {
      Logger::Log(LEVEL_INFO, "%s - EPG map miss for UID %d, querying Jellyfin by channel+time",
                  __FUNCTION__, timer.GetEPGUid());

      const std::string channelJfId = m_channelLoader->GetJellyfinId(timer.GetClientChannelUid());
      if (!channelJfId.empty())
      {
        // Query programs in a 1-minute window around the start time
        const std::string startIso = FormatIso8601(timer.GetStartTime());
        const std::string endIso = FormatIso8601(timer.GetStartTime() + 60);
        const std::string endpoint = "/LiveTv/Programs?ChannelIds=" + channelJfId
          + "&MinStartDate=" + WebUtils::UrlEncode(startIso)
          + "&MaxStartDate=" + WebUtils::UrlEncode(endIso)
          + "&Limit=1";

        Json::Value response = m_client->SendGet(endpoint);
        if (!response.isNull() && response.isMember("Items") && !response["Items"].empty())
        {
          programId = response["Items"][0]["Id"].asString();
          Logger::Log(LEVEL_INFO, "%s - Found program %s via channel+time fallback",
                      __FUNCTION__, programId.c_str());
        }
      }
    }

    if (programId.empty())
    {
      Logger::Log(LEVEL_ERROR, "%s - No Jellyfin program ID for EPG UID %d",
                  __FUNCTION__, timer.GetEPGUid());
      return PVR_ERROR_INVALID_PARAMETERS;
    }

    // Get timer defaults from Jellyfin for this program
    Json::Value defaults = m_client->SendGet("/LiveTv/Timers/Defaults?programId=" + programId);
    if (defaults.isNull())
    {
      Logger::Log(LEVEL_ERROR, "%s - Failed to get timer defaults for program %s",
                  __FUNCTION__, programId.c_str());
      return PVR_ERROR_SERVER_ERROR;
    }

    // Apply user padding
    defaults["PrePaddingSeconds"] = static_cast<int>(timer.GetMarginStart()) * 60;
    defaults["IsPrePaddingRequired"] = timer.GetMarginStart() > 0;
    defaults["PostPaddingSeconds"] = static_cast<int>(timer.GetMarginEnd()) * 60;
    defaults["IsPostPaddingRequired"] = timer.GetMarginEnd() > 0;

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    const std::string bodyStr = Json::writeString(writer, defaults);

    Logger::Log(LEVEL_INFO, "%s - Creating timer: %s (program %s)",
                __FUNCTION__, timer.GetTitle().c_str(), programId.c_str());

    if (!m_client->SendPostExpectSuccess("/LiveTv/Timers", bodyStr))
    {
      Logger::Log(LEVEL_ERROR, "%s - Server rejected timer creation for program %s",
                  __FUNCTION__, programId.c_str());
      return PVR_ERROR_SERVER_ERROR;
    }

    Reload();
    return PVR_ERROR_NO_ERROR;
  }
  else if (timer.GetTimerType() == TIMER_SERIES)
  {
    // Look up Jellyfin program ID from EPG — same as single timers
    std::string programId = m_channelLoader->GetJellyfinProgramId(timer.GetEPGUid());

    if (programId.empty() && timer.GetStartTime() > 0)
    {
      Logger::Log(LEVEL_INFO, "%s - Series timer EPG map miss for UID %d, querying by channel+time",
                  __FUNCTION__, timer.GetEPGUid());

      const std::string channelJfId = m_channelLoader->GetJellyfinId(timer.GetClientChannelUid());
      if (!channelJfId.empty())
      {
        const std::string startIso = FormatIso8601(timer.GetStartTime());
        const std::string endIso = FormatIso8601(timer.GetStartTime() + 60);
        const std::string endpoint = "/LiveTv/Programs?ChannelIds=" + channelJfId
          + "&MinStartDate=" + WebUtils::UrlEncode(startIso)
          + "&MaxStartDate=" + WebUtils::UrlEncode(endIso)
          + "&Limit=1";

        Json::Value response = m_client->SendGet(endpoint);
        if (!response.isNull() && response.isMember("Items") && !response["Items"].empty())
        {
          programId = response["Items"][0]["Id"].asString();
          Logger::Log(LEVEL_INFO, "%s - Found program %s via channel+time fallback",
                      __FUNCTION__, programId.c_str());
        }
      }
    }

    if (programId.empty())
    {
      Logger::Log(LEVEL_ERROR, "%s - No Jellyfin program ID for series timer EPG UID %d",
                  __FUNCTION__, timer.GetEPGUid());
      return PVR_ERROR_INVALID_PARAMETERS;
    }

    // Get timer defaults for this program — gives us the full template
    Json::Value defaults = m_client->SendGet("/LiveTv/Timers/Defaults?programId=" + programId);
    if (defaults.isNull())
    {
      Logger::Log(LEVEL_ERROR, "%s - Failed to get timer defaults for program %s",
                  __FUNCTION__, programId.c_str());
      return PVR_ERROR_SERVER_ERROR;
    }

    // Override with series-specific settings
    defaults["RecordAnyTime"] = true;
    defaults["RecordAnyChannel"] = (timer.GetClientChannelUid() == PVR_TIMER_ANY_CHANNEL);
    defaults["RecordNewOnly"] = timer.GetPreventDuplicateEpisodes() > 0;

    // Weekdays
    Json::Value days(Json::arrayValue);
    unsigned int weekdays = timer.GetWeekdays();
    if (weekdays & PVR_WEEKDAY_MONDAY) days.append("Monday");
    if (weekdays & PVR_WEEKDAY_TUESDAY) days.append("Tuesday");
    if (weekdays & PVR_WEEKDAY_WEDNESDAY) days.append("Wednesday");
    if (weekdays & PVR_WEEKDAY_THURSDAY) days.append("Thursday");
    if (weekdays & PVR_WEEKDAY_FRIDAY) days.append("Friday");
    if (weekdays & PVR_WEEKDAY_SATURDAY) days.append("Saturday");
    if (weekdays & PVR_WEEKDAY_SUNDAY) days.append("Sunday");
    if (!days.empty())
      defaults["Days"] = days;

    // Padding
    defaults["PrePaddingSeconds"] = static_cast<int>(timer.GetMarginStart()) * 60;
    defaults["IsPrePaddingRequired"] = timer.GetMarginStart() > 0;
    defaults["PostPaddingSeconds"] = static_cast<int>(timer.GetMarginEnd()) * 60;
    defaults["IsPostPaddingRequired"] = timer.GetMarginEnd() > 0;

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    const std::string bodyStr = Json::writeString(writer, defaults);

    Logger::Log(LEVEL_INFO, "%s - Creating series timer: %s (program %s)",
                __FUNCTION__, timer.GetTitle().c_str(), programId.c_str());

    if (!m_client->SendPostExpectSuccess("/LiveTv/SeriesTimers", bodyStr))
    {
      Logger::Log(LEVEL_ERROR, "%s - Server rejected series timer creation for program %s",
                  __FUNCTION__, programId.c_str());
      return PVR_ERROR_SERVER_ERROR;
    }

    Reload();
    return PVR_ERROR_NO_ERROR;
  }
  else if (timer.GetTimerType() == TIMER_ONCE_MANUAL)
  {
    // Manual recording — channel + time range, no EPG program ID.
    // Get timer defaults (without programId) and fill in from Kodi's timer.
    const std::string channelJfId = m_channelLoader->GetJellyfinId(timer.GetClientChannelUid());
    if (channelJfId.empty())
    {
      Logger::Log(LEVEL_ERROR, "%s - No Jellyfin channel ID for channel UID %d",
                  __FUNCTION__, timer.GetClientChannelUid());
      return PVR_ERROR_INVALID_PARAMETERS;
    }

    Json::Value defaults = m_client->SendGet("/LiveTv/Timers/Defaults");
    if (defaults.isNull())
    {
      Logger::Log(LEVEL_ERROR, "%s - Failed to get timer defaults", __FUNCTION__);
      return PVR_ERROR_SERVER_ERROR;
    }

    defaults["ChannelId"] = channelJfId;

    // GetStartTime() returns 0 for instant records (meaning "now"), and may
    // return seconds-since-midnight when only the time is edited in the
    // dialog (Kodi bug). Detect and fix both cases.
    time_t startTime = timer.GetStartTime();
    time_t endTime = timer.GetEndTime();
    time_t now = std::time(nullptr);

    if (startTime <= 0)
      startTime = now;
    else if (startTime < 86400) // Looks like time-of-day, not a real time_t
    {
      std::tm today = SafeGmtime(now);
      today.tm_hour = static_cast<int>(startTime / 3600);
      today.tm_min = static_cast<int>((startTime % 3600) / 60);
      today.tm_sec = static_cast<int>(startTime % 60);
      startTime = SafeTimegm(&today);
    }

    if (endTime <= 0)
      endTime = now + 7200; // Default 2 hours
    else if (endTime < 86400)
    {
      std::tm today = SafeGmtime(now);
      today.tm_hour = static_cast<int>(endTime / 3600);
      today.tm_min = static_cast<int>((endTime % 3600) / 60);
      today.tm_sec = static_cast<int>(endTime % 60);
      endTime = SafeTimegm(&today);
    }

    defaults["StartDate"] = FormatIso8601(startTime);
    defaults["EndDate"] = FormatIso8601(endTime);
    defaults["Name"] = timer.GetTitle().empty() ? "Manual Recording" : timer.GetTitle();
    defaults["PrePaddingSeconds"] = static_cast<int>(timer.GetMarginStart()) * 60;
    defaults["IsPrePaddingRequired"] = timer.GetMarginStart() > 0;
    defaults["PostPaddingSeconds"] = static_cast<int>(timer.GetMarginEnd()) * 60;
    defaults["IsPostPaddingRequired"] = timer.GetMarginEnd() > 0;

    // Do NOT set ProgramId — leave it empty so Jellyfin's RefreshTimers
    // won't delete this timer when it can't find a matching programme.

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    const std::string bodyStr = Json::writeString(writer, defaults);

    Logger::Log(LEVEL_INFO, "%s - Creating manual timer: %s on channel %s (%s - %s)",
                __FUNCTION__, defaults["Name"].asString().c_str(), channelJfId.c_str(),
                defaults["StartDate"].asString().c_str(), defaults["EndDate"].asString().c_str());

    if (!m_client->SendPostExpectSuccess("/LiveTv/Timers", bodyStr))
    {
      Logger::Log(LEVEL_ERROR, "%s - Server rejected manual timer creation on channel %s",
                  __FUNCTION__, channelJfId.c_str());
      return PVR_ERROR_SERVER_ERROR;
    }

    Reload();
    return PVR_ERROR_NO_ERROR;
  }

  return PVR_ERROR_INVALID_PARAMETERS;
}

PVR_ERROR JellyfinRecordingManager::DeleteTimer(const kodi::addon::PVRTimer& timer, bool forceDelete)
{
  int clientIndex = timer.GetClientIndex();
  std::string jellyfinId;
  bool isSeries = (timer.GetTimerType() == TIMER_SERIES);

  // Look up Jellyfin ID under lock, copy it out
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (isSeries)
    {
      auto it = m_seriesTimerUidToId.find(clientIndex);
      if (it == m_seriesTimerUidToId.end())
      {
        Logger::Log(LEVEL_ERROR, "%s - Series timer UID %d not found", __FUNCTION__, clientIndex);
        return PVR_ERROR_INVALID_PARAMETERS;
      }
      jellyfinId = it->second;
    }
    else
    {
      auto it = m_timerUidToId.find(clientIndex);
      if (it == m_timerUidToId.end())
      {
        Logger::Log(LEVEL_ERROR, "%s - Timer UID %d not found", __FUNCTION__, clientIndex);
        return PVR_ERROR_INVALID_PARAMETERS;
      }
      jellyfinId = it->second;
    }
  }

  // Network call outside lock
  const std::string endpoint = isSeries
    ? "/LiveTv/SeriesTimers/" + jellyfinId
    : "/LiveTv/Timers/" + jellyfinId;

  Logger::Log(LEVEL_INFO, "%s - Deleting %stimer %s", __FUNCTION__,
              isSeries ? "series " : "", jellyfinId.c_str());

  // The caller runs this off Kodi's main thread; the server-side operation
  // (stopping a recording) can take several seconds. Reload afterwards so the
  // model reflects the deletion before the caller triggers Kodi UI updates.
  if (!m_client->SendDelete(endpoint))
  {
    // Surface the failure: returning NO_ERROR here made a rejected delete
    // silently "succeed" and the timer reappear on the next poll.
    Logger::Log(LEVEL_ERROR, "%s - Failed to delete %stimer %s", __FUNCTION__,
                isSeries ? "series " : "", jellyfinId.c_str());
    return PVR_ERROR_SERVER_ERROR;
  }
  Reload();

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR JellyfinRecordingManager::UpdateTimer(const kodi::addon::PVRTimer& timer)
{
  // Jellyfin's DefaultLiveTvService.UpdateTimerAsync silently ignores updates
  // to in-progress timers (checks GetActiveRecordingPath != null). Non-active
  // timer updates are accepted but the API returns 204 without applying
  // padding changes reliably. Disabled until Jellyfin fixes this server-side.
  // See: jellyfin-allow-padding-update-on-inprogress-timers feature request.

  Logger::Log(LEVEL_WARNING, "%s - Timer edits not supported by Jellyfin", __FUNCTION__);
  kodi::QueueNotification(QUEUE_ERROR, "Kofin PVR", "Timer edits not supported by Jellyfin");
  return PVR_ERROR_REJECTED;
}

/***************************************************************************
 * Recordings
 **************************************************************************/

int JellyfinRecordingManager::GetRecordingsAmount()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return static_cast<int>(m_recordings.size());
}

PVR_ERROR JellyfinRecordingManager::GetRecordings(kodi::addon::PVRRecordingsResultSet& results)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  for (const auto& recording : m_recordings)
    results.Add(recording);

  Logger::Log(LEVEL_DEBUG, "%s - %d recordings", __FUNCTION__,
              static_cast<int>(m_recordings.size()));

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR JellyfinRecordingManager::DeleteRecording(const kodi::addon::PVRRecording& recording)
{
  const std::string recordingId = recording.GetRecordingId();

  Logger::Log(LEVEL_INFO, "%s - Deleting recording %s", __FUNCTION__, recordingId.c_str());

  // Try /Items/{id} first (more reliable), fall back to /LiveTv/Recordings/{id}
  if (!m_client->SendDelete("/Items/" + recordingId))
  {
    if (!m_client->SendDelete("/LiveTv/Recordings/" + recordingId))
      return PVR_ERROR_SERVER_ERROR;
  }

  Reload();
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR JellyfinRecordingManager::GetRecordingStreamProperties(
    const kodi::addon::PVRRecording& recording,
    std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  const std::string recordingId = recording.GetRecordingId();

  // m_inProgressRecordingIds and m_recordings are cleared/rebuilt under
  // m_mutex by the 60 s Reload() poll — read both under the same lock
  // (the count() was previously unlocked: UB during a concurrent rebuild).
  bool inProgress = false;
  int channelUid = 0;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    inProgress = m_inProgressRecordingIds.count(recordingId) > 0;
    if (inProgress)
    {
      for (const auto& rec : m_recordings)
      {
        if (rec.GetRecordingId() == recordingId)
        {
          channelUid = rec.GetChannelUid();
          break;
        }
      }
    }
  }

  iptvsimple::jellyfin::ChannelOverrides overrides;
  if (inProgress && m_channels)
  {
    if (channelUid > 0)
    {
      iptvsimple::data::Channel channel(m_settings);
      if (m_channels->GetChannel(channelUid, channel))
      {
        auto channelOverrides = ChannelOverrides::FromChannel(channel);
        overrides.forceTranscode = channelOverrides.forceTranscode;
        overrides.bitrateBps = channelOverrides.bitrateBps;
      }
    }
  }

  const std::string streamUrl = m_channelLoader->GetRecordingStreamUrl(
      recordingId, inProgress, overrides);
  if (streamUrl.empty())
  {
    Logger::Log(LEVEL_ERROR, "%s - Failed to get recording stream URL for %s",
                __FUNCTION__, recordingId.c_str());
    return PVR_ERROR_SERVER_ERROR;
  }

  // DEBUG, not INFO: keep resolved stream URLs out of default kodi.log output.
  Logger::Log(LEVEL_DEBUG, "%s - Recording stream URL (id=%s, inProgress=%d): %s",
              __FUNCTION__, recordingId.c_str(), inProgress ? 1 : 0,
              WebUtils::RedactUrl(streamUrl).c_str());

  properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, streamUrl);
  properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, inProgress ? "true" : "false");

  if (inProgress && m_settings->GetInProgressInputStream() == 0)
  {
    properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.adaptive");
    properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, "application/vnd.apple.mpegurl");
    properties.emplace_back("inputstream.adaptive.manifest_type", "hls");
    properties.emplace_back("inputstream.adaptive.play_timeshift_buffer", "true");
  }

  return PVR_ERROR_NO_ERROR;
}

/***************************************************************************
 * Data Loading
 **************************************************************************/

int JellyfinRecordingManager::Reload()
{
  LoadTimers();
  LoadSeriesTimers();
  LoadRecordings();

  // The red "recording" indicator in the guide tracks the timer state. Jellyfin
  // reports a just-created timer as InProgress immediately, but its recording
  // isn't playable from the EPG for a couple of seconds (it materialises after
  // the timer — see IptvSimple::AddTimer). Until the recording exists in the
  // model, present EPG-linked in-progress timers as Scheduled (clock icon) so
  // the red dot only appears once the recording is actually playable. Runs
  // after LoadRecordings so the recordings snapshot is current; manual/non-EPG
  // timers (no EPGUid) are left as RECORDING.
  std::lock_guard<std::mutex> lock(m_mutex);
  for (auto& timer : m_timers)
  {
    if (timer.GetState() == PVR_TIMER_STATE_RECORDING && timer.GetEPGUid() != 0 &&
        !HasRecordingForEpgLocked(timer.GetEPGUid(), timer.GetClientChannelUid()))
    {
      timer.SetState(PVR_TIMER_STATE_SCHEDULED);
    }
  }

  // Fingerprint the rebuilt model. Per-item hashes are summed, not chained, so
  // a reordered server response doesn't read as a change; the item count is
  // folded in afterwards so an add plus a remove can't cancel out. Runs after
  // the state fixup above, which is itself part of what Kodi renders.
  uint64_t timersFingerprint = 0;
  for (const auto& timer : m_timers)
    timersFingerprint += FingerprintTimer(timer);
  for (const auto& timer : m_seriesTimers)
    timersFingerprint += FingerprintTimer(timer);
  HashCombine(timersFingerprint, m_timers.size());
  HashCombine(timersFingerprint, m_seriesTimers.size());

  uint64_t recordingsFingerprint = 0;
  for (const auto& recording : m_recordings)
    recordingsFingerprint += FingerprintRecording(recording);
  HashCombine(recordingsFingerprint, m_recordings.size());

  // First reload after a (re)connect reports everything as changed: Kodi's view
  // of the model is unknown at that point, so it has to be refreshed once.
  int changed = RELOAD_CHANGE_NONE;
  if (!m_fingerprintsValid || timersFingerprint != m_timersFingerprint)
    changed |= RELOAD_CHANGE_TIMERS;
  if (!m_fingerprintsValid || recordingsFingerprint != m_recordingsFingerprint)
    changed |= RELOAD_CHANGE_RECORDINGS;

  m_timersFingerprint = timersFingerprint;
  m_recordingsFingerprint = recordingsFingerprint;
  m_fingerprintsValid = true;

  return changed;
}

uint64_t JellyfinRecordingManager::FingerprintTimer(const kodi::addon::PVRTimer& timer)
{
  uint64_t hash = 0;
  HashCombine(hash, timer.GetClientIndex());
  HashCombine(hash, timer.GetParentClientIndex());
  HashCombine(hash, timer.GetTimerType());
  HashCombine(hash, static_cast<uint64_t>(timer.GetState()));
  HashCombine(hash, HashString(timer.GetTitle()));
  HashCombine(hash, HashString(timer.GetSummary()));
  HashCombine(hash, static_cast<uint64_t>(timer.GetClientChannelUid()));
  HashCombine(hash, static_cast<uint64_t>(timer.GetStartTime()));
  HashCombine(hash, static_cast<uint64_t>(timer.GetEndTime()));
  HashCombine(hash, timer.GetMarginStart());
  HashCombine(hash, timer.GetMarginEnd());
  HashCombine(hash, timer.GetEPGUid());
  HashCombine(hash, timer.GetWeekdays());
  HashCombine(hash, timer.GetPreventDuplicateEpisodes());
  return hash;
}

uint64_t JellyfinRecordingManager::FingerprintRecording(const kodi::addon::PVRRecording& recording)
{
  // Note what is *not* here: an in-progress recording's duration comes from its
  // scheduled EndDate (see LoadRecordingsInternal), not from a server-side
  // counter that ticks up every poll, so a recording in progress holds a stable
  // fingerprint for its whole run rather than refreshing the UI every interval.
  uint64_t hash = 0;
  HashCombine(hash, HashString(recording.GetRecordingId()));
  HashCombine(hash, HashString(recording.GetTitle()));
  HashCombine(hash, HashString(recording.GetEpisodeName()));
  HashCombine(hash, HashString(recording.GetPlot()));
  HashCombine(hash, HashString(recording.GetChannelName()));
  HashCombine(hash, HashString(recording.GetIconPath()));
  HashCombine(hash, HashString(recording.GetDirectory()));
  HashCombine(hash, HashString(recording.GetFirstAired()));
  HashCombine(hash, HashString(recording.GetGenreDescription()));
  HashCombine(hash, recording.GetFlags());
  HashCombine(hash, recording.GetEPGEventId());
  HashCombine(hash, static_cast<uint64_t>(recording.GetChannelUid()));
  HashCombine(hash, static_cast<uint64_t>(recording.GetRecordingTime()));
  HashCombine(hash, static_cast<uint64_t>(recording.GetDuration()));
  HashCombine(hash, static_cast<uint64_t>(recording.GetSeriesNumber()));
  HashCombine(hash, static_cast<uint64_t>(recording.GetEpisodeNumber()));
  HashCombine(hash, static_cast<uint64_t>(recording.GetYear()));
  HashCombine(hash, static_cast<uint64_t>(recording.GetPlayCount()));
  HashCombine(hash, static_cast<uint64_t>(recording.GetLastPlayedPosition()));
  return hash;
}

bool JellyfinRecordingManager::HasRecordingForEpg(unsigned int broadcastUid, int channelUid) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return HasRecordingForEpgLocked(broadcastUid, channelUid);
}

bool JellyfinRecordingManager::HasRecordingForEpgLocked(unsigned int broadcastUid, int channelUid) const
{
  for (const auto& rec : m_recordings)
  {
    if (rec.GetEPGEventId() == broadcastUid && rec.GetChannelUid() == channelUid)
      return true;
  }
  return false;
}

std::string JellyfinRecordingManager::GetRecordingIdForEpg(unsigned int broadcastUid, int channelUid) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  for (const auto& rec : m_recordings)
  {
    if (rec.GetEPGEventId() == broadcastUid && rec.GetChannelUid() == channelUid)
      return rec.GetRecordingId();
  }
  return "";
}

PVR_ERROR JellyfinRecordingManager::LoadTimers()
{
  // Exception firewall — see AddTimer.
  try
  {
    return LoadTimersInternal();
  }
  catch (const std::exception& e)
  {
    Logger::Log(LEVEL_ERROR, "%s - Exception parsing timer data: %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

PVR_ERROR JellyfinRecordingManager::LoadTimersInternal()
{
  const std::string endpoint = "/LiveTv/Timers";
  Json::Value response = m_client->SendGet(endpoint);

  std::lock_guard<std::mutex> lock(m_mutex);

  // Keep the previous model on a failed fetch, as LoadRecordings does: a server
  // with no timers still answers with an empty Items array, so a missing Items
  // means the request failed, and blanking the list on a transient blip made
  // every timer vanish from the UI until the next poll.
  if (response.isNull() || !response.isMember("Items"))
  {
    Logger::Log(LEVEL_WARNING, "%s - Failed to load timers, keeping previous data", __FUNCTION__);
    return PVR_ERROR_SERVER_ERROR;
  }

  m_timers.clear();
  m_timerUidToId.clear();
  m_timerNameToProgramId.clear();
  m_timerNameToChannelUid.clear();

  const Json::Value& items = response["Items"];

  for (const auto& item : items)
  {
    const std::string jellyfinId = item["Id"].asString();
    const int uid = GenerateUid(jellyfinId);
    m_timerUidToId[uid] = jellyfinId;

    kodi::addon::PVRTimer timer;
    timer.SetClientIndex(uid);
    timer.SetTitle(item.get("Name", "").asString());
    timer.SetSummary(item.get("Overview", "").asString());

    // Timer type: child of series, manual, or EPG-based
    const std::string seriesTimerId = item.get("SeriesTimerId", "").asString();
    if (!seriesTimerId.empty())
    {
      timer.SetTimerType(TIMER_ONCE_CREATED_BY_SERIES);
      timer.SetParentClientIndex(GenerateUid(seriesTimerId));
    }
    else if (item.get("IsManual", false).asBool())
    {
      timer.SetTimerType(TIMER_ONCE_MANUAL);
    }
    else
    {
      timer.SetTimerType(TIMER_ONCE_EPG);
    }

    // Channel
    if (item.isMember("ChannelId"))
    {
      const std::string channelJfId = item["ChannelId"].asString();
      // Reconstruct the Kodi channel UID the loader computes for this
      // Jellyfin channel — shared recipe, see utilities::GenerateChannelUid.
      int channelUid = 0;
      if (m_channelLoader)
      {
        channelUid = GenerateChannelUid(item.get("ChannelName", "").asString(),
                                        m_client->GetBaseUrl(), channelJfId);
      }
      timer.SetClientChannelUid(channelUid);
    }

    // Timing
    if (item.isMember("StartDate"))
      timer.SetStartTime(ParseIso8601(item["StartDate"].asString()));
    if (item.isMember("EndDate"))
      timer.SetEndTime(ParseIso8601(item["EndDate"].asString()));

    // Padding/margins (Jellyfin uses seconds, Kodi uses minutes)
    if (item.isMember("PrePaddingSeconds"))
      timer.SetMarginStart(SafeInt(item["PrePaddingSeconds"]) / 60);
    if (item.isMember("PostPaddingSeconds"))
      timer.SetMarginEnd(SafeInt(item["PostPaddingSeconds"]) / 60);

    // State
    const std::string status = item.get("Status", "New").asString();
    Logger::Log(LEVEL_DEBUG, "%s - Timer %s: status=%s", __FUNCTION__,
                item.get("Name", "").asString().c_str(), status.c_str());
    if (status == "New" || status == "Scheduled")
      timer.SetState(PVR_TIMER_STATE_SCHEDULED);
    else if (status == "InProgress")
      timer.SetState(PVR_TIMER_STATE_RECORDING);
    else if (status == "Completed")
      timer.SetState(PVR_TIMER_STATE_COMPLETED);
    else if (status == "Cancelled" || status == "Canceled")
      timer.SetState(PVR_TIMER_STATE_CANCELLED);
    else if (status == "ConflictedOk")
      timer.SetState(PVR_TIMER_STATE_CONFLICT_OK);
    else if (status == "ConflictedNotOk")
      timer.SetState(PVR_TIMER_STATE_CONFLICT_NOK);
    else if (status == "Error")
      timer.SetState(PVR_TIMER_STATE_ERROR);
    else
      timer.SetState(PVR_TIMER_STATE_NEW);

    // EPG reference
    if (item.isMember("ProgramId"))
    {
      const std::string programId = item["ProgramId"].asString();
      timer.SetEPGUid(GenerateUid(programId));
      const std::string timerName = item.get("Name", "").asString();
      // Save name→ProgramId and name→ChannelUid for cross-referencing in-progress
      // recordings (their items come back with no ProgramId/ChannelId of their own).
      // Only populate from InProgress timers: an in-progress recording maps to an
      // InProgress timer, and back-to-back programmes can share an identical
      // truncated title. A name-keyed map built from all timers would let a later
      // "New" timer for the same-named next programme overwrite the in-progress
      // entry, mis-linking the recording's EPG event to the wrong programme.
      if (status == "InProgress" && !programId.empty())
      {
        m_timerNameToProgramId[timerName] = programId;
        if (timer.GetClientChannelUid() != 0)
          m_timerNameToChannelUid[timerName] = timer.GetClientChannelUid();
      }
    }

    m_timers.emplace_back(timer);
  }

  Logger::Log(LEVEL_INFO, "%s - Loaded %d timers", __FUNCTION__,
              static_cast<int>(m_timers.size()));
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR JellyfinRecordingManager::LoadSeriesTimers()
{
  // Exception firewall — see AddTimer.
  try
  {
    return LoadSeriesTimersInternal();
  }
  catch (const std::exception& e)
  {
    Logger::Log(LEVEL_ERROR, "%s - Exception parsing series timer data: %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

PVR_ERROR JellyfinRecordingManager::LoadSeriesTimersInternal()
{
  const std::string endpoint = "/LiveTv/SeriesTimers";
  Json::Value response = m_client->SendGet(endpoint);

  std::lock_guard<std::mutex> lock(m_mutex);

  // Keep the previous model on a failed fetch — see LoadTimersInternal.
  if (response.isNull() || !response.isMember("Items"))
  {
    Logger::Log(LEVEL_WARNING, "%s - Failed to load series timers, keeping previous data",
                __FUNCTION__);
    return PVR_ERROR_SERVER_ERROR;
  }

  m_seriesTimers.clear();
  m_seriesTimerUidToId.clear();

  const Json::Value& items = response["Items"];

  for (const auto& item : items)
  {
    const std::string jellyfinId = item["Id"].asString();
    const int uid = GenerateUid(jellyfinId);
    m_seriesTimerUidToId[uid] = jellyfinId;

    kodi::addon::PVRTimer timer;
    timer.SetClientIndex(uid);
    timer.SetTimerType(TIMER_SERIES);
    timer.SetTitle(item.get("Name", "").asString());
    timer.SetState(PVR_TIMER_STATE_SCHEDULED);

    // Channel
    if (item.isMember("ChannelId") && !item["ChannelId"].asString().empty())
    {
      const std::string channelJfId = item["ChannelId"].asString();
      // Shared channel-UID recipe — see utilities::GenerateChannelUid.
      const int channelUid = GenerateChannelUid(item.get("ChannelName", "").asString(),
                                                m_client->GetBaseUrl(), channelJfId);
      timer.SetClientChannelUid(channelUid);
    }
    else
    {
      timer.SetClientChannelUid(PVR_TIMER_ANY_CHANNEL);
    }

    // Options
    if (item.isMember("RecordNewOnly") && item["RecordNewOnly"].asBool())
      timer.SetPreventDuplicateEpisodes(1);

    // Weekdays from Days array
    unsigned int weekdays = PVR_WEEKDAY_NONE;
    if (item.isMember("Days") && item["Days"].isArray())
    {
      for (const auto& day : item["Days"])
      {
        const std::string d = day.asString();
        if (d == "Monday") weekdays |= PVR_WEEKDAY_MONDAY;
        else if (d == "Tuesday") weekdays |= PVR_WEEKDAY_TUESDAY;
        else if (d == "Wednesday") weekdays |= PVR_WEEKDAY_WEDNESDAY;
        else if (d == "Thursday") weekdays |= PVR_WEEKDAY_THURSDAY;
        else if (d == "Friday") weekdays |= PVR_WEEKDAY_FRIDAY;
        else if (d == "Saturday") weekdays |= PVR_WEEKDAY_SATURDAY;
        else if (d == "Sunday") weekdays |= PVR_WEEKDAY_SUNDAY;
      }
    }
    if (weekdays == PVR_WEEKDAY_NONE)
      weekdays = PVR_WEEKDAY_ALLDAYS;
    timer.SetWeekdays(weekdays);

    // Padding
    if (item.isMember("PrePaddingSeconds"))
      timer.SetMarginStart(SafeInt(item["PrePaddingSeconds"]) / 60);
    if (item.isMember("PostPaddingSeconds"))
      timer.SetMarginEnd(SafeInt(item["PostPaddingSeconds"]) / 60);

    m_seriesTimers.emplace_back(timer);
  }

  Logger::Log(LEVEL_INFO, "%s - Loaded %d series timers", __FUNCTION__,
              static_cast<int>(m_seriesTimers.size()));
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR JellyfinRecordingManager::LoadRecordings()
{
  // Exception firewall — see AddTimer.
  try
  {
    return LoadRecordingsInternal();
  }
  catch (const std::exception& e)
  {
    Logger::Log(LEVEL_ERROR, "%s - Exception parsing recording data: %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

PVR_ERROR JellyfinRecordingManager::LoadRecordingsInternal()
{
  // Fetch before locking: the HTTP call can stall up to the connection
  // timeout, and Kodi's UI-path callbacks (GetTimers, GetRecordings,
  // HasRecordingForEpg, ...) block on m_mutex — holding it across the network
  // round-trip froze the UI whenever the server was slow. Same fetch-then-lock
  // structure as LoadTimers/LoadSeriesTimers; parsing below is pure CPU.
  const std::string endpoint = "/LiveTv/Recordings?UserId=" + m_client->GetUserId()
    + "&EnableImages=true&Fields=Overview,ChannelInfo,ProgramId,DateCreated";
  Json::Value response = m_client->SendGet(endpoint);

  std::lock_guard<std::mutex> lock(m_mutex);

  if (response.isNull() || !response.isMember("Items"))
  {
    // Keep the previous model and the premature-stop snapshot: a transient
    // fetch failure must not blank the recordings list, nor make every active
    // recording look "stopped early" on the next successful poll.
    Logger::Log(LEVEL_WARNING, "%s - Failed to load recordings, keeping previous data", __FUNCTION__);
    return PVR_ERROR_SERVER_ERROR;
  }

  // Previous poll's in-progress set; diffed after the rebuild below to detect
  // recordings that stopped before their scheduled end.
  const std::map<std::string, InProgressInfo> previousInProgress = std::move(m_inProgressSnapshot);
  m_inProgressSnapshot.clear();

  m_recordings.clear();
  m_inProgressRecordingIds.clear();

  // Collect Jellyfin items from multiple sources, de-duplicated by ID
  std::set<std::string> seenIds;
  Json::Value allItems(Json::arrayValue);

  // 1. All recordings from /LiveTv/Recordings (includes in-progress ones)
  //    The Status field on each item tells us if it's still recording.
  {
    if (!response.isNull() && response.isMember("Items"))
    {
      for (const auto& item : response["Items"])
      {
        const std::string id = item["Id"].asString();
        if (seenIds.insert(id).second)
        {
          const std::string status = item.get("Status", "").asString();
          Logger::Log(LEVEL_DEBUG, "%s - /LiveTv/Recordings item: %s, status=%s, id=%s",
                      __FUNCTION__, item.get("Name", "").asString().c_str(),
                      status.empty() ? "(none)" : status.c_str(), id.c_str());
          if (status == "InProgress")
            m_inProgressRecordingIds.insert(id);
          allItems.append(item);
        }
      }
      Logger::Log(LEVEL_INFO, "%s - Found %d recordings from /LiveTv/Recordings (%d in-progress)",
                  __FUNCTION__, static_cast<int>(allItems.size()),
                  static_cast<int>(m_inProgressRecordingIds.size()));
    }
  }

  // Note: library view query removed — after Jellyfin library scans,
  // non-recording items get indexed into the Recordings folder and appear
  // as phantom recordings in Kodi. /LiveTv/Recordings is authoritative.

  // 3. Build PVRRecording objects from merged items
  for (const auto& item : allItems)
  {
    const std::string jellyfinId = item["Id"].asString();
    const bool inProgress = m_inProgressRecordingIds.count(jellyfinId) > 0;

    kodi::addon::PVRRecording recording;
    recording.SetRecordingId(jellyfinId);
    recording.SetTitle(item.get("Name", "").asString());

    if (inProgress)
      recording.SetFlags(PVR_RECORDING_FLAG_IS_LIVE);

    // EPG link (enables "Play recording" in EPG context menu)
    std::string programId;
    if (item.isMember("ProgramId") && !item["ProgramId"].asString().empty())
    {
      programId = item["ProgramId"].asString();
      Logger::Log(LEVEL_DEBUG, "%s - Recording '%s' has ProgramId from API: %s",
                  __FUNCTION__, item.get("Name", "").asString().c_str(), programId.c_str());
    }
    // Fallback: cross-reference timer data by name (recordings may lack ProgramId)
    if (programId.empty() && inProgress)
    {
      auto it = m_timerNameToProgramId.find(item.get("Name", "").asString());
      if (it != m_timerNameToProgramId.end())
      {
        programId = it->second;
        Logger::Log(LEVEL_DEBUG, "%s - Recording '%s' got ProgramId from timer cross-ref: %s",
                    __FUNCTION__, item.get("Name", "").asString().c_str(), programId.c_str());
      }
      else
      {
        Logger::Log(LEVEL_WARNING, "%s - Recording '%s' (in-progress): no ProgramId found (API or timer map, %d entries)",
                    __FUNCTION__, item.get("Name", "").asString().c_str(),
                    static_cast<int>(m_timerNameToProgramId.size()));
      }
    }
    if (!programId.empty())
    {
      unsigned int epgUid = static_cast<unsigned int>(GenerateUid(programId));
      recording.SetEPGEventId(epgUid);
      Logger::Log(LEVEL_DEBUG, "%s - Recording '%s': SetEPGEventId(%u) from programId=%s",
                  __FUNCTION__, item.get("Name", "").asString().c_str(), epgUid, programId.c_str());
    }

    if (item.isMember("EpisodeTitle"))
      recording.SetEpisodeName(item["EpisodeTitle"].asString());

    if (item.isMember("Overview"))
      recording.SetPlot(item["Overview"].asString());

    if (item.isMember("ChannelName"))
      recording.SetChannelName(item["ChannelName"].asString());

    // Channel UID (live TV recordings have ChannelId; library items may not)
    {
      int channelUid = 0;
      const std::string channelJfId = item.isMember("ChannelId") ? item["ChannelId"].asString() : "";
      if (!channelJfId.empty() && channelJfId != "null" && m_channelLoader)
        channelUid = m_channelLoader->GetChannelUid(channelJfId);
      // Fallback: get channel UID from timer data (in-progress recordings may lack ChannelId)
      if (channelUid == 0 && inProgress)
      {
        auto it2 = m_timerNameToChannelUid.find(item.get("Name", "").asString());
        if (it2 != m_timerNameToChannelUid.end())
          channelUid = it2->second;
      }
      if (channelUid != 0)
        recording.SetChannelUid(channelUid);
      Logger::Log(LEVEL_DEBUG, "%s - Recording '%s': channelUid=%d, channelJfId=%s, inProgress=%d",
                  __FUNCTION__, item.get("Name", "").asString().c_str(), channelUid,
                  channelJfId.empty() ? "(none)" : channelJfId.c_str(), inProgress ? 1 : 0);
    }

    // Timing: prefer StartDate/EndDate (live TV recordings), fall back to DateCreated/RunTimeTicks (library items)
    if (item.isMember("StartDate"))
      recording.SetRecordingTime(ParseIso8601(item["StartDate"].asString()));
    else if (item.isMember("DateCreated"))
      recording.SetRecordingTime(ParseIso8601(item["DateCreated"].asString()));

    if (item.isMember("StartDate") && item.isMember("EndDate"))
    {
      time_t start = ParseIso8601(item["StartDate"].asString());
      time_t end = ParseIso8601(item["EndDate"].asString());
      recording.SetDuration(static_cast<int>(end - start));
      // EndDate of an in-progress recording is the *scheduled* end; once the
      // recording completes the field goes null (only RunTimeTicks remains),
      // so it has to be remembered now for the premature-stop check.
      if (inProgress)
        m_inProgressSnapshot[jellyfinId] = {item.get("Name", "").asString(), end};
    }
    else if (item.isMember("RunTimeTicks"))
    {
      // RunTimeTicks is in 100-nanosecond intervals
      recording.SetDuration(static_cast<int>(SafeInt64(item["RunTimeTicks"]) / 10000000LL));
    }

    // Image
    if (item.isMember("ImageTags") && item["ImageTags"].isMember("Primary"))
      recording.SetIconPath(m_client->BuildImageUrl(jellyfinId,
                                                     item["ImageTags"]["Primary"].asString()));

    // Season/Episode
    if (item.isMember("IndexNumber"))
      recording.SetEpisodeNumber(SafeInt(item["IndexNumber"]));
    if (item.isMember("ParentIndexNumber"))
      recording.SetSeriesNumber(SafeInt(item["ParentIndexNumber"]));

    // Year + full date for skin $INFO[VideoPlayer.Premiered]
    if (item.isMember("ProductionYear"))
      recording.SetYear(SafeInt(item["ProductionYear"]));
    {
      std::string iso;
      if (item.isMember("StartDate"))
        iso = item["StartDate"].asString();
      else if (item.isMember("DateCreated"))
        iso = item["DateCreated"].asString();
      if (iso.size() >= 10)
        recording.SetFirstAired(iso.substr(0, 10));
    }

    // Genre
    if (item.isMember("Genres") && item["Genres"].isArray() && !item["Genres"].empty())
      recording.SetGenreDescription(item["Genres"][0].asString());

    // Watched state from server — derive PlayCount from Played flag
    if (item.isMember("UserData"))
    {
      const Json::Value& ud = item["UserData"];
      recording.SetPlayCount(ud.get("Played", false).asBool() ? 1 : 0);
      if (ud.isMember("PlaybackPositionTicks"))
      {
        const int64_t ticks = SafeInt64(ud["PlaybackPositionTicks"]);
        recording.SetLastPlayedPosition(static_cast<int>(ticks / 10000000LL));
      }
    }

    // Directory (group recordings by series name or parent folder)
    if (item.isMember("SeriesName") && !item["SeriesName"].asString().empty())
      recording.SetDirectory(item["SeriesName"].asString());

    m_recordings.emplace_back(recording);
  }

  // A recording that was in progress last poll and no longer is — whether it
  // flipped to completed or vanished — while its scheduled end is still
  // comfortably in the future was stopped early (tuner drop, server-side
  // cancel, disk full, ...). Snapshot entries live for exactly one poll, so
  // each premature stop is reported once. Toasts render over fullscreen
  // playback, so this also covers "stopped while being watched".
  const time_t now = std::time(nullptr);
  for (const auto& entry : previousInProgress)
  {
    if (m_inProgressRecordingIds.count(entry.first) == 0 &&
        entry.second.scheduledEnd - now > PREMATURE_STOP_MARGIN_SECS)
    {
      Logger::Log(LEVEL_WARNING, "%s - Recording '%s' stopped %ld min before its scheduled end",
                  __FUNCTION__, entry.second.name.c_str(),
                  static_cast<long>((entry.second.scheduledEnd - now) / 60));
      kodi::QueueNotification(QUEUE_WARNING, "Kofin PVR",
                              kodi::addon::GetLocalizedString(30830) + ": " + entry.second.name);
    }
  }

  Logger::Log(LEVEL_INFO, "%s - Loaded %d recordings (%d in-progress)", __FUNCTION__,
              static_cast<int>(m_recordings.size()),
              static_cast<int>(m_inProgressRecordingIds.size()));
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR JellyfinRecordingManager::SetRecordingPlayCount(const kodi::addon::PVRRecording& recording, int count)
{
  const std::string recordingId = recording.GetRecordingId();

  if (count == 0)
  {
    // Mark as unwatched — send immediately (Kodi may not call Position(0) after this)
    Logger::Log(LEVEL_INFO, "%s - Marking unwatched: %s", __FUNCTION__, recordingId.c_str());
    const std::string endpoint = "/Users/" + m_client->GetUserId()
      + "/Items/" + recordingId + "/UserData";
    Json::Value body;
    body["Played"] = false;
    body["PlaybackPositionTicks"] = static_cast<Json::Int64>(0);
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    if (!m_client->SendPostExpectSuccess(endpoint, Json::writeString(writer, body)))
      return PVR_ERROR_SERVER_ERROR;
  }
  else
  {
    // Record the intent — defer to SetRecordingLastPlayedPosition(0) to
    // distinguish "mark as watched" (PlayCount then Position=0) from
    // "playback start" (PlayCount only, no Position=0)
    std::lock_guard<std::mutex> lock(m_mutex);
    m_recentPlayCountCalls[recordingId] = {count, std::chrono::steady_clock::now()};
    Logger::Log(LEVEL_DEBUG, "%s - Recorded PlayCount(%d) for %s",
                __FUNCTION__, count, recordingId.c_str());
  }

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR JellyfinRecordingManager::SetRecordingLastPlayedPosition(const kodi::addon::PVRRecording& recording, int lastplayedposition)
{
  const std::string recordingId = recording.GetRecordingId();

  if (lastplayedposition < 0)
    lastplayedposition = 0;

  const std::string endpoint = "/Users/" + m_client->GetUserId()
    + "/Items/" + recordingId + "/UserData";

  Json::Value body;
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";

  if (lastplayedposition > 0)
  {
    // Normal position save
    body["PlaybackPositionTicks"] = static_cast<Json::Int64>(static_cast<int64_t>(lastplayedposition) * 10000000LL);
    Logger::Log(LEVEL_DEBUG, "%s - Setting resume position %ds for %s",
                __FUNCTION__, lastplayedposition, recordingId.c_str());
  }
  else
  {
    // Position=0 — check for a recent SetRecordingPlayCount to determine intent
    body["PlaybackPositionTicks"] = static_cast<Json::Int64>(0);

    bool hadRecentPlayCount = false;
    bool markPlayed = false;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      auto it = m_recentPlayCountCalls.find(recordingId);
      if (it != m_recentPlayCountCalls.end() &&
          std::chrono::steady_clock::now() - it->second.second < std::chrono::seconds(2))
      {
        hadRecentPlayCount = true;
        markPlayed = (it->second.first > 0);
        m_recentPlayCountCalls.erase(it);
      }
    }

    if (hadRecentPlayCount)
    {
      body["Played"] = markPlayed;
      Logger::Log(LEVEL_INFO, "%s - Marking %s for %s",
                  __FUNCTION__, markPlayed ? "watched" : "unwatched", recordingId.c_str());
    }
    else
    {
      // Reset resume position (no preceding PlayCount) — just clear position
      Logger::Log(LEVEL_INFO, "%s - Clearing resume position for %s",
                  __FUNCTION__, recordingId.c_str());
    }
  }

  if (!m_client->SendPostExpectSuccess(endpoint, Json::writeString(writer, body)))
    return PVR_ERROR_SERVER_ERROR;

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR JellyfinRecordingManager::GetRecordingLastPlayedPosition(const kodi::addon::PVRRecording& recording, int& position)
{
  // Position is set via recording.SetLastPlayedPosition() in LoadRecordings.
  // Returning NOT_IMPLEMENTED tells Kodi to use that value instead of polling
  // the server per-recording every 10 seconds (which blocks the UI thread).
  position = -1;
  return PVR_ERROR_NOT_IMPLEMENTED;
}

/***************************************************************************
 * Recorded-stream byte path (Recordings window playback)
 *
 * Streams the raw recording file via /Videos/{id}/stream?static=true using
 * kodi::vfs::CFile. Used by the PVR Recordings section, which opens recordings
 * directly rather than via GetRecordingStreamProperties. Only one recording
 * plays at a time and Kodi drives Open/Read/Seek/Close on a single playback
 * thread, so this state is independent of the data model and not guarded by
 * m_mutex.
 **************************************************************************/

bool JellyfinRecordingManager::OpenRecordedStream(const kodi::addon::PVRRecording& recording)
{
  CloseRecordedStream();

  const std::string recordingId = recording.GetRecordingId();
  if (!m_client)
    return false;

  // Token in the Authorization header, not the URL: unlike the inputstream
  // URLs (where a query param is unavoidable), this request is made by the
  // addon itself, so the token can stay out of server/proxy access logs.
  const std::string streamUrl = m_client->GetBaseUrl()
    + "/Videos/" + recordingId + "/stream?static=true";

  Logger::Log(LEVEL_INFO, "%s - Opening recording stream for %s", __FUNCTION__, recordingId.c_str());

  if (!m_recordingStream.CURLCreate(streamUrl) ||
      !m_recordingStream.CURLAddOption(ADDON_CURL_OPTION_HEADER, "Authorization",
                                       m_client->BuildAuthHeader()) ||
      !m_recordingStream.CURLOpen(ADDON_READ_NO_CACHE))
  {
    Logger::Log(LEVEL_ERROR, "%s - Failed to open recording stream: %s", __FUNCTION__, recordingId.c_str());
    m_recordingStream.Close();
    return false;
  }

  m_recordingStreamOpen = true;
  Logger::Log(LEVEL_INFO, "%s - Recording stream open, length: %lld",
              __FUNCTION__, static_cast<long long>(m_recordingStream.GetLength()));
  return true;
}

void JellyfinRecordingManager::CloseRecordedStream()
{
  if (m_recordingStreamOpen)
  {
    Logger::Log(LEVEL_INFO, "%s - Closing recording stream", __FUNCTION__);
    m_recordingStream.Close();
    m_recordingStreamOpen = false;
  }
}

int JellyfinRecordingManager::ReadRecordedStream(unsigned char* buffer, unsigned int size)
{
  if (!m_recordingStreamOpen)
    return -1;

  auto bytesRead = m_recordingStream.Read(buffer, size);
  return static_cast<int>(bytesRead);
}

int64_t JellyfinRecordingManager::SeekRecordedStream(int64_t position, int whence)
{
  if (!m_recordingStreamOpen)
    return -1;

  return m_recordingStream.Seek(position, whence);
}

int64_t JellyfinRecordingManager::LengthRecordedStream()
{
  if (!m_recordingStreamOpen)
    return -1;

  return m_recordingStream.GetLength();
}

/***************************************************************************
 * Utilities
 **************************************************************************/

int JellyfinRecordingManager::GenerateUid(const std::string& str)
{
  return utilities::GenerateUid(str);
}

time_t JellyfinRecordingManager::ParseIso8601(const std::string& dateStr)
{
  if (dateStr.empty())
    return 0;

  std::tm tm = {};
  if (sscanf(dateStr.c_str(), "%d-%d-%dT%d:%d:%d",
             &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
             &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 6)
  {
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return SafeTimegm(&tm);
  }
  return 0;
}

std::string JellyfinRecordingManager::FormatIso8601(time_t time)
{
  std::tm tm = SafeGmtime(time);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return std::string(buf);
}
