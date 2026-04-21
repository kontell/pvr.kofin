/*
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "JellyfinRecordingManager.h"

#include "../utilities/Logger.h"
#include <kodi/General.h>
#include "../utilities/WebUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <ctime>

using namespace iptvsimple;
using namespace iptvsimple::jellyfin;
using namespace iptvsimple::utilities;

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

  // Type 4: Manual recording (channel + time, no EPG)
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

      const std::string& channelJfId = m_channelLoader->GetJellyfinId(timer.GetClientChannelUid());
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

    m_client->SendPost("/LiveTv/Timers", bodyStr);

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

      const std::string& channelJfId = m_channelLoader->GetJellyfinId(timer.GetClientChannelUid());
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

    m_client->SendPost("/LiveTv/SeriesTimers", bodyStr);

    Reload();
    return PVR_ERROR_NO_ERROR;
  }
  else if (timer.GetTimerType() == TIMER_ONCE_MANUAL)
  {
    // Manual recording — channel + time range, no EPG program ID.
    // Get timer defaults (without programId) and fill in from Kodi's timer.
    const std::string& channelJfId = m_channelLoader->GetJellyfinId(timer.GetClientChannelUid());
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
      struct tm today;
      gmtime_r(&now, &today);
      today.tm_hour = static_cast<int>(startTime / 3600);
      today.tm_min = static_cast<int>((startTime % 3600) / 60);
      today.tm_sec = static_cast<int>(startTime % 60);
      startTime = timegm(&today);
    }

    if (endTime <= 0)
      endTime = now + 7200; // Default 2 hours
    else if (endTime < 86400)
    {
      struct tm today;
      gmtime_r(&now, &today);
      today.tm_hour = static_cast<int>(endTime / 3600);
      today.tm_min = static_cast<int>((endTime % 3600) / 60);
      today.tm_sec = static_cast<int>(endTime % 60);
      endTime = timegm(&today);
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

    m_client->SendPost("/LiveTv/Timers", bodyStr);

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

  Logger::Log(LEVEL_INFO, "%s - Deleting %stimer %s (async)", __FUNCTION__,
              isSeries ? "series " : "", jellyfinId.c_str());

  // Run DELETE + reload on a detached thread to avoid blocking the UI.
  // The server-side operation (stopping a recording) can take several seconds.
  auto client = m_client;
  auto self = this;
  std::thread([client, endpoint, self]() {
    client->SendDelete(endpoint);
    self->Reload();
  }).detach();

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR JellyfinRecordingManager::UpdateTimer(const kodi::addon::PVRTimer& timer)
{
  // Jellyfin's DefaultLiveTvService.UpdateTimerAsync silently ignores updates
  // to in-progress timers (checks GetActiveRecordingPath != null). Non-active
  // timer updates are accepted but the API returns 204 without applying
  // padding changes reliably. Disabled until Jellyfin fixes this server-side.
  // See: jellyfin-allow-padding-update-on-inprogress-timers feature request.
#if 0
  const int clientIndex = timer.GetClientIndex();
  const bool isSeries = (timer.GetTimerType() == TIMER_SERIES);
  std::string jellyfinId;

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (isSeries)
    {
      auto it = m_seriesTimerUidToId.find(clientIndex);
      if (it == m_seriesTimerUidToId.end())
        return PVR_ERROR_INVALID_PARAMETERS;
      jellyfinId = it->second;
    }
    else
    {
      auto it = m_timerUidToId.find(clientIndex);
      if (it == m_timerUidToId.end())
        return PVR_ERROR_INVALID_PARAMETERS;
      jellyfinId = it->second;
    }
  }

  // GET the existing timer, modify, POST back
  const std::string endpoint = isSeries
    ? "/LiveTv/SeriesTimers/" + jellyfinId
    : "/LiveTv/Timers/" + jellyfinId;

  Json::Value existing = m_client->SendGet(endpoint);
  if (existing.isNull())
  {
    Logger::Log(LEVEL_ERROR, "%s - Failed to get timer %s for update", __FUNCTION__, jellyfinId.c_str());
    return PVR_ERROR_SERVER_ERROR;
  }

  // Apply Kodi's changes
  existing["PrePaddingSeconds"] = static_cast<int>(timer.GetMarginStart()) * 60;
  existing["IsPrePaddingRequired"] = timer.GetMarginStart() > 0;
  existing["PostPaddingSeconds"] = static_cast<int>(timer.GetMarginEnd()) * 60;
  existing["IsPostPaddingRequired"] = timer.GetMarginEnd() > 0;

  if (isSeries)
  {
    existing["RecordAnyChannel"] = (timer.GetClientChannelUid() == PVR_TIMER_ANY_CHANNEL);
    existing["RecordNewOnly"] = timer.GetPreventDuplicateEpisodes() > 0;

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
      existing["Days"] = days;
  }

  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  const std::string bodyStr = Json::writeString(writer, existing);

  Logger::Log(LEVEL_INFO, "%s - Updating %stimer %s: %s", __FUNCTION__,
              isSeries ? "series " : "", jellyfinId.c_str(), timer.GetTitle().c_str());

  m_client->SendPost(endpoint, bodyStr);

  Reload();
  return PVR_ERROR_NO_ERROR;
#endif

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
  const bool inProgress = m_inProgressRecordingIds.count(recordingId) > 0;

  // In-progress recordings: remux via PlaybackInfo + inputstream.adaptive
  // (HLS with EVENT playlist type for live-edge seeking).
  // Completed recordings: direct stream URL via Kodi's internal inputstream.
  const std::string streamUrl = m_channelLoader->GetRecordingStreamUrl(recordingId);
  if (streamUrl.empty())
  {
    Logger::Log(LEVEL_ERROR, "%s - Failed to get recording stream URL for %s",
                __FUNCTION__, recordingId.c_str());
    return PVR_ERROR_SERVER_ERROR;
  }

  Logger::Log(LEVEL_INFO, "%s - Recording stream URL (id=%s, inProgress=%d): %s",
              __FUNCTION__, recordingId.c_str(), inProgress ? 1 : 0,
              WebUtils::RedactUrl(streamUrl).c_str());

  properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, streamUrl);
  properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, inProgress ? "true" : "false");

  if (inProgress && m_settings->GetInProgressInputStream() == 0)
  {
    properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.adaptive");
    properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, "application/vnd.apple.mpegurl");
    properties.emplace_back("inputstream.adaptive.manifest_type", "hls");
  }

  return PVR_ERROR_NO_ERROR;
}

/***************************************************************************
 * Data Loading
 **************************************************************************/

