/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 *
 *  Adapted from pvr.iptvsimple CatchupController — removed Epg/StreamManager
 *  dependencies. Jellyfin provides stream URLs via PlaybackInfo; catchup URL
 *  construction uses the Channel's catchup source populated from the reference M3U.
 */

#include "CatchupController.h"

#include "data/Channel.h"
#include "utilities/Logger.h"
#include "utilities/TimeUtils.h"
#include "utilities/WebUtils.h"

#include <iomanip>
#include <regex>

#include <kodi/tools/StringUtils.h>

using namespace kodi::tools;
using namespace iptvsimple;
using namespace iptvsimple::data;
using namespace iptvsimple::utilities;

CatchupController::CatchupController(std::shared_ptr<InstanceSettings>& settings)
  : m_settings(settings) {}

int CatchupController::GetTimezoneShift(const Channel& channel) const
{
  return channel.GetCatchupCorrectionSecs();
}

// ─── Format helpers (from pvr.iptvsimple, unchanged) ────────────────────────

namespace
{

void FormatUnits(const std::string& name, time_t tTime, std::string& urlFormatString)
{
  const std::regex timeSecondsRegex(".*(\\{" + name + ":(\\d+)\\}).*");
  std::cmatch mr;
  if (std::regex_match(urlFormatString.c_str(), mr, timeSecondsRegex) && mr.length() >= 3)
  {
    std::string timeSecondsExp = mr[1].first;
    std::string second = mr[1].second;
    if (second.length() > 0)
      timeSecondsExp = timeSecondsExp.erase(timeSecondsExp.find(second));
    std::string dividerStr = mr[2].first;
    second = mr[2].second;
    if (second.length() > 0)
      dividerStr = dividerStr.erase(dividerStr.find(second));

    const time_t divider = stoi(dividerStr);
    if (divider != 0)
    {
      time_t units = tTime / divider;
      if (units < 0)
        units = 0;
      urlFormatString.replace(urlFormatString.find(timeSecondsExp), timeSecondsExp.length(), std::to_string(units));
    }
  }
}

void FormatTime(const char ch, const struct tm* pTime, std::string& urlFormatString)
{
  std::string str = {'{', ch, '}'};
  size_t pos = urlFormatString.find(str);
  while (pos != std::string::npos)
  {
    std::ostringstream os;
    os << std::put_time(pTime, StringUtils::Format("%%%c", ch).c_str());
    std::string timeString = os.str();
    if (timeString.size() > 0)
      urlFormatString.replace(pos, str.size(), timeString);
    pos = urlFormatString.find(str);
  }
}

void FormatTime(const std::string name, const struct tm* pTime, std::string& urlFormatString, bool hasVarPrefix)
{
  std::string qualifier = hasVarPrefix ? "$" : "";
  qualifier += "{" + name + ":";
  size_t found = urlFormatString.find(qualifier);
  if (found != std::string::npos)
  {
    size_t foundStart = found + qualifier.size();
    size_t foundEnd = urlFormatString.find("}", foundStart + 1);
    if (foundEnd != std::string::npos)
    {
      std::string formatString = urlFormatString.substr(foundStart, foundEnd - foundStart);
      const std::regex timeSpecifiers("([YmdHMS])");
      formatString = std::regex_replace(formatString, timeSpecifiers, R"(%$&)");
      std::ostringstream os;
      os << std::put_time(pTime, formatString.c_str());
      std::string timeString = os.str();
      if (timeString.size() > 0)
        urlFormatString.replace(found, foundEnd - found + 1, timeString);
    }
  }
}

void FormatUtc(const std::string& str, time_t tTime, std::string& urlFormatString)
{
  auto pos = urlFormatString.find(str);
  if (pos != std::string::npos)
  {
    std::string utcTimeAsString = StringUtils::Format("%lu", tTime);
    urlFormatString.replace(pos, str.size(), utcTimeAsString);
  }
}

std::string FormatDateTime(time_t timeStart, time_t duration, const std::string& urlFormatString)
{
  std::string formattedUrl = urlFormatString;

  const time_t timeEnd = timeStart + duration;
  const time_t timeNow = std::time(0);

  std::tm dateTimeStart = SafeLocaltime(timeStart);
  std::tm dateTimeEnd = SafeLocaltime(timeEnd);
  std::tm dateTimeNow = SafeLocaltime(timeNow);

  FormatTime('Y', &dateTimeStart, formattedUrl);
  FormatTime('m', &dateTimeStart, formattedUrl);
  FormatTime('d', &dateTimeStart, formattedUrl);
  FormatTime('H', &dateTimeStart, formattedUrl);
  FormatTime('M', &dateTimeStart, formattedUrl);
  FormatTime('S', &dateTimeStart, formattedUrl);
  FormatUtc("{utc}", timeStart, formattedUrl);
  FormatUtc("${start}", timeStart, formattedUrl);
  FormatUtc("{utcend}", timeStart + duration, formattedUrl);
  FormatUtc("${end}", timeStart + duration, formattedUrl);
  FormatUtc("{lutc}", timeNow, formattedUrl);
  FormatUtc("${now}", timeNow, formattedUrl);
  FormatUtc("${timestamp}", timeNow, formattedUrl);
  FormatUtc("${duration}", duration, formattedUrl);
  FormatUtc("{duration}", duration, formattedUrl);
  FormatUnits("duration", duration, formattedUrl);
  FormatUtc("${offset}", timeNow - timeStart, formattedUrl);
  FormatUnits("offset", timeNow - timeStart, formattedUrl);

  FormatTime("utc", &dateTimeStart, formattedUrl, false);
  FormatTime("start", &dateTimeStart, formattedUrl, true);
  FormatTime("utcend", &dateTimeEnd, formattedUrl, false);
  FormatTime("end", &dateTimeEnd, formattedUrl, true);
  FormatTime("lutc", &dateTimeNow, formattedUrl, false);
  FormatTime("now", &dateTimeNow, formattedUrl, true);
  FormatTime("timestamp", &dateTimeNow, formattedUrl, true);

  Logger::Log(LEVEL_DEBUG, "%s - \"%s\"", __FUNCTION__, WebUtils::RedactUrl(formattedUrl).c_str());
  return formattedUrl;
}

std::string FormatDateTimeNowOnly(const std::string& urlFormatString, int timezoneShiftSecs, int timeStart = 0, int duration = 0)
{
  std::string formattedUrl = urlFormatString;

  timeStart -= timezoneShiftSecs;
  const time_t timeNow = std::time(0) - timezoneShiftSecs;
  std::tm dateTimeNow = SafeLocaltime(timeNow);

  FormatUtc("{lutc}", timeNow, formattedUrl);
  FormatUtc("${now}", timeNow, formattedUrl);
  FormatUtc("${timestamp}", timeNow, formattedUrl);
  FormatTime("lutc", &dateTimeNow, formattedUrl, false);
  FormatTime("now", &dateTimeNow, formattedUrl, true);
  FormatTime("timestamp", &dateTimeNow, formattedUrl, true);

  if (timeStart > 0)
  {
    std::tm dateTimeStart = SafeLocaltime(timeStart);
    const time_t timeEnd = timeStart + duration;
    std::tm dateTimeEnd = SafeLocaltime(timeEnd);

    FormatTime('Y', &dateTimeStart, formattedUrl);
    FormatTime('m', &dateTimeStart, formattedUrl);
    FormatTime('d', &dateTimeStart, formattedUrl);
    FormatTime('H', &dateTimeStart, formattedUrl);
    FormatTime('M', &dateTimeStart, formattedUrl);
    FormatTime('S', &dateTimeStart, formattedUrl);
    FormatUtc("{utc}", timeStart, formattedUrl);
    FormatUtc("${start}", timeStart, formattedUrl);
    FormatUtc("{utcend}", timeStart + duration, formattedUrl);
    FormatUtc("${end}", timeStart + duration, formattedUrl);
    FormatUtc("${duration}", duration, formattedUrl);
    FormatUtc("{duration}", duration, formattedUrl);
    FormatUnits("duration", duration, formattedUrl);
    FormatUtc("${offset}", timeNow - timeStart, formattedUrl);
    FormatUnits("offset", timeNow - timeStart, formattedUrl);

    FormatTime("utc", &dateTimeStart, formattedUrl, false);
    FormatTime("start", &dateTimeStart, formattedUrl, true);
    FormatTime("utcend", &dateTimeEnd, formattedUrl, false);
    FormatTime("end", &dateTimeEnd, formattedUrl, true);
  }

  Logger::Log(LEVEL_DEBUG, "%s - \"%s\"", __FUNCTION__, WebUtils::RedactUrl(formattedUrl).c_str());
  return formattedUrl;
}

std::string BuildEpgTagUrl(time_t startTime, time_t duration, const Channel& channel,
                           long long timeOffset, const std::string& programmeCatchupId,
                           int timezoneShiftSecs)
{
  std::string startTimeUrl;
  time_t timeNow = std::time(nullptr);
  time_t offset = startTime + timeOffset;

  if ((startTime > 0 && offset < (timeNow - 5)) ||
      (channel.IgnoreCatchupDays() && !programmeCatchupId.empty()))
    startTimeUrl = FormatDateTime(offset - timezoneShiftSecs, duration, channel.GetCatchupSource());
  else
    startTimeUrl = FormatDateTimeNowOnly(channel.GetStreamURL(), timezoneShiftSecs, startTime, duration);

  static const std::regex CATCHUP_ID_REGEX("\\{catchup-id\\}");
  if (!programmeCatchupId.empty())
    startTimeUrl = std::regex_replace(startTimeUrl, CATCHUP_ID_REGEX, programmeCatchupId);

  Logger::Log(LEVEL_DEBUG, "%s - %s", __FUNCTION__, WebUtils::RedactUrl(startTimeUrl).c_str());
  return startTimeUrl;
}

} // unnamed namespace

