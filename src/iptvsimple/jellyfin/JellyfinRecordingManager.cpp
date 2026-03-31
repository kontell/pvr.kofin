/*
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "JellyfinRecordingManager.h"

#include "../utilities/Logger.h"
#include "../utilities/WebUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
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

    // Apply user padding overrides
    if (timer.GetMarginStart() > 0)
    {
      defaults["IsPrePaddingRequired"] = true;
      defaults["PrePaddingSeconds"] = static_cast<int>(timer.GetMarginStart()) * 60;
    }
    if (timer.GetMarginEnd() > 0)
    {
      defaults["IsPostPaddingRequired"] = true;
      defaults["PostPaddingSeconds"] = static_cast<int>(timer.GetMarginEnd()) * 60;
    }

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
    const std::string& channelJfId = m_channelLoader->GetJellyfinId(timer.GetClientChannelUid());

    Json::Value seriesBody;
    if (!channelJfId.empty())
      seriesBody["ChannelId"] = channelJfId;
    seriesBody["Name"] = timer.GetTitle();
    seriesBody["RecordAnyChannel"] = (timer.GetClientChannelUid() == PVR_TIMER_ANY_CHANNEL);
    seriesBody["RecordAnyTime"] = true;
    seriesBody["RecordNewOnly"] = timer.GetPreventDuplicateEpisodes() > 0;

    // Map weekdays bitmask to Jellyfin Days array
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
      seriesBody["Days"] = days;

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    const std::string bodyStr = Json::writeString(writer, seriesBody);

    Logger::Log(LEVEL_INFO, "%s - Creating series timer: %s",
                __FUNCTION__, timer.GetTitle().c_str());

    Json::Value response = m_client->SendPost("/LiveTv/SeriesTimers", bodyStr);

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

  if (!m_client->SendDelete(endpoint))
    return PVR_ERROR_SERVER_ERROR;

  Reload();
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR JellyfinRecordingManager::UpdateTimer(const kodi::addon::PVRTimer& timer)
{
  // Jellyfin doesn't have a straightforward timer update endpoint.
  // For enable/disable, we'd need to cancel and recreate.
  // For now, return not implemented.
  Logger::Log(LEVEL_DEBUG, "%s - Timer update not yet supported", __FUNCTION__);
  return PVR_ERROR_NOT_IMPLEMENTED;
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

  // Remux recording into TS via PlaybackInfo — gives proper seeking with ffmpegdirect.
  // This is independent of live TV transcode settings (unlimited bitrate = copy, TS container).
  const std::string streamUrl = m_channelLoader->GetRecordingStreamUrl(recordingId);
  if (streamUrl.empty())
  {
    Logger::Log(LEVEL_ERROR, "%s - Failed to get recording stream URL for %s",
                __FUNCTION__, recordingId.c_str());
    return PVR_ERROR_SERVER_ERROR;
  }

  Logger::Log(LEVEL_INFO, "%s - Recording stream URL (id=%s): %s", __FUNCTION__,
              recordingId.c_str(), WebUtils::RedactUrl(streamUrl).c_str());

  properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, streamUrl);
  properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.adaptive");
  properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, "application/vnd.apple.mpegurl");
  properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, "false");
  properties.emplace_back("inputstream.adaptive.manifest_type", "hls");

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

    // Timer type: child of series or standalone
    const std::string seriesTimerId = item.get("SeriesTimerId", "").asString();
    if (!seriesTimerId.empty())
    {
      timer.SetTimerType(TIMER_ONCE_CREATED_BY_SERIES);
      timer.SetParentClientIndex(GenerateUid(seriesTimerId));
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

  // 2. Completed recordings from library view or /LiveTv/Recordings fallback
  {
    std::string recordingsEndpoint;

    Json::Value views = m_client->SendGet("/Users/" + m_client->GetUserId() + "/Views");
    if (!views.isNull() && views.isMember("Items"))
    {
      for (const auto& view : views["Items"])
      {
        std::string name = view.get("Name", "").asString();
        std::string nameLower = name;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
        if (nameLower.find("record") != std::string::npos)
        {
          const std::string viewId = view["Id"].asString();
          recordingsEndpoint = "/Users/" + m_client->GetUserId() + "/Items"
            "?ParentId=" + viewId
            + "&Recursive=true"
            + "&IncludeItemTypes=Movie,Episode,Video,Recording"
            + "&Fields=Overview,ChannelInfo,DateCreated"
            + "&EnableImages=true&EnableUserData=true"
            + "&SortBy=DateCreated&SortOrder=Descending"
            + "&Limit=1000";
          Logger::Log(LEVEL_INFO, "%s - Found recordings library: %s (id: %s)",
                      __FUNCTION__, name.c_str(), viewId.c_str());
          break;
        }
      }
    }

    if (recordingsEndpoint.empty())
    {
      Logger::Log(LEVEL_INFO, "%s - No recordings library found, using /LiveTv/Recordings", __FUNCTION__);
      recordingsEndpoint = "/LiveTv/Recordings?UserId=" + m_client->GetUserId()
        + "&EnableImages=true&Fields=Overview,ChannelInfo";
    }

    Json::Value response = m_client->SendGet(recordingsEndpoint);
    if (!response.isNull() && response.isMember("Items"))
    {
      for (const auto& item : response["Items"])
      {
        const std::string id = item["Id"].asString();
        if (seenIds.insert(id).second)
          allItems.append(item);
      }
    }
  }

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
      if (!channelJfId.empty() && channelJfId != "null")
      {
        channelUid = GenerateUid(
          item.get("ChannelName", "").asString() +
          m_client->GetBaseUrl() + "/LiveTv/Channels/" + channelJfId);
      }
      // Fallback: get channel UID from timer data (in-progress recordings may lack ChannelId)
      if (channelUid == 0 && inProgress)
      {
        auto it = m_timerNameToChannelUid.find(item.get("Name", "").asString());
        if (it != m_timerNameToChannelUid.end())
        {
          channelUid = it->second;
          Logger::Log(LEVEL_DEBUG, "%s - Recording '%s': got channelUid %d from timer cross-ref",
                      __FUNCTION__, item.get("Name", "").asString().c_str(), channelUid);
        }
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

    // Play count
    if (item.isMember("UserData") && item["UserData"].isMember("PlayCount"))
      recording.SetPlayCount(item["UserData"]["PlayCount"].asInt());

    // Directory (group recordings by series name or parent folder)
    if (item.isMember("SeriesName") && !item["SeriesName"].asString().empty())
      recording.SetDirectory(item["SeriesName"].asString());

    m_recordings.emplace_back(recording);
  }

  Logger::Log(LEVEL_INFO, "%s - Loaded %d recordings (%d in-progress)", __FUNCTION__,
              static_cast<int>(m_recordings.size()),
              static_cast<int>(m_inProgressRecordingIds.size()));
  return PVR_ERROR_NO_ERROR;
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