void JellyfinRecordingManager::Reload()
{
  LoadTimers();
  LoadSeriesTimers();
  LoadRecordings();
}

bool JellyfinRecordingManager::HasRecordingForEpg(unsigned int broadcastUid, int channelUid) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
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
  const std::string endpoint = "/LiveTv/Timers";
  Json::Value response = m_client->SendGet(endpoint);

  std::lock_guard<std::mutex> lock(m_mutex);
  m_timers.clear();
  m_timerUidToId.clear();
  m_timerNameToProgramId.clear();
  m_timerNameToChannelUid.clear();

  if (response.isNull() || !response.isMember("Items"))
  {
    Logger::Log(LEVEL_WARNING, "%s - No timer data from Jellyfin", __FUNCTION__);
    return PVR_ERROR_NO_ERROR;
  }

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
      // Look up Kodi channel UID from Jellyfin ID
      int channelUid = 0;
      if (m_channelLoader)
      {
        // Reverse lookup: find Kodi UID for this Jellyfin channel ID
        // The channelLoader stores jellyfinId -> uid mapping
        // We need to iterate since we only have the reverse map in the loader
        // For now, generate the same UID the loader would
        channelUid = GenerateUid(
          item.get("ChannelName", "").asString() +
          m_client->GetBaseUrl() + "/LiveTv/Channels/" + channelJfId);
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
      timer.SetMarginStart(item["PrePaddingSeconds"].asInt() / 60);
    if (item.isMember("PostPaddingSeconds"))
      timer.SetMarginEnd(item["PostPaddingSeconds"].asInt() / 60);

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
      // Save name→ProgramId and name→ChannelUid for cross-referencing in-progress recordings
      if (!programId.empty())
        m_timerNameToProgramId[timerName] = programId;
      if (timer.GetClientChannelUid() != 0)
        m_timerNameToChannelUid[timerName] = timer.GetClientChannelUid();
    }

    m_timers.emplace_back(timer);
  }

  Logger::Log(LEVEL_INFO, "%s - Loaded %d timers", __FUNCTION__,
              static_cast<int>(m_timers.size()));
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR JellyfinRecordingManager::LoadSeriesTimers()
{
  const std::string endpoint = "/LiveTv/SeriesTimers";
  Json::Value response = m_client->SendGet(endpoint);

  std::lock_guard<std::mutex> lock(m_mutex);
  m_seriesTimers.clear();
  m_seriesTimerUidToId.clear();

  if (response.isNull() || !response.isMember("Items"))
  {
    Logger::Log(LEVEL_WARNING, "%s - No series timer data from Jellyfin", __FUNCTION__);
    return PVR_ERROR_NO_ERROR;
  }

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
      int channelUid = GenerateUid(
        item.get("ChannelName", "").asString() +
        m_client->GetBaseUrl() + "/LiveTv/Channels/" + channelJfId);
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
      timer.SetMarginStart(item["PrePaddingSeconds"].asInt() / 60);
    if (item.isMember("PostPaddingSeconds"))
      timer.SetMarginEnd(item["PostPaddingSeconds"].asInt() / 60);

    m_seriesTimers.emplace_back(timer);
  }

  Logger::Log(LEVEL_INFO, "%s - Loaded %d series timers", __FUNCTION__,
              static_cast<int>(m_seriesTimers.size()));
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR JellyfinRecordingManager::LoadRecordings()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_recordings.clear();
  m_recordingUidToId.clear();
  m_inProgressRecordingIds.clear();

  // Collect Jellyfin items from multiple sources, de-duplicated by ID
  std::set<std::string> seenIds;
  Json::Value allItems(Json::arrayValue);

  // 1. All recordings from /LiveTv/Recordings (includes in-progress ones)
  //    The Status field on each item tells us if it's still recording.
  {
    const std::string endpoint = "/LiveTv/Recordings?UserId=" + m_client->GetUserId()
      + "&EnableImages=true&Fields=Overview,ChannelInfo,ProgramId,DateCreated";
    Json::Value response = m_client->SendGet(endpoint);
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
    const int uid = GenerateUid(jellyfinId);
    const bool inProgress = m_inProgressRecordingIds.count(jellyfinId) > 0;
    m_recordingUidToId[uid] = jellyfinId;

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
    // Note: Jellyfin has a bug where DateCreated stores server-local time with a
    // Z suffix (claims UTC but isn't). Correct by subtracting local UTC offset.
    if (item.isMember("StartDate"))
      recording.SetRecordingTime(ParseIso8601(item["StartDate"].asString()));
    else if (item.isMember("DateCreated"))
    {
      time_t raw = ParseIso8601(item["DateCreated"].asString());
      struct tm local_tm;
      localtime_r(&raw, &local_tm);
      recording.SetRecordingTime(raw - local_tm.tm_gmtoff);
    }

    if (item.isMember("StartDate") && item.isMember("EndDate"))
    {
      time_t start = ParseIso8601(item["StartDate"].asString());
      time_t end = ParseIso8601(item["EndDate"].asString());
      recording.SetDuration(static_cast<int>(end - start));
    }
    else if (item.isMember("RunTimeTicks"))
    {
      // RunTimeTicks is in 100-nanosecond intervals
      recording.SetDuration(static_cast<int>(item["RunTimeTicks"].asInt64() / 10000000LL));
    }

    // Image
    if (item.isMember("ImageTags") && item["ImageTags"].isMember("Primary"))
      recording.SetIconPath(m_client->BuildImageUrl(jellyfinId,
                                                     item["ImageTags"]["Primary"].asString()));

    // Season/Episode
    if (item.isMember("IndexNumber"))
      recording.SetEpisodeNumber(item["IndexNumber"].asInt());
    if (item.isMember("ParentIndexNumber"))
      recording.SetSeriesNumber(item["ParentIndexNumber"].asInt());

    // Year
    if (item.isMember("ProductionYear"))
      recording.SetYear(item["ProductionYear"].asInt());

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
        int64_t ticks = ud["PlaybackPositionTicks"].asInt64();
        recording.SetLastPlayedPosition(static_cast<int>(ticks / 10000000LL));
      }
    }

    // Directory (group recordings by series name or parent folder)
    if (item.isMember("SeriesName") && !item["SeriesName"].asString().empty())
      recording.SetDirectory(item["SeriesName"].asString());

    // EPG index fallback: Jellyfin strips ProgramId/ChannelId from completed
    // recordings and replaces Name with the episode title. SeriesName still
    // holds the series title that the EPG is indexed under.
    if (recording.GetEPGEventId() == 0 && m_channelLoader)
    {
      unsigned int matchedBroadcastUid = 0;
      int matchedChannelUid = 0;
      const std::string name = item.get("Name", "").asString();
      const std::string seriesName = item.get("SeriesName", "").asString();
      if (m_channelLoader->FindRecordingEpgMatch(
            name, seriesName, recording.GetRecordingTime(),
            matchedBroadcastUid, matchedChannelUid))
      {
        recording.SetEPGEventId(matchedBroadcastUid);
        if (recording.GetChannelUid() == 0)
          recording.SetChannelUid(matchedChannelUid);
        Logger::Log(LEVEL_DEBUG, "%s - Recording '%s' (series '%s'): matched EPG (broadcastUid=%u, channelUid=%d)",
                    __FUNCTION__, name.c_str(), seriesName.c_str(),
                    matchedBroadcastUid, matchedChannelUid);
      }
    }

    m_recordings.emplace_back(recording);
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
    m_client->SendPost(endpoint, Json::writeString(writer, body));
  }
  else
  {
    // Record the intent — defer to SetRecordingLastPlayedPosition(0) to
    // distinguish "mark as watched" (PlayCount then Position=0) from
    // "playback start" (PlayCount only, no Position=0)
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

    auto it = m_recentPlayCountCalls.find(recordingId);
    bool hadRecentPlayCount = (it != m_recentPlayCountCalls.end() &&
      std::chrono::steady_clock::now() - it->second.second < std::chrono::seconds(2));

    if (hadRecentPlayCount)
    {
      bool markPlayed = (it->second.first > 0);
      body["Played"] = markPlayed;
      m_recentPlayCountCalls.erase(it);
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

  m_client->SendPost(endpoint, Json::writeString(writer, body));

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
 * Utilities
 **************************************************************************/

int JellyfinRecordingManager::GenerateUid(const std::string& str)
{
  const char* s = str.c_str();
  int hash = 0;
  int c;
  while ((c = *s++))
    hash = ((hash << 5) + hash) + c;
  return std::abs(hash);
}

time_t JellyfinRecordingManager::ParseIso8601(const std::string& dateStr)
{
  if (dateStr.empty())
    return 0;

  struct tm tm = {};
  if (sscanf(dateStr.c_str(), "%d-%d-%dT%d:%d:%d",
             &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
             &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 6)
  {
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return timegm(&tm);
  }
  return 0;
}

std::string JellyfinRecordingManager::FormatIso8601(time_t time)
{
  struct tm tm;
  gmtime_r(&time, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return std::string(buf);
}