// ─── Public methods ─────────────────────────────────────────────────────────

void CatchupController::ProcessChannelForPlayback(const Channel& channel, std::map<std::string, std::string>& catchupProperties)
{
  // Matching pvr.iptvsimple's flow:
  // - From a normal channel switch: reset state, set up fresh catchup window
  // - From a timeshifted EPG tag: preserve the programme times and offset

  m_playbackIsVideo = false;
  m_controlsLiveStream = channel.IsCatchupSupported() && channel.CatchupSupportsTimeshifting();

  if (!m_fromTimeshiftedEpgTagCall)
  {
    // Normal channel switch — clear programme state
    ClearProgramme();
    m_programmeCatchupId.clear();
    m_catchupStartTime = 0;
    m_catchupEndTime = 0;
  }

  if (m_controlsLiveStream)
  {
    if (m_resetCatchupState)
    {
      m_resetCatchupState = false;
      m_programmeCatchupId.clear();
      if (channel.IsCatchupSupported())
      {
        m_timeshiftBufferOffset = channel.GetCatchupDaysInSeconds();
        m_timeshiftBufferStartTime = std::time(nullptr) - channel.GetCatchupDaysInSeconds();
      }
      else
      {
        m_timeshiftBufferOffset = 0;
        m_timeshiftBufferStartTime = 0;
      }
    }

    // We no longer need to know if this originated from an EPG tag
    m_fromTimeshiftedEpgTagCall = false;

    m_catchupStartTime = m_timeshiftBufferStartTime;

    SetCatchupInputStreamProperties(true, channel, catchupProperties);
  }
  else
  {
    m_fromTimeshiftedEpgTagCall = false;
  }
}

