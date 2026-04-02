/*
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "M3UParser.h"

#include "InstanceSettings.h"
#include "utilities/Logger.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include <kodi/Filesystem.h>

using namespace iptvsimple;
using namespace iptvsimple::utilities;

// M3U tag markers (same as pvr.iptvsimple PlaylistLoader)
static const std::string CATCHUP          = "catchup=";
static const std::string CATCHUP_TYPE     = "catchup-type=";
static const std::string CATCHUP_DAYS     = "catchup-days=";
static const std::string CATCHUP_SOURCE   = "catchup-source=";
static const std::string CATCHUP_SIPTV    = "timeshift=";
static const std::string CATCHUP_CORRECTION = "catchup-correction=";
static const std::string TVG_NAME         = "tvg-name=";
static const std::string EXTINF           = "#EXTINF";

static std::string ToLower(const std::string& str)
{
  std::string lower = str;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return lower;
}

M3UParser::M3UParser(std::shared_ptr<InstanceSettings>& settings)
  : m_settings(settings) {}

bool M3UParser::Parse()
{
  m_catchupMap.clear();

  const std::string& path = m_settings->GetCatchupM3UPath();
  if (path.empty())
    return false;

  // Read the file — supports local paths and HTTP/HTTPS URLs via Kodi VFS
  std::string content;

  if (path.find("://") != std::string::npos)
  {
    // Network file — use kodi::vfs::CFile
    kodi::vfs::CFile file;
    if (!file.OpenFile(path, 0))
    {
      Logger::Log(LEVEL_ERROR, "%s - Failed to open M3U URL: %s", __FUNCTION__, path.c_str());
      return false;
    }

    char buffer[4096];
    ssize_t bytesRead;
    while ((bytesRead = file.Read(buffer, sizeof(buffer))) > 0)
      content.append(buffer, static_cast<size_t>(bytesRead));
    file.Close();
  }
  else
  {
    // Local file
    std::ifstream file(path);
    if (!file.is_open())
    {
      Logger::Log(LEVEL_ERROR, "%s - Failed to open M3U file: %s", __FUNCTION__, path.c_str());
      return false;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    content = ss.str();
  }

  // Parse line by line
  std::istringstream stream(content);
  std::string line;
  const int defaultCatchupMode = m_settings->GetAllChannelsCatchupMode();
  const int defaultCatchupDays = m_settings->GetCatchupDays();

  while (std::getline(stream, line))
  {
    // Trim \r
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    if (line.find(EXTINF) != 0)
      continue;

    // Extract channel name — everything after the last comma, trimmed
    std::string channelName;
    auto lastComma = line.rfind(',');
    if (lastComma != std::string::npos)
    {
      channelName = line.substr(lastComma + 1);
      // Trim leading/trailing whitespace
      size_t start = channelName.find_first_not_of(" \t");
      size_t end = channelName.find_last_not_of(" \t");
      if (start != std::string::npos)
        channelName = channelName.substr(start, end - start + 1);
      else
        channelName.clear();
    }

    // Also try tvg-name as alternative
    std::string tvgName = ReadMarkerValue(line, TVG_NAME);

    if (channelName.empty() && tvgName.empty())
      continue;

    // Parse catchup tags
    std::string strCatchup = ReadMarkerValue(line, CATCHUP);
    if (strCatchup.empty())
      strCatchup = ReadMarkerValue(line, CATCHUP_TYPE);
    std::string strCatchupDays = ReadMarkerValue(line, CATCHUP_DAYS);
    std::string strCatchupSource = ReadMarkerValue(line, CATCHUP_SOURCE);
    std::string strCatchupSiptv = ReadMarkerValue(line, CATCHUP_SIPTV);
    std::string strCatchupCorrection = ReadMarkerValue(line, CATCHUP_CORRECTION);

    M3UCatchupInfo info;

    // Determine catchup mode
    if (!strCatchup.empty())
    {
      info.mode = ParseCatchupMode(strCatchup);
      info.hasCatchup = (info.mode != CatchupMode::DISABLED);
    }
    else if (!strCatchupSiptv.empty() && std::atoi(strCatchupSiptv.c_str()) > 0)
    {
      // timeshift= is legacy SIPTV format → SHIFT mode (only if days > 0)
      info.mode = CatchupMode::SHIFT;
      info.hasCatchup = true;
      if (strCatchupDays.empty())
        strCatchupDays = strCatchupSiptv;
    }
    else if (defaultCatchupMode > 0)
    {
      info.mode = static_cast<CatchupMode>(defaultCatchupMode);
      info.hasCatchup = true;
    }

    if (!info.hasCatchup)
      continue;

    // Catchup days
    if (!strCatchupDays.empty())
      info.days = std::atoi(strCatchupDays.c_str());
    else
      info.days = defaultCatchupDays;

    // Catchup source
    info.source = strCatchupSource;

    // Correction (in hours, converted to float)
    if (!strCatchupCorrection.empty())
      info.correctionHours = static_cast<float>(std::atof(strCatchupCorrection.c_str()));

    // Store by both channel name and tvg-name (case-insensitive)
    if (!channelName.empty())
      m_catchupMap[ToLower(channelName)] = info;
    if (!tvgName.empty() && ToLower(tvgName) != ToLower(channelName))
      m_catchupMap[ToLower(tvgName)] = info;

    Logger::Log(LEVEL_DEBUG, "%s - Catchup channel '%s': mode=%d, days=%d, source='%s'",
                __FUNCTION__, channelName.c_str(), static_cast<int>(info.mode),
                info.days, info.source.c_str());
  }

  Logger::Log(LEVEL_INFO, "%s - Parsed %d catchup channels from reference M3U",
              __FUNCTION__, static_cast<int>(m_catchupMap.size()));
  return !m_catchupMap.empty();
}

const M3UCatchupInfo* M3UParser::GetCatchupInfo(const std::string& channelName) const
{
  auto it = m_catchupMap.find(ToLower(channelName));
  if (it != m_catchupMap.end())
    return &it->second;
  return nullptr;
}

std::string M3UParser::ReadMarkerValue(const std::string& line, const std::string& marker) const
{
  size_t pos = line.find(marker);
  if (pos == std::string::npos)
    return "";

  pos += marker.length();

  // Value may be quoted or unquoted
  if (pos < line.length() && line[pos] == '"')
  {
    // Quoted value
    pos++;
    size_t end = line.find('"', pos);
    if (end != std::string::npos)
      return line.substr(pos, end - pos);
    return line.substr(pos);
  }
  else
  {
    // Unquoted — read until next space or end
    size_t end = line.find(' ', pos);
    if (end != std::string::npos)
      return line.substr(pos, end - pos);
    // Could also end at comma
    end = line.rfind(',');
    if (end != std::string::npos && end > pos)
      return line.substr(pos, end - pos);
    return line.substr(pos);
  }
}

CatchupMode M3UParser::ParseCatchupMode(const std::string& modeStr) const
{
  std::string mode = ToLower(modeStr);
  if (mode == "default")       return CatchupMode::DEFAULT;
  if (mode == "append")        return CatchupMode::APPEND;
  if (mode == "shift")         return CatchupMode::SHIFT;
  if (mode == "flussonic" || mode == "flussonic-ts" || mode == "fs")
                               return CatchupMode::FLUSSONIC;
  if (mode == "xc")            return CatchupMode::XTREAM_CODES;
  if (mode == "timeshift")     return CatchupMode::TIMESHIFT;
  if (mode == "vod")           return CatchupMode::VOD;
  return CatchupMode::DISABLED;
}
