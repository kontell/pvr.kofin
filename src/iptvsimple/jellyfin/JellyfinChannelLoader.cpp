/*
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "JellyfinChannelLoader.h"

#include "../M3UParser.h"
#include "../utilities/JsonUtils.h"
#include "../utilities/Logger.h"
#include "../utilities/TimeUtils.h"
#include "../utilities/WebUtils.h"

#include <kodi/Filesystem.h>
#include <kodi/General.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>

using namespace iptvsimple;
using namespace iptvsimple::data;
using namespace iptvsimple::jellyfin;
using namespace iptvsimple::utilities;

namespace
{
bool ParseBoolProp(const std::string& value)
{
  std::string v = value;
  std::transform(v.begin(), v.end(), v.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return v == "true" || v == "1" || v == "yes" || v == "on";
}
} // unnamed namespace

ChannelOverrides ChannelOverrides::FromChannel(const Channel& channel)
{
  ChannelOverrides o;
  const auto& props = channel.GetProperties();

  auto get = [&](const std::string& key) -> const std::string* {
    auto it = props.find(key);
    return (it != props.end()) ? &it->second : nullptr;
  };

  if (const auto* v = get("kofin-force-remux"))
    o.forceRemux = ParseBoolProp(*v);
  if (const auto* v = get("kofin-force-transcode"))
    o.forceTranscode = ParseBoolProp(*v);
  if (const auto* v = get("kofin-force-direct-play"))
    o.forceDirectPlay = ParseBoolProp(*v);
  if (const auto* v = get("kofin-bitrate-limit"))
  {
    int kbps = std::atoi(v->c_str());
    if (kbps > 0)
      o.bitrateBps = kbps * 1000;
    else
      o.bitrateBps = 1000000000; // unlimited sentinel matching GetMaxBitrateBps()
  }

  // Inputstream override drives container selection in BuildDeviceProfile.
  if (const auto* v = get(PVR_STREAM_PROPERTY_INPUTSTREAM))
    o.inputstream = *v;
  else if (const auto* v = get("inputstream"))
    o.inputstream = *v;

  return o;
}

JellyfinChannelLoader::JellyfinChannelLoader(
    std::shared_ptr<JellyfinClient> client,
    std::shared_ptr<iptvsimple::InstanceSettings> settings)
  : m_client(client), m_settings(settings)
{
}


bool JellyfinChannelLoader::LoadChannels(Channels& channels, ChannelGroups& channelGroups)
{
  // Exception firewall: jsoncpp accessors throw Json::LogicError on
  // unexpected types. This runs on a worker std::thread, where an escaped
  // exception means std::terminate — treat it as a failed load instead.
  try
  {
    return LoadChannelsInternal(channels, channelGroups);
  }
  catch (const std::exception& e)
  {
    Logger::Log(LEVEL_ERROR, "%s - Exception parsing channel data: %s", __FUNCTION__, e.what());
    channels.ChannelsLoadFailed();
    return false;
  }
}

bool JellyfinChannelLoader::LoadChannelsInternal(Channels& channels, ChannelGroups& channelGroups)
{
  Logger::Log(LEVEL_INFO, "%s - Loading channels from Jellyfin", __FUNCTION__);

  const std::string endpoint = "/LiveTv/Channels?UserId=" + m_client->GetUserId()
    + "&EnableImages=true&SortBy=SortName&Limit=1000";

  Json::Value response = m_client->SendGet(endpoint);

  if (response.isNull() || !response.isMember("Items"))
  {
    Logger::Log(LEVEL_ERROR, "%s - Failed to load channels from Jellyfin", __FUNCTION__);
    channels.ChannelsLoadFailed();
    return false;
  }

  const Json::Value& items = response["Items"];

  if (items.empty())
  {
    Logger::Log(LEVEL_WARNING, "%s - No channels returned from Jellyfin", __FUNCTION__);
    return true;
  }

  // Parse the reference playlist up-front so its data is available when each
  // channel is added to the channels/groups stores.
  M3UParser m3uParser(m_settings);
  bool m3uLoaded = false;
  if (m_settings->ReferencePlaylistEnabled() && !m_settings->GetReferencePlaylistPath().empty())
    m3uLoaded = m3uParser.Parse();

  // Always create the "Jellyfin" all-channels group.
  ChannelGroup allGroup;
  allGroup.SetGroupName("Jellyfin");
  allGroup.SetRadio(false);
  const int jellyfinGroupId = channelGroups.AddChannelGroup(allGroup);

  // Pre-create groups discovered in the reference playlist (preserves order).
  std::map<std::string, int> m3uGroupIdByName;
  if (m3uLoaded)
  {
    for (const auto& name : m3uParser.GetAllGroupNames())
    {
      ChannelGroup g;
      g.SetGroupName(name);
      g.SetRadio(false);
      m3uGroupIdByName[name] = channelGroups.AddChannelGroup(g);
    }
  }

  int channelNumber = 1;
  int matchedCount = 0;

  // Build the lookup maps into locals, then swap them in under m_dataMutex at
  // the end, so readers on other threads never observe a half-rebuilt map.
  std::map<std::string, int> jellyfinIdToUid;
  std::map<int, std::string> uidToJellyfinId;

  for (const auto& item : items)
  {
    const std::string jellyfinId = item["Id"].asString();
    const std::string name = item["Name"].asString();

    Channel channel(m_settings);
    channel.SetChannelName(name);

    // Parse channel number (supports "5.1" format)
    if (item.isMember("ChannelNumber") && !item["ChannelNumber"].asString().empty())
    {
      const std::string numStr = item["ChannelNumber"].asString();
      size_t dotPos = numStr.find('.');
      if (dotPos != std::string::npos)
      {
        channel.SetChannelNumber(std::atoi(numStr.substr(0, dotPos).c_str()));
        channel.SetSubChannelNumber(std::atoi(numStr.substr(dotPos + 1).c_str()));
      }
      else
      {
        channel.SetChannelNumber(std::atoi(numStr.c_str()));
      }
    }
    else
    {
      channel.SetChannelNumber(channelNumber);
    }
    channelNumber++;

    // Store Jellyfin ID for EPG and stream lookups
    channel.SetTvgId(jellyfinId);

    // Channel icon from Jellyfin
    if (item.isMember("ImageTags") && item["ImageTags"].isMember("Primary"))
      channel.SetIconPath(m_client->BuildImageUrl(jellyfinId, item["ImageTags"]["Primary"].asString()));

    // Stream URL placeholder - actual URL resolved via PlaybackInfo in GetChannelStreamProperties
    channel.SetStreamURL(m_client->GetBaseUrl() + "/LiveTv/Channels/" + jellyfinId);

    // Always-include the "Jellyfin" group; M3U groups are added per-match.
    std::vector<int> groupIdList = {jellyfinGroupId};

    // Apply reference-playlist data
    if (m3uLoaded)
    {
      const M3UChannelInfo* info = m3uParser.GetChannelInfo(name);
      if (info)
      {
        // Catchup (only when catchup is enabled by the user)
        if (m_settings->CatchupEnabled() && info->hasCatchup)
        {
          channel.SetHasCatchup(true);
          channel.SetCatchupMode(info->catchupMode);
          channel.SetCatchupDays(info->catchupDays > 0 ? info->catchupDays : m_settings->GetCatchupDays());
          if (!info->catchupSource.empty())
            channel.SetCatchupSource(info->catchupSource);
          if (info->catchupCorrectionHours != 0.0f)
            channel.SetCatchupCorrectionSecs(static_cast<int>(info->catchupCorrectionHours * 3600.0f));
          channel.ConfigureCatchupMode();
        }

        // KodiProps (inputstream.*, mimetype, http-*)
        for (const auto& kv : info->kodiProps)
          channel.AddProperty(kv.first, kv.second);

        // KofinProps (kofin-*) — stored on the channel; consumed by ChannelOverrides::FromChannel
        for (const auto& kv : info->kofinProps)
          channel.AddProperty(kv.first, kv.second);

        // Logo override
        if (!info->tvgLogo.empty())
          channel.SetIconPath(info->tvgLogo);

        // M3U-defined groups (additive to the Jellyfin fallback)
        for (const auto& gname : info->groupNames)
        {
          auto it = m3uGroupIdByName.find(gname);
          if (it != m3uGroupIdByName.end())
            groupIdList.push_back(it->second);
        }

        matchedCount++;
      }
    }

    if (channels.AddChannel(channel, groupIdList, channelGroups, true))
    {
      int uid = channel.GetUniqueId();
      jellyfinIdToUid[jellyfinId] = uid;
      uidToJellyfinId[uid] = jellyfinId;

      Logger::Log(LEVEL_DEBUG, "%s - Added channel '%s' (uid=%d, jellyfinId=%s, groups=%zu)",
                  __FUNCTION__, name.c_str(), uid, jellyfinId.c_str(), groupIdList.size());
    }
  }

  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    m_jellyfinIdToUid = std::move(jellyfinIdToUid);
    m_uidToJellyfinId = std::move(uidToJellyfinId);
  }

  channelGroups.RemoveEmptyGroups();

  Logger::Log(LEVEL_INFO,
              "%s - Loaded %d channels from Jellyfin (%d matched in reference playlist)",
              __FUNCTION__, static_cast<int>(items.size()), matchedCount);
  return true;
}

PVR_ERROR JellyfinChannelLoader::LoadEpg(int channelUid, time_t start, time_t end,
                                          kodi::addon::PVREPGTagsResultSet& results)
{
  // Exception firewall: called directly from Kodi's GetEPGForChannel — a
  // jsoncpp type error must not cross the C ABI.
  try
  {
    return LoadEpgInternal(channelUid, start, end, results);
  }
  catch (const std::exception& e)
  {
    Logger::Log(LEVEL_ERROR, "%s - Exception parsing EPG data: %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

PVR_ERROR JellyfinChannelLoader::LoadEpgInternal(int channelUid, time_t start, time_t end,
                                                 kodi::addon::PVREPGTagsResultSet& results)
{
  std::string jellyfinId;
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    auto it = m_uidToJellyfinId.find(channelUid);
    if (it == m_uidToJellyfinId.end())
    {
      Logger::Log(LEVEL_ERROR, "%s - No Jellyfin ID for channel UID %d", __FUNCTION__, channelUid);
      return PVR_ERROR_INVALID_PARAMETERS;
    }
    jellyfinId = it->second;
  }

  // Use MaxStartDate (not MaxEndDate) - Jellyfin API returns programmes that start before this time
  const std::string endpoint = "/LiveTv/Programs?ChannelIds=" + jellyfinId
    + "&UserId=" + m_client->GetUserId()
    + "&MinStartDate=" + WebUtils::UrlEncode(FormatIso8601(start))
    + "&MaxStartDate=" + WebUtils::UrlEncode(FormatIso8601(end))
    + "&Fields=Overview,ChannelInfo"
    + "&EnableImages=true"
    + "&SortBy=StartDate"
    + "&Limit=500";

  Json::Value response = m_client->SendGet(endpoint);

  if (response.isNull() || !response.isMember("Items"))
  {
    Logger::Log(LEVEL_WARNING, "%s - No EPG data for channel %d", __FUNCTION__, channelUid);
    return PVR_ERROR_NO_ERROR;
  }

  const Json::Value& items = response["Items"];

  for (const auto& item : items)
  {
    kodi::addon::PVREPGTag tag;

    const std::string jellyfinProgramId = item["Id"].asString();
    const unsigned int broadcastUid = static_cast<unsigned int>(GenerateUid(jellyfinProgramId));
    {
      std::lock_guard<std::mutex> lock(m_dataMutex);
      m_epgUidToJellyfinProgramId[broadcastUid] = jellyfinProgramId;
    }

    tag.SetUniqueBroadcastId(broadcastUid);
    tag.SetUniqueChannelId(static_cast<unsigned int>(channelUid));
    tag.SetTitle(item.get("Name", "").asString());

    if (item.isMember("EpisodeTitle"))
      tag.SetEpisodeName(item["EpisodeTitle"].asString());

    if (item.isMember("Overview"))
      tag.SetPlot(item["Overview"].asString());

    if (item.isMember("ShortOverview"))
      tag.SetPlotOutline(item["ShortOverview"].asString());

    // Parse start/end times
    if (item.isMember("StartDate"))
      tag.SetStartTime(ParseIso8601(item["StartDate"].asString()));

    if (item.isMember("EndDate"))
      tag.SetEndTime(ParseIso8601(item["EndDate"].asString()));

    // Image
    if (item.isMember("ImageTags") && item["ImageTags"].isMember("Primary"))
      tag.SetIconPath(m_client->BuildImageUrl(item["Id"].asString(),
                                               item["ImageTags"]["Primary"].asString()));

    // Season/Episode
    if (item.isMember("IndexNumber"))
      tag.SetEpisodeNumber(SafeInt(item["IndexNumber"]));
    if (item.isMember("ParentIndexNumber"))
      tag.SetSeriesNumber(SafeInt(item["ParentIndexNumber"]));

    // Year
    if (item.isMember("ProductionYear"))
      tag.SetYear(SafeInt(item["ProductionYear"]));

    // Genre
    if (item.isMember("Genres") && item["Genres"].isArray() && !item["Genres"].empty())
      tag.SetGenreDescription(item["Genres"][0].asString());

    // Flags
    unsigned int flags = 0;
    if (item.isMember("IsNew") && item["IsNew"].asBool())
      flags |= EPG_TAG_FLAG_IS_NEW;
    if (item.isMember("IsPremiere") && item["IsPremiere"].asBool())
      flags |= EPG_TAG_FLAG_IS_PREMIERE;
    if (flags)
      tag.SetFlags(flags);

    results.Add(tag);
  }

  Logger::Log(LEVEL_DEBUG, "%s - Loaded %d EPG entries for channel %d",
              __FUNCTION__, static_cast<int>(items.size()), channelUid);
  return PVR_ERROR_NO_ERROR;
}

Json::Value JellyfinChannelLoader::BuildDeviceProfile(const ChannelOverrides& overrides)
{
  const std::string preferredVideo = m_settings->GetPreferredVideoCodecName();
  const std::string preferredAudio = m_settings->GetPreferredAudioCodecName();

  // Per-channel overrides win over global settings; otherwise fall back.
  const bool forceDirectPlay = overrides.forceDirectPlay.value_or(m_settings->GetForceDirectPlay());
  const int maxBitrateBps = forceDirectPlay ? 1000000000
      : overrides.bitrateBps.value_or(m_settings->GetMaxBitrateBps());
  const bool forceRemux = overrides.forceRemux.value_or(m_settings->GetForceTranscode());
  const bool forceTranscodeActive = overrides.forceTranscode.value_or(m_settings->GetForceTranscoding());
  const bool bitrateUnlimited = (maxBitrateBps >= 1000000000);

  Json::Value profile;
  profile["Name"] = "Kodi";
  profile["MaxStreamingBitrate"] = maxBitrateBps;
  profile["MaxStaticBitrate"] = maxBitrateBps;
  profile["MusicStreamingTranscodingBitrate"] = 1280000;
  profile["TimelineOffsetSeconds"] = 5;

  // Build audio codec list: preferred codec first, then the rest from the
  // allowed list. Codecs not in the list will be transcoded.
  const std::string& allowedAudio = m_settings->GetDirectPlayAudioCodecs();
  std::string audioCodecs = preferredAudio;
  std::string::size_type aStart = 0;
  while (aStart < allowedAudio.length())
  {
    auto aEnd = allowedAudio.find(',', aStart);
    if (aEnd == std::string::npos)
      aEnd = allowedAudio.length();
    std::string codec = allowedAudio.substr(aStart, aEnd - aStart);
    if (codec != preferredAudio)
      audioCodecs += "," + codec;
    aStart = aEnd + 1;
  }

  // Allowed video codecs from settings. Virtual entries h264_10bit and
  // hevc_rext are mapped to their real codec names; they control CodecProfile
  // conditions below rather than adding separate codec entries.
  const std::string& rawVideoCodecs = m_settings->GetDirectPlayVideoCodecs();

  // Parse comma-delimited tokens into a set for exact matching.
  std::set<std::string> videoTokens;
  {
    std::string::size_type s = 0;
    while (s < rawVideoCodecs.length())
    {
      auto e = rawVideoCodecs.find(',', s);
      if (e == std::string::npos)
        e = rawVideoCodecs.length();
      videoTokens.insert(rawVideoCodecs.substr(s, e - s));
      s = e + 1;
    }
  }

  const bool h264Allowed = videoTokens.count("h264") > 0;
  const bool h264_10bitAllowed = videoTokens.count("h264_10bit") > 0;
  const bool hevcAllowed = videoTokens.count("hevc") > 0;
  const bool hevcRextAllowed = videoTokens.count("hevc_rext") > 0;

  // Build the actual codec CSV for the device profile, mapping virtual
  // entries back to real codec names.
  std::string allowedVideoCodecsCsv;
  auto addCodec = [&](const std::string& codec) {
    if (!allowedVideoCodecsCsv.empty())
      allowedVideoCodecsCsv += ",";
    allowedVideoCodecsCsv += codec;
  };
  if (h264Allowed || h264_10bitAllowed)
    addCodec("h264");
  if (hevcAllowed || hevcRextAllowed)
    addCodec("hevc");
  for (const auto& token : videoTokens)
  {
    if (token != "h264" && token != "h264_10bit" && token != "hevc" && token != "hevc_rext")
      addCodec(token);
  }

  // inputstream.adaptive rejects single-segment transcode playlists with
  // "Codec id NN require extradata". Remux segments are instant and work
  // with 1; transcode needs 3 when adaptive is the inputstream.
  const bool useAdaptive = overrides.inputstream.value_or("") == "inputstream.adaptive";
  const std::string minSegs = useAdaptive ? "3" : "1";
  const std::string maxCh = std::to_string(m_settings->GetMaxAudioChannels());

  // Build TS codec list: all allowed codecs except AV1, which can't ride
  // MPEG-TS and gets its own fMP4 profile. The leading codec is what a forced
  // transcode targets, so it should be the most efficient TS-capable codec:
  // HEVC when AV1 is preferred (AV1 itself is excluded here), otherwise the
  // preferred codec. Falls back to the preferred codec when HEVC isn't allowed.
  std::string tsVideoCodecs;
  {
    const bool hevcInList = hevcAllowed || hevcRextAllowed;
    const std::string tsLead =
        (preferredVideo == "av1" && hevcInList) ? "hevc" : preferredVideo;
    auto addCodecToList = [&](const std::string& codec) {
      if (codec == "av1") return;
      if (!tsVideoCodecs.empty()) tsVideoCodecs += ",";
      tsVideoCodecs += codec;
    };
    addCodecToList(tsLead);
    std::string::size_type vStart = 0;
    while (vStart < allowedVideoCodecsCsv.length())
    {
      auto vEnd = allowedVideoCodecsCsv.find(',', vStart);
      if (vEnd == std::string::npos)
        vEnd = allowedVideoCodecsCsv.length();
      std::string codec = allowedVideoCodecsCsv.substr(vStart, vEnd - vStart);
      if (codec != tsLead)
        addCodecToList(codec);
      vStart = vEnd + 1;
    }
  }

  // Two TranscodingProfiles: fMP4 for AV1, TS for everything else.
  // The server ranks profiles by codec-copy compatibility and picks the
  // best match. Order controls preference when ranks are equal.
  Json::Value fmp4Profile;
  fmp4Profile["Container"] = "mp4";
  fmp4Profile["Type"] = "Video";
  fmp4Profile["AudioCodec"] = audioCodecs;
  fmp4Profile["VideoCodec"] = "av1";
  fmp4Profile["Context"] = "Streaming";
  fmp4Profile["Protocol"] = "hls";
  fmp4Profile["MaxAudioChannels"] = maxCh;
  fmp4Profile["MinSegments"] = minSegs;
  fmp4Profile["BreakOnNonKeyFrames"] = true;

  Json::Value tsProfile;
  tsProfile["Container"] = "ts";
  tsProfile["Type"] = "Video";
  tsProfile["AudioCodec"] = audioCodecs;
  tsProfile["VideoCodec"] = tsVideoCodecs;
  tsProfile["Context"] = "Streaming";
  tsProfile["Protocol"] = "hls";
  tsProfile["MaxAudioChannels"] = maxCh;
  tsProfile["MinSegments"] = minSegs;
  tsProfile["BreakOnNonKeyFrames"] = true;

  Json::Value transcodingProfiles(Json::arrayValue);
  if (preferredVideo == "av1")
  {
    transcodingProfiles.append(fmp4Profile);
    transcodingProfiles.append(tsProfile);
  }
  else
  {
    transcodingProfiles.append(tsProfile);
    transcodingProfiles.append(fmp4Profile);
  }
  profile["TranscodingProfiles"] = transcodingProfiles;

  // Direct play only when neither remux nor transcode is forced AND no
  // bitrate limit. For live TV (Protocol=Http), Jellyfin ignores
  // MaxStreamingBitrate for DirectPlay decisions, so we must clear
  // DirectPlayProfiles to force TranscodingProfiles when a limit is set.
  // DirectPlay video codecs: allowed list + preferred (if not already in it).
  // Setting a preferred codec implies the device can decode it.
  std::string directPlayVideoCodecs = allowedVideoCodecsCsv;
  if (allowedVideoCodecsCsv.find(preferredVideo) == std::string::npos)
  {
    if (!directPlayVideoCodecs.empty())
      directPlayVideoCodecs += ",";
    directPlayVideoCodecs += preferredVideo;
  }

  Json::Value directPlayProfiles(Json::arrayValue);
  if (forceDirectPlay)
  {
    Json::Value v;
    v["Type"] = "Video";
    v["Container"] = "";
    v["VideoCodec"] = "";
    v["AudioCodec"] = "";
    directPlayProfiles.append(v);
    Json::Value a;
    a["Type"] = "Audio";
    directPlayProfiles.append(a);
  }
  else if (!forceRemux && !forceTranscodeActive && bitrateUnlimited && !directPlayVideoCodecs.empty())
  {
    Json::Value v;
    v["Type"] = "Video";
    v["VideoCodec"] = directPlayVideoCodecs;
    v["AudioCodec"] = audioCodecs;
    directPlayProfiles.append(v);
    Json::Value a;
    a["Type"] = "Audio";
    directPlayProfiles.append(a);
  }
  profile["DirectPlayProfiles"] = directPlayProfiles;

  // Codec profiles: constrain direct play to sub-variants the device can decode.
  // h264 without h264_10bit → limit to 8-bit; hevc without hevc_rext → limit to main/main10.
  Json::Value codecProfiles(Json::arrayValue);
  if (h264Allowed && !h264_10bitAllowed)
  {
    Json::Value cp;
    cp["Type"] = "Video";
    cp["Codec"] = "h264";
    Json::Value cond;
    cond["Condition"] = "LessThanEqual";
    cond["Property"] = "VideoBitDepth";
    cond["Value"] = "8";
    cp["Conditions"] = Json::Value(Json::arrayValue);
    cp["Conditions"].append(cond);
    codecProfiles.append(cp);
  }
  if (hevcAllowed && !hevcRextAllowed)
  {
    Json::Value cp;
    cp["Type"] = "Video";
    cp["Codec"] = "hevc";
    Json::Value cond;
    cond["Condition"] = "EqualsAny";
    cond["Property"] = "VideoProfile";
    cond["Value"] = "main|main 10";
    cp["Conditions"] = Json::Value(Json::arrayValue);
    cp["Conditions"].append(cond);
    codecProfiles.append(cp);
  }

  // HDR: advertise which VideoRangeTypes the display accepts, per codec. The
  // server tone-maps/transcodes formats the user didn't select; SDR is always
  // allowed. These are separate CodecProfiles (Jellyfin ANDs them with the
  // bit-depth/profile ones above). When every type is selected (the default),
  // nothing is emitted — identical to prior behavior.
  if (!forceDirectPlay)
  {
    static const std::set<std::string> kAllHdr = {
      "HDR10", "HLG", "HDR10Plus", "DOVI", "DOVIWithHDR10", "DOVIWithHLG",
      "DOVIWithSDR", "DOVIWithEL", "DOVIWithHDR10Plus", "DOVIWithELHDR10Plus"};
    static const std::set<std::string> kVp9Hdr = {"HDR10", "HLG", "HDR10Plus"};

    // Parse the selection CSV, keeping only known tokens.
    std::set<std::string> hdrSel;
    {
      const std::string& raw = m_settings->GetAllowedHdrTypes();
      std::string::size_type s = 0;
      while (s < raw.length())
      {
        auto e = raw.find(',', s);
        if (e == std::string::npos)
          e = raw.length();
        std::string tok = raw.substr(s, e - s);
        if (kAllHdr.count(tok))
          hdrSel.insert(tok);
        s = e + 1;
      }
    }

    // hevc and av1 share the full capability set: av1 has no Profile-7
    // enhancement layer, but the EL tokens are harmless (no av1 source ever
    // classifies as EL). vp9 carries no Dolby Vision.
    auto emitHdr = [&](const char* codec, const std::set<std::string>& cap,
                       bool canDovi, bool present) {
      if (!present)
        return;
      // Only restrict when the user deselected something this codec can carry.
      bool restricts = false;
      for (const auto& t : cap)
        if (!hdrSel.count(t)) { restricts = true; break; }
      if (!restricts)
        return;
      std::string value = "SDR"; // always allowed; must lead the list
      for (const auto& t : kAllHdr) // iterate canonical set for deterministic order
        if (cap.count(t) && hdrSel.count(t))
          value += "|" + t;
      if (canDovi && hdrSel.count("HDR10"))
        value += "|DOVIInvalid"; // invalid DV is served as its HDR10 base layer
      Json::Value cp;
      cp["Type"] = "Video";
      cp["Codec"] = codec;
      Json::Value cond;
      cond["Condition"] = "EqualsAny";
      cond["Property"] = "VideoRangeType";
      cond["Value"] = value;
      cond["IsRequired"] = false;
      cp["Conditions"] = Json::Value(Json::arrayValue);
      cp["Conditions"].append(cond);
      codecProfiles.append(cp);
    };

    emitHdr("hevc", kAllHdr, true, hevcAllowed || hevcRextAllowed);
    emitHdr("av1", kAllHdr, true, videoTokens.count("av1") > 0);
    emitHdr("vp9", kVp9Hdr, false, videoTokens.count("vp9") > 0);
  }

  // Maximum resolution: cap by width (Jellyfin uses width only). A codec-less
  // Video CodecProfile applies to all video codecs; wider sources are
  // downscaled by the server. Unlimited (0) or force-direct-play emits nothing.
  const int maxWidth = forceDirectPlay ? 0 : m_settings->GetMaxWidth();
  if (maxWidth > 0)
  {
    Json::Value cp;
    cp["Type"] = "Video";
    Json::Value cond;
    cond["Condition"] = "LessThanEqual";
    cond["Property"] = "Width";
    cond["Value"] = std::to_string(maxWidth);
    cond["IsRequired"] = false;
    cp["Conditions"] = Json::Value(Json::arrayValue);
    cp["Conditions"].append(cond);
    codecProfiles.append(cp);
  }
  profile["CodecProfiles"] = codecProfiles;

  // Subtitle profiles
  Json::Value subtitleProfiles(Json::arrayValue);
  for (const char* fmt : {"srt", "ass", "sub", "ssa", "smi", "pgssub", "dvdsub", "pgs"})
  {
    for (const char* method : {"Embed", "External"})
    {
      Json::Value sp;
      sp["Format"] = fmt;
      sp["Method"] = method;
      subtitleProfiles.append(sp);
    }
  }
  profile["SubtitleProfiles"] = subtitleProfiles;

  profile["ResponseProfiles"] = Json::Value(Json::arrayValue);
  profile["ContainerProfiles"] = Json::Value(Json::arrayValue);

  return profile;
}

std::string JellyfinChannelLoader::GetRecordingStreamUrl(
    const std::string& recordingId, bool inProgress,
    const ChannelOverrides& overrides)
{
  // Exception firewall: reached from Kodi's GetRecordingStreamProperties.
  try
  {
    return GetRecordingStreamUrlInternal(recordingId, inProgress, overrides);
  }
  catch (const std::exception& e)
  {
    Logger::Log(LEVEL_ERROR, "%s - Exception parsing PlaybackInfo: %s", __FUNCTION__, e.what());
    return "";
  }
}

std::string JellyfinChannelLoader::GetRecordingStreamUrlInternal(
    const std::string& recordingId, bool inProgress,
    const ChannelOverrides& overrides)
{
  Logger::Log(LEVEL_DEBUG, "%s - Getting recording stream URL for %s", __FUNCTION__, recordingId.c_str());

  // Close previous live stream if any
  CloseLiveStream();

  const std::string endpoint = "/Items/" + recordingId + "/PlaybackInfo";

  Json::Value body;
  body["UserId"] = m_client->GetUserId();
  ChannelOverrides effectiveOverrides = overrides;
  if (inProgress)
  {
    effectiveOverrides.forceRemux = true;
    effectiveOverrides.forceDirectPlay = false;
  }
  Json::Value deviceProfile = BuildDeviceProfile(effectiveOverrides);
  if (inProgress)
    deviceProfile["DirectPlayProfiles"] = Json::Value(Json::arrayValue);
  // Recordings play via inputstream.adaptive which needs MinSegments >= 3.
  if (!deviceProfile["TranscodingProfiles"].empty())
    deviceProfile["TranscodingProfiles"][0]["MinSegments"] = "3";
  body["DeviceProfile"] = deviceProfile;
  m_activeMaxBitrateBps = effectiveOverrides.bitrateBps.value_or(m_settings->GetMaxBitrateBps());
  body["AutoOpenLiveStream"] = true;

  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  const std::string bodyStr = Json::writeString(writer, body);

  Logger::Log(LEVEL_DEBUG, "%s - PlaybackInfo request body: %s", __FUNCTION__, bodyStr.c_str());

  Json::Value response = m_client->SendPost(endpoint, bodyStr);

  if (response.isNull() || !response.isMember("MediaSources") || response["MediaSources"].empty())
  {
    Logger::Log(LEVEL_ERROR, "%s - PlaybackInfo returned no media sources for recording %s",
                __FUNCTION__, recordingId.c_str());
    return "";
  }

  const Json::Value& source = response["MediaSources"][0];

  m_activeSourceBitrateBps = SafeInt(source["Bitrate"]);

  // Track session state for reporting and cleanup
  m_activeLiveStreamId = source.get("LiveStreamId", "").asString();
  m_activePlaySessionId = response.get("PlaySessionId", "").asString();
  m_activeMediaSourceId = source.get("Id", "").asString();
  m_activeItemId = recordingId;

  const bool hasTranscodingUrl = source.isMember("TranscodingUrl")
    && !source["TranscodingUrl"].asString().empty();
  m_activePlayMethod = hasTranscodingUrl ? "Transcode" : "DirectPlay";
  m_activeIsRecording = true;

  Logger::Log(LEVEL_DEBUG, "%s - LiveStreamId: %s, PlayMethod: %s, TranscodingUrl: %s",
              __FUNCTION__, m_activeLiveStreamId.c_str(), m_activePlayMethod.c_str(),
              hasTranscodingUrl ? "present" : "absent");

  std::string streamUrl;
  const bool forceTranscodeActive = effectiveOverrides.forceTranscode.value_or(m_settings->GetForceTranscoding());

  if (hasTranscodingUrl)
  {
    streamUrl = PostProcessTranscodingUrl(source["TranscodingUrl"].asString(), true, forceTranscodeActive);
  }
  else
  {
    streamUrl = m_client->GetBaseUrl() + "/Videos/" + recordingId
      + "/stream?static=true&ApiKey=" + m_client->GetAccessToken();
  }

  RewriteLocalhost(streamUrl);

  const std::string baseUrl = m_client->GetBaseUrl();
  if (streamUrl.find(baseUrl) == 0 &&
      streamUrl.find("ApiKey=") == std::string::npos)
  {
    streamUrl += (streamUrl.find('?') != std::string::npos ? "&" : "?");
    streamUrl += "ApiKey=" + m_client->GetAccessToken();
  }

  Logger::Log(LEVEL_DEBUG, "%s - Recording stream URL: %s", __FUNCTION__,
              WebUtils::RedactUrl(streamUrl).c_str());

  WriteSessionFile();

  return streamUrl;
}

void JellyfinChannelLoader::WriteSessionFile()
{
  const std::string path = m_settings->GetUserPath() + "session.json";
  Json::Value session;
  session["ItemId"] = m_activeItemId;
  session["MediaSourceId"] = m_activeMediaSourceId;
  session["PlaySessionId"] = m_activePlaySessionId;
  session["LiveStreamId"] = m_activeLiveStreamId;
  session["PlayMethod"] = m_activePlayMethod;
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  const std::string data = Json::writeString(writer, session);
  kodi::vfs::CFile file;
  if (file.OpenFileForWrite(path, true))
  {
    file.Write(data.c_str(), data.size());
    file.Close();
  }
}

std::string JellyfinChannelLoader::PostProcessTranscodingUrl(
    const std::string& transcodingUrl, bool keepMaster, bool forceTranscode)
{
  // Post-process TranscodingUrl to match jellyfin-kodi behaviour:
  // - For ffmpegdirect/internal: replace "stream"/"master" with "live" so we
  //   land on Jellyfin's bare media playlist (preferred by ffmpegdirect).
  // - For inputstream.adaptive: keep "master" so we hit Jellyfin's master
  //   playlist endpoint, which wraps the media playlist with proper
  //   #EXT-X-STREAM-INF + CODECS attributes that adaptive needs to
  //   construct codec extradata before opening segments.
  // - Recalculate audio/video bitrates when the user set an explicit limit;
  //   when unlimited, preserve the server's original values.

  auto qPos = transcodingUrl.find('?');
  if (qPos == std::string::npos)
    return m_client->GetBaseUrl() + transcodingUrl;

  std::string base = transcodingUrl.substr(0, qPos);
  std::string paramStr = transcodingUrl.substr(qPos + 1);

  // Split params by &
  std::vector<std::string> params;
  std::string::size_type start = 0;
  while (start < paramStr.length())
  {
    auto end = paramStr.find('&', start);
    if (end == std::string::npos)
      end = paramStr.length();
    params.push_back(paramStr.substr(start, end - start));
    start = end + 1;
  }

  const int maxBitrateBps = m_activeMaxBitrateBps > 0
    ? m_activeMaxBitrateBps : m_settings->GetMaxBitrateBps();

  // Strip the server's VideoBitrate/AudioBitrate — we always recalculate.
  params.erase(std::remove_if(params.begin(), params.end(), [](const std::string& p) {
    return p.find("AudioBitrate=") == 0 || p.find("VideoBitrate=") == 0;
  }), params.end());

  // Rebuild query string
  std::string newParams;
  for (const auto& p : params)
  {
    if (!newParams.empty())
      newParams += "&";
    newParams += p;
  }

  const int audioBitrate = 384000;
  int videoBitrate;
  if (forceTranscode)
  {
    const int sourceBps = m_activeSourceBitrateBps > 0
      ? m_activeSourceBitrateBps : 30000000;
    videoBitrate = std::min(sourceBps, maxBitrateBps) - audioBitrate;
  }
  else
  {
    videoBitrate = maxBitrateBps - audioBitrate;
  }
  newParams += "&VideoBitrate=" + std::to_string(videoBitrate);
  newParams += "&AudioBitrate=" + std::to_string(audioBitrate);

  // Replace "stream"/"master" with "live" in URL path for live TV — unless
  // the caller asked us to keep the master endpoint (adaptive needs it).
  if (!keepMaster)
  {
    std::string search = (base.find("stream") != std::string::npos) ? "stream" : "master";
    auto pos = base.find(search);
    if (pos != std::string::npos)
      base.replace(pos, search.length(), "live");
  }

  return m_client->GetBaseUrl() + base + "?" + newParams;
}

void JellyfinChannelLoader::RewriteLocalhost(std::string& url)
{
  // Jellyfin sometimes returns 127.0.0.1 or localhost in stream URLs
  // when the tuner provider reports local addresses. Replace with the
  // actual server hostname so Kodi can reach it.
  const std::string host = m_settings->GetJellyfinHost();
  for (const char* pattern : {"127.0.0.1", "localhost"})
  {
    size_t pos = 0;
    const size_t len = strlen(pattern);
    while ((pos = url.find(pattern, pos)) != std::string::npos)
    {
      url.replace(pos, len, host);
      pos += host.size();
    }
  }
}

std::string JellyfinChannelLoader::GetLiveStreamUrl(const std::string& channelId,
                                                    const ChannelOverrides& overrides)
{
  return GetItemStreamUrl(channelId, overrides);
}

std::string JellyfinChannelLoader::GetItemStreamUrl(const std::string& itemId,
                                                    const ChannelOverrides& overrides)
{
  // Exception firewall: reached from Kodi's GetChannelStreamProperties.
  try
  {
    return GetItemStreamUrlInternal(itemId, overrides);
  }
  catch (const std::exception& e)
  {
    Logger::Log(LEVEL_ERROR, "%s - Exception parsing PlaybackInfo: %s", __FUNCTION__, e.what());
    return "";
  }
}

std::string JellyfinChannelLoader::GetItemStreamUrlInternal(const std::string& itemId,
                                                            const ChannelOverrides& overrides)
{
  Logger::Log(LEVEL_DEBUG, "%s - Getting stream URL for item %s", __FUNCTION__, itemId.c_str());

  // Close previous live stream if any
  CloseLiveStream();

  // POST to PlaybackInfo — all params in body, matching jellyfin-kodi
  const std::string endpoint = "/Items/" + itemId + "/PlaybackInfo";

  Json::Value body;
  body["UserId"] = m_client->GetUserId();
  body["DeviceProfile"] = BuildDeviceProfile(overrides);
  body["AutoOpenLiveStream"] = true;
  // Stash the override bitrate for PostProcessTranscodingUrl, which the server
  // will echo into the URL params we then rewrite.
  m_activeMaxBitrateBps = overrides.bitrateBps.value_or(m_settings->GetMaxBitrateBps());

  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  const std::string bodyStr = Json::writeString(writer, body);

  Logger::Log(LEVEL_DEBUG, "%s - PlaybackInfo request body: %s", __FUNCTION__, bodyStr.c_str());

  Json::Value response = m_client->SendPost(endpoint, bodyStr);

  if (response.isNull() || !response.isMember("MediaSources") || response["MediaSources"].empty())
  {
    Logger::Log(LEVEL_ERROR, "%s - PlaybackInfo returned no media sources for item %s",
                __FUNCTION__, itemId.c_str());
    return "";
  }

  const Json::Value& source = response["MediaSources"][0];

  m_activeSourceBitrateBps = SafeInt(source["Bitrate"]);

  // Track session state for reporting and cleanup
  m_activeLiveStreamId = source.get("LiveStreamId", "").asString();
  m_activePlaySessionId = response.get("PlaySessionId", "").asString();
  m_activeMediaSourceId = source.get("Id", "").asString();
  m_activeItemId = itemId;

  const bool hasTranscodingUrl = source.isMember("TranscodingUrl")
    && !source["TranscodingUrl"].asString().empty();
  m_activePlayMethod = hasTranscodingUrl ? "Transcode" : "DirectPlay";
  m_activeIsRecording = false;

  Logger::Log(LEVEL_DEBUG, "%s - LiveStreamId: %s, PlaySessionId: %s, PlayMethod: %s, "
              "SupportsDirectPlay: %s, Bitrate: %d",
              __FUNCTION__, m_activeLiveStreamId.c_str(), m_activePlaySessionId.c_str(),
              m_activePlayMethod.c_str(),
              source.get("SupportsDirectPlay", false).asBool() ? "true" : "false",
              m_activeSourceBitrateBps);

  std::string streamUrl;

  const bool forceTranscodeActive = overrides.forceTranscode.value_or(m_settings->GetForceTranscoding());

  if (hasTranscodingUrl)
  {
    const bool keepMaster = overrides.inputstream.value_or("") == "inputstream.adaptive";
    streamUrl = PostProcessTranscodingUrl(source["TranscodingUrl"].asString(), keepMaster, forceTranscodeActive);
  }
  else if (source.isMember("Path") && !source["Path"].asString().empty())
  {
    streamUrl = source["Path"].asString();
  }

  // Fallback: construct URL from LiveStreamId/MediaSourceId
  if (streamUrl.empty())
  {
    const std::string mediaSourceId = source.get("Id", "").asString();
    streamUrl = m_client->GetBaseUrl() + "/Videos/" + itemId + "/live.m3u8"
      + "?LiveStreamId=" + WebUtils::UrlEncode(m_activeLiveStreamId)
      + "&MediaSourceId=" + WebUtils::UrlEncode(mediaSourceId)
      + "&ApiKey=" + m_client->GetAccessToken();
  }

  // Fix localhost/127.0.0.1 references from tuner providers
  RewriteLocalhost(streamUrl);

  const std::string baseUrl = m_client->GetBaseUrl();
  if (streamUrl.find(baseUrl) == 0 &&
      streamUrl.find("ApiKey=") == std::string::npos)
  {
    streamUrl += (streamUrl.find('?') != std::string::npos ? "&" : "?");
    streamUrl += "ApiKey=" + m_client->GetAccessToken();
  }

  Logger::Log(LEVEL_DEBUG, "%s - Stream URL: %s", __FUNCTION__,
              WebUtils::RedactUrl(streamUrl).c_str());

  WriteSessionFile();

  return streamUrl;
}

void JellyfinChannelLoader::CloseLiveStream()
{
  if (m_activeLiveStreamId.empty() && m_activePlaySessionId.empty())
    return;

  Logger::Log(LEVEL_INFO, "%s - Clearing session %s, stream %s",
              __FUNCTION__, m_activePlaySessionId.c_str(), m_activeLiveStreamId.c_str());

  m_activeLiveStreamId.clear();
  m_activePlaySessionId.clear();
  m_activeMediaSourceId.clear();
  m_activeItemId.clear();
  m_activePlayMethod.clear();
  m_activeIsRecording = false;
}


std::string JellyfinChannelLoader::GetJellyfinId(int channelUid) const
{
  std::lock_guard<std::mutex> lock(m_dataMutex);
  auto it = m_uidToJellyfinId.find(channelUid);
  if (it != m_uidToJellyfinId.end())
    return it->second;
  return {};
}

std::string JellyfinChannelLoader::GetJellyfinProgramId(unsigned int epgBroadcastUid) const
{
  std::lock_guard<std::mutex> lock(m_dataMutex);
  auto it = m_epgUidToJellyfinProgramId.find(epgBroadcastUid);
  if (it != m_epgUidToJellyfinProgramId.end())
    return it->second;
  return {};
}

int JellyfinChannelLoader::GetChannelUid(const std::string& jellyfinId) const
{
  std::lock_guard<std::mutex> lock(m_dataMutex);
  auto it = m_jellyfinIdToUid.find(jellyfinId);
  if (it != m_jellyfinIdToUid.end())
    return it->second;
  return 0;
}

int JellyfinChannelLoader::GenerateUid(const std::string& str)
{
  // djb2 hash - same algorithm as Channels::GenerateChannelId
  const char* s = str.c_str();
  int hash = 0;
  int c;
  while ((c = *s++))
    hash = ((hash << 5) + hash) + c;
  return std::abs(hash);
}

std::string JellyfinChannelLoader::FormatIso8601(time_t time)
{
  std::tm tm = SafeGmtime(time);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return std::string(buf);
}

time_t JellyfinChannelLoader::ParseIso8601(const std::string& dateStr)
{
  if (dateStr.empty())
    return 0;

  std::tm tm = {};
  // Parse "2024-01-15T20:00:00.0000000Z" or "2024-01-15T20:00:00Z"
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