void CatchupController::ProcessEPGTagForTimeshiftedPlayback(const kodi::addon::PVREPGTag& epgTag, const Channel& channel, std::map<std::string, std::string>& catchupProperties)
{
  m_programmeCatchupId.clear();
  m_controlsLiveStream = channel.CatchupSupportsTimeshifting();

  if (m_controlsLiveStream)
  {
    UpdateProgrammeFrom(epgTag, channel.GetTvgShift());
    m_catchupStartTime = epgTag.GetStartTime();
    m_catchupEndTime = epgTag.GetEndTime();

    time_t timeNow = time(0);
    time_t programmeOffset = timeNow - m_catchupStartTime;
    time_t timeshiftBufferDuration = std::max(programmeOffset, static_cast<time_t>(channel.GetCatchupDaysInSeconds()));
    m_timeshiftBufferStartTime = timeNow - timeshiftBufferDuration;
    m_catchupStartTime = m_timeshiftBufferStartTime;
    m_catchupEndTime = timeNow;
    m_timeshiftBufferOffset = timeshiftBufferDuration - programmeOffset;

    m_resetCatchupState = false;
    SetCatchupInputStreamProperties(true, channel, catchupProperties);
  }
  else
  {
    UpdateProgrammeFrom(epgTag, channel.GetTvgShift());
    m_catchupStartTime = epgTag.GetStartTime();
    m_catchupEndTime = epgTag.GetEndTime();
    m_timeshiftBufferStartTime = 0;
    m_timeshiftBufferOffset = 0;

    if (m_settings->CatchupPlayEpgAsLive())
      catchupProperties.insert({PVR_STREAM_PROPERTY_EPGPLAYBACKASLIVE, "true"});
  }

  m_fromTimeshiftedEpgTagCall = true;
}

