/*
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 *
 *  Marker parsing helpers (ReadMarkerValue, ParseSinglePropertyIntoMap)
 *  are adapted from pvr.iptvsimple PlaylistLoader (GPL-2.0-or-later,
 *  Copyright (C) 2005-2021 Team Kodi).
 */

#include "M3UParser.h"

#include "InstanceSettings.h"
#include "utilities/Logger.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

#include <kodi/Filesystem.h>
#include <kodi/addon-instance/PVR.h>

using namespace iptvsimple;
using namespace iptvsimple::utilities;

namespace
{

// M3U markers
const std::string EXTM3U                  = "#EXTM3U";
const std::string EXTINF                  = "#EXTINF";
const std::string EXTGRP                  = "#EXTGRP:";
const std::string KODIPROP_MARKER         = "#KODIPROP:";
const std::string EXTVLCOPT_MARKER        = "#EXTVLCOPT:";
const std::string EXTVLCOPT_DASH_MARKER   = "#EXTVLCOPT--";

// Per-channel attribute markers
const std::string TVG_NAME                = "tvg-name=";
const std::string TVG_LOGO                = "tvg-logo=";
const std::string GROUP_NAME              = "group-title=";
const std::string CATCHUP                 = "catchup=";
const std::string CATCHUP_TYPE            = "catchup-type=";
const std::string CATCHUP_DAYS            = "catchup-days=";
const std::string CATCHUP_SOURCE          = "catchup-source=";
const std::string CATCHUP_SIPTV           = "timeshift=";
const std::string CATCHUP_CORRECTION      = "catchup-correction=";

std::string ToLower(const std::string& str)
{
  std::string lower = str;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return lower;
}

void TrimInPlace(std::string& s)
{
  size_t start = s.find_first_not_of(" \t");
  size_t end = s.find_last_not_of(" \t\r\n");
  if (start == std::string::npos)
    s.clear();
  else
    s = s.substr(start, end - start + 1);
}

// Adapted from pvr.iptvsimple PlaylistLoader::ReadMarkerValue.
// For group-title= the value is everything to end-of-line (semicolons are
// the in-value separator); for everything else we stop at space or quote.
std::string ReadMarkerValue(const std::string& line, const std::string& marker)
{
  size_t pos = line.find(marker);
  if (pos == std::string::npos)
    return "";

  pos += marker.length();
  if (pos >= line.length())
    return "";

  if (marker == GROUP_NAME && line[pos] != '"')
    return line.substr(pos);

  char terminator = ' ';
  if (line[pos] == '"')
  {
    terminator = '"';
    pos++;
  }
  size_t end = line.find(terminator, pos);
  if (end == std::string::npos)
    end = line.length();
  return line.substr(pos, end - pos);
}

CatchupMode ParseCatchupMode(const std::string& modeStr)
{
  std::string mode = ToLower(modeStr);
  if (mode == "default")       return CatchupMode::DEFAULT;
  if (mode == "append")        return CatchupMode::APPEND;
  if (mode == "shift")         return CatchupMode::SHIFT;
  if (mode == "flussonic" || mode == "flussonic-hls" ||
      mode == "flussonic-ts" || mode == "fs")
                               return CatchupMode::FLUSSONIC;
  if (mode == "xc")            return CatchupMode::XTREAM_CODES;
  if (mode == "timeshift")     return CatchupMode::TIMESHIFT;
  if (mode == "vod")           return CatchupMode::VOD;
  return CatchupMode::DISABLED;
}

// Adapted from pvr.iptvsimple PlaylistLoader::ParseSinglePropertyIntoChannel.
// Writes filtered key/value into either kodiProps or kofinProps depending on
// the marker and the key. Returns false if the property is rejected.
void ParseSinglePropertyIntoMap(const std::string& line,
                                const std::string& marker,
                                M3UChannelInfo& info)
{
  // For #KODIPROP: and #WEBPROP: the value can contain spaces, so the marker
  // grabs everything to end-of-line.
  std::string value;
  if (marker == KODIPROP_MARKER)
  {
    auto pos = line.find(marker);
    if (pos == std::string::npos)
      return;
    value = line.substr(pos + marker.length());
  }
  else
  {
    value = ReadMarkerValue(line, marker);
  }

  auto eq = value.find('=');
  if (eq == std::string::npos)
    return;

  std::string prop = value.substr(0, eq);
  std::string propValue = value.substr(eq + 1);
  // Trim leading/trailing whitespace (including CR) so values like
  // `kofin-force-remux=true ` (trailing space) parse correctly.
  TrimInPlace(prop);
  TrimInPlace(propValue);
  std::transform(prop.begin(), prop.end(), prop.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  if (marker == EXTVLCOPT_DASH_MARKER)
  {
    // Whitelist — only http-reconnect
    if (prop != "http-reconnect")
      return;
    info.kodiProps[prop] = propValue;
    return;
  }
  if (marker == EXTVLCOPT_MARKER)
  {
    // Whitelist — http-user-agent / http-referrer / program
    if (prop != "http-user-agent" && prop != "http-referrer" && prop != "program")
      return;
    info.kodiProps[prop] = propValue;
    return;
  }

  // KODIPROP_MARKER from here on
  if (prop == "inputstreamaddon" || prop == "inputstreamclass")
    prop = PVR_STREAM_PROPERTY_INPUTSTREAM;

  // KofinProps — never forwarded to Kodi
  if (prop.rfind("kofin-", 0) == 0)
  {
    info.kofinProps[prop] = propValue;
    return;
  }

  // For pvr.kofin we only want inputstream-related KodiProps; Jellyfin
  // already supplies the stream URL and most other knobs.
  if (prop == "inputstream" ||
      prop == PVR_STREAM_PROPERTY_INPUTSTREAM ||
      prop == PVR_STREAM_PROPERTY_MIMETYPE ||
      prop.rfind("inputstream.", 0) == 0)
  {
    info.kodiProps[prop] = propValue;
  }
}

// Split a semicolon list and append unique entries to dest, also tracking
// global ordering.
void SplitGroupNames(const std::string& list,
                     std::vector<std::string>& dest,
                     std::vector<std::string>& allGroups,
                     std::set<std::string>& allGroupSet)
{
  std::stringstream ss(list);
  std::string name;
  while (std::getline(ss, name, ';'))
  {
    TrimInPlace(name);
    if (name.empty())
      continue;
    if (std::find(dest.begin(), dest.end(), name) == dest.end())
      dest.push_back(name);
    if (allGroupSet.insert(name).second)
      allGroups.push_back(name);
  }
}

bool LoadFileContents(const std::string& path, std::string& out)
{
  if (path.find("://") != std::string::npos)
  {
    kodi::vfs::CFile file;
    if (!file.OpenFile(path, 0))
      return false;
    char buffer[4096];
    ssize_t bytesRead;
    while ((bytesRead = file.Read(buffer, sizeof(buffer))) > 0)
      out.append(buffer, static_cast<size_t>(bytesRead));
    file.Close();
    return true;
  }

  std::ifstream file(path);
  if (!file.is_open())
    return false;
  std::ostringstream ss;
  ss << file.rdbuf();
  out = ss.str();
  return true;
}

} // anonymous namespace

M3UParser::M3UParser(std::shared_ptr<InstanceSettings>& settings)
  : m_settings(settings) {}

bool M3UParser::Parse()
{
  m_channels.clear();
  m_allGroupNames.clear();

  const std::string& path = m_settings->GetReferencePlaylistPath();
  if (path.empty())
    return false;

  std::string content;
  if (!LoadFileContents(path, content))
  {
    Logger::Log(LEVEL_ERROR, "%s - Failed to open reference playlist: %s",
                __FUNCTION__, path.c_str());
    return false;
  }

  std::set<std::string> allGroupSet;

  // #EXTGRP: is a begin directive that applies to subsequent entries until
  // a per-entry group-title= overrides or another #EXTGRP: appears.
  std::vector<std::string> currentExtGrpNames;

  // Per-entry pending state — applied when the URL line is seen.
  M3UChannelInfo pending;
  std::string pendingChannelName;
  bool pendingHasEntry = false;
  bool pendingGroupsFromBeginDirective = false;

  const int defaultCatchupMode = m_settings->GetAllChannelsCatchupMode();
  const int defaultCatchupDays = m_settings->GetCatchupDays();

  std::istringstream stream(content);
  std::string line;
  while (std::getline(stream, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    if (line.empty())
      continue;

    if (line.rfind(EXTINF, 0) == 0)
    {
      // Start a new entry. Apply pending if URL was missing (defensive).
      pending = M3UChannelInfo{};
      pendingChannelName.clear();
      pendingHasEntry = true;
      pendingGroupsFromBeginDirective = false;

      // Channel name = text after the last comma.
      auto lastComma = line.rfind(',');
      if (lastComma != std::string::npos)
      {
        pendingChannelName = line.substr(lastComma + 1);
        TrimInPlace(pendingChannelName);
      }

      // tvg-name fallback if comma name is empty
      std::string tvgName = ReadMarkerValue(line, TVG_NAME);
      if (pendingChannelName.empty())
        pendingChannelName = tvgName;

      // tvg-logo override
      pending.tvgLogo = ReadMarkerValue(line, TVG_LOGO);

      // group-title= (per-entry; takes priority over #EXTGRP)
      const std::string groupList = ReadMarkerValue(line, GROUP_NAME);
      if (!groupList.empty())
      {
        SplitGroupNames(groupList, pending.groupNames, m_allGroupNames, allGroupSet);
      }
      else if (!currentExtGrpNames.empty())
      {
        pending.groupNames = currentExtGrpNames;
        pendingGroupsFromBeginDirective = true;
      }

      // Catchup attributes
      std::string strCatchup = ReadMarkerValue(line, CATCHUP);
      if (strCatchup.empty())
        strCatchup = ReadMarkerValue(line, CATCHUP_TYPE);
      const std::string strCatchupDays = ReadMarkerValue(line, CATCHUP_DAYS);
      const std::string strCatchupSource = ReadMarkerValue(line, CATCHUP_SOURCE);
      const std::string strCatchupSiptv = ReadMarkerValue(line, CATCHUP_SIPTV);
      const std::string strCatchupCorrection = ReadMarkerValue(line, CATCHUP_CORRECTION);

      if (!strCatchup.empty())
      {
        pending.catchupMode = ParseCatchupMode(strCatchup);
        pending.hasCatchup = (pending.catchupMode != CatchupMode::DISABLED);
      }
      else if (!strCatchupSiptv.empty() && std::atoi(strCatchupSiptv.c_str()) > 0)
      {
        pending.catchupMode = CatchupMode::SHIFT;
        pending.hasCatchup = true;
      }
      else if (defaultCatchupMode > 0)
      {
        pending.catchupMode = static_cast<CatchupMode>(defaultCatchupMode);
        pending.hasCatchup = true;
      }

      if (!strCatchupDays.empty())
        pending.catchupDays = std::atoi(strCatchupDays.c_str());
      else if (!strCatchupSiptv.empty())
        pending.catchupDays = std::atoi(strCatchupSiptv.c_str());
      else
        pending.catchupDays = defaultCatchupDays;

      pending.catchupSource = strCatchupSource;
      if (!strCatchupCorrection.empty())
        pending.catchupCorrectionHours = static_cast<float>(std::atof(strCatchupCorrection.c_str()));

      continue;
    }

    if (line.rfind(KODIPROP_MARKER, 0) == 0 && pendingHasEntry)
    {
      ParseSinglePropertyIntoMap(line, KODIPROP_MARKER, pending);
      continue;
    }
    if (line.rfind(EXTVLCOPT_DASH_MARKER, 0) == 0 && pendingHasEntry)
    {
      ParseSinglePropertyIntoMap(line, EXTVLCOPT_DASH_MARKER, pending);
      continue;
    }
    if (line.rfind(EXTVLCOPT_MARKER, 0) == 0 && pendingHasEntry)
    {
      ParseSinglePropertyIntoMap(line, EXTVLCOPT_MARKER, pending);
      continue;
    }

    if (line.rfind(EXTGRP, 0) == 0)
    {
      currentExtGrpNames.clear();
      const std::string list = ReadMarkerValue(line, EXTGRP);
      if (!list.empty())
        SplitGroupNames(list, currentExtGrpNames, m_allGroupNames, allGroupSet);
      // If a pending entry has no per-entry groups yet, adopt the new
      // begin-directive list.
      if (pendingHasEntry && pending.groupNames.empty())
      {
        pending.groupNames = currentExtGrpNames;
        pendingGroupsFromBeginDirective = true;
      }
      continue;
    }

    if (line[0] == '#' || line[0] == '\0')
      continue;

    // URL line — finalize pending entry.
    if (pendingHasEntry && !pendingChannelName.empty())
    {
      // Persist by lowercase channel name. If duplicate, last one wins.
      m_channels[ToLower(pendingChannelName)] = pending;

      Logger::Log(LEVEL_DEBUG,
                  "%s - Parsed '%s': groups=%zu kodiProps=%zu kofinProps=%zu catchup=%s",
                  __FUNCTION__, pendingChannelName.c_str(),
                  pending.groupNames.size(), pending.kodiProps.size(),
                  pending.kofinProps.size(),
                  pending.hasCatchup ? "yes" : "no");
    }

    // Reset pending; clear group adoption flag so next entry without groups
    // picks up the current #EXTGRP directive again.
    pendingHasEntry = false;
    pendingChannelName.clear();
    pending = M3UChannelInfo{};
    if (!pendingGroupsFromBeginDirective)
      pendingGroupsFromBeginDirective = false; // explicit reset
  }

  Logger::Log(LEVEL_INFO,
              "%s - Parsed %zu entries, %zu unique groups from reference playlist",
              __FUNCTION__, m_channels.size(), m_allGroupNames.size());
  return !m_channels.empty();
}

const M3UChannelInfo* M3UParser::GetChannelInfo(const std::string& channelName) const
{
  auto it = m_channels.find(ToLower(channelName));
  if (it != m_channels.end())
    return &it->second;
  return nullptr;
}