void CatchupController::ProcessEPGTagForVideoPlayback(const kodi::addon::PVREPGTag& epgTag, const Channel& channel, std::map<std::string, std::string>& catchupProperties)
{
  m_programmeCatchupId.clear();
  m_controlsLiveStream = channel.CatchupSupportsTimeshifting();

  if (m_controlsLiveStream)
  {
    UpdateProgrammeFrom(epgTag, channel.GetTvgShift());
    m_catchupStartTime = epgTag.GetStartTime();
    m_catchupEndTime = epgTag.GetEndTime();

    const time_t beginBuffer = m_settings->GetCatchupWatchEpgBeginBufferSecs();
    const time_t endBuffer = m_settings->GetCatchupWatchEpgEndBufferSecs();
    m_timeshiftBufferStartTime = m_catchupStartTime - beginBuffer;
    m_catchupStartTime = m_timeshiftBufferStartTime;
    m_catchupEndTime += endBuffer;
    m_timeshiftBufferOffset = beginBuffer;

    m_resetCatchupState = false;
    SetCatchupInputStreamProperties(false, channel, catchupProperties);
  }
  else
  {
    UpdateProgrammeFrom(epgTag, channel.GetTvgShift());
    m_catchupStartTime = epgTag.GetStartTime();
    m_catchupEndTime = epgTag.GetEndTime();
    m_timeshiftBufferStartTime = 0;
    m_timeshiftBufferOffset = 0;
    m_catchupStartTime -= m_settings->GetCatchupWatchEpgBeginBufferSecs();
    m_catchupEndTime += m_settings->GetCatchupWatchEpgEndBufferSecs();
  }

  if (m_catchupStartTime > 0)
    m_playbackIsVideo = true;

  m_fromTimeshiftedEpgTagCall = false;
}

void CatchupController::SetCatchupInputStreamProperties(bool playbackAsLive, const Channel& channel, std::map<std::string, std::string>& catchupProperties)
{
  const int tzShift = GetTimezoneShift(channel);

  catchupProperties.insert({PVR_STREAM_PROPERTY_EPGPLAYBACKASLIVE, playbackAsLive ? "true" : "false"});
  catchupProperties.insert({"inputstream.ffmpegdirect.is_realtime_stream", "true"});
  catchupProperties.insert({"inputstream.ffmpegdirect.stream_mode", "catchup"});
  catchupProperties.insert({"inputstream.ffmpegdirect.default_url", channel.GetStreamURL()});
  catchupProperties.insert({"inputstream.ffmpegdirect.playback_as_live", playbackAsLive ? "true" : "false"});
  catchupProperties.insert({"inputstream.ffmpegdirect.catchup_url_format_string", GetCatchupUrlFormatString(channel)});
  catchupProperties.insert({"inputstream.ffmpegdirect.catchup_buffer_start_time", std::to_string(m_catchupStartTime)});
  catchupProperties.insert({"inputstream.ffmpegdirect.catchup_buffer_end_time", std::to_string(m_catchupEndTime)});
  catchupProperties.insert({"inputstream.ffmpegdirect.catchup_buffer_offset", std::to_string(m_timeshiftBufferOffset)});
  catchupProperties.insert({"inputstream.ffmpegdirect.timezone_shift", std::to_string(tzShift)});
  if (!m_programmeCatchupId.empty())
    catchupProperties.insert({"inputstream.ffmpegdirect.programme_catchup_id", m_programmeCatchupId});
  catchupProperties.insert({"inputstream.ffmpegdirect.catchup_terminates", channel.CatchupSourceTerminates() ? "true" : "false"});
  catchupProperties.insert({"inputstream.ffmpegdirect.catchup_granularity", std::to_string(channel.GetCatchupGranularitySeconds())});

  Logger::Log(LEVEL_DEBUG, "%s - default_url: %s", __FUNCTION__, WebUtils::RedactUrl(channel.GetStreamURL()).c_str());
  Logger::Log(LEVEL_DEBUG, "%s - catchup_url_format_string: %s", __FUNCTION__, WebUtils::RedactUrl(GetCatchupUrlFormatString(channel)).c_str());
  Logger::Log(LEVEL_DEBUG, "%s - buffer: start=%lld end=%lld offset=%lld",
              __FUNCTION__, static_cast<long long>(m_catchupStartTime),
              static_cast<long long>(m_catchupEndTime), m_timeshiftBufferOffset);
}

std::string CatchupController::GetCatchupUrlFormatString(const Channel& channel) const
{
  if (m_catchupStartTime > 0)
    return channel.GetCatchupSource();
  return "";
}

std::string CatchupController::GetCatchupUrl(const Channel& channel) const
{
  if (m_catchupStartTime > 0)
  {
    time_t duration = 60 * 60;
    if (m_programmeStartTime > 0 && m_programmeStartTime < m_programmeEndTime)
    {
      duration = static_cast<time_t>(m_programmeEndTime - m_programmeStartTime);
      if (!m_settings->CatchupPlayEpgAsLive() && m_playbackIsVideo)
        duration += m_settings->GetCatchupWatchEpgBeginBufferSecs() + m_settings->GetCatchupWatchEpgEndBufferSecs();
      time_t timeNow = time(0);
      if (m_programmeStartTime + duration > timeNow)
        duration = timeNow - m_programmeStartTime;
    }
    return BuildEpgTagUrl(m_catchupStartTime, duration, channel, m_timeshiftBufferOffset,
                          m_programmeCatchupId, GetTimezoneShift(channel));
  }
  return "";
}

std::string CatchupController::ProcessStreamUrl(const Channel& channel) const
{
  std::string processedUrl = FormatDateTimeNowOnly(channel.GetStreamURL(), GetTimezoneShift(channel),
                                                    m_programmeStartTime, m_programmeEndTime - m_programmeStartTime);
  static const std::regex CATCHUP_ID_REGEX("\\{catchup-id\\}");
  if (!m_programmeCatchupId.empty())
    processedUrl = std::regex_replace(processedUrl, CATCHUP_ID_REGEX, m_programmeCatchupId);
  return processedUrl;
}

void CatchupController::UpdateProgrammeFrom(const kodi::addon::PVREPGTag& epgTag, int tvgShift)
{
  m_programmeStartTime = epgTag.GetStartTime();
  m_programmeEndTime = epgTag.GetEndTime();
  m_programmeTitle = epgTag.GetTitle();
  m_programmeUniqueChannelId = epgTag.GetUniqueChannelId();
  m_programmeChannelTvgShift = tvgShift;
}

void CatchupController::ClearProgramme()
{
  m_programmeStartTime = 0;
  m_programmeEndTime = 0;
  m_programmeTitle.clear();
  m_programmeUniqueChannelId = 0;
  m_programmeChannelTvgShift = 0;
}
