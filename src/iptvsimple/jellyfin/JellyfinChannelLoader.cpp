/*
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "JellyfinChannelLoader.h"

#include "../M3UParser.h"
#include "../utilities/Logger.h"
#include "../utilities/TimeUtils.h"
#include "../utilities/WebUtils.h"

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
      m_jellyfinIdToUid[jellyfinId] = uid;
      m_uidToJellyfinId[uid] = jellyfinId;

      Logger::Log(LEVEL_DEBUG, "%s - Added channel '%s' (uid=%d, jellyfinId=%s, groups=%zu)",
                  __FUNCTION__, name.c_str(), uid, jellyfinId.c_str(), groupIdList.size());
    }
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
  auto it = m_uidToJellyfinId.find(channelUid);
  if (it == m_uidToJellyfinId.end())
  {
    Logger::Log(LEVEL_ERROR, "%s - No Jellyfin ID for channel UID %d", __FUNCTION__, channelUid);
    return PVR_ERROR_INVALID_PARAMETERS;
  }

  const std::string& jellyfinId = it->second;

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
    m_epgUidToJellyfinProgramId[broadcastUid] = jellyfinProgramId;

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
      tag.SetEpisodeNumber(item["IndexNumber"].asInt());
    if (item.isMember("ParentIndexNumber"))
      tag.SetSeriesNumber(item["ParentIndexNumber"].asInt());

    // Year
    if (item.isMember("ProductionYear"))
      tag.SetYear(item["ProductionYear"].asInt());

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
  const int maxBitrateBps = overrides.bitrateBps.value_or(m_settings->GetMaxBitrateBps());
  const bool forceRemux = overrides.forceRemux.value_or(m_settings->GetForceTranscode());
  const bool forceTranscodeOverride = overrides.forceTranscode.value_or(false);

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

  const bool bitrateUnlimited = (maxBitrateBps >= 1000000000);

  // Container choice: AV1 can't ride MPEG-TS so transcode-to-AV1 needs fMP4.
  // Remux always uses TS (AV1 is stripped from the remux codec list below).
  const std::string transcodeContainer = (preferredVideo == "av1") ? "mp4" : "ts";

  // inputstream.adaptive rejects single-segment transcode playlists with
  // "Codec id NN require extradata". Remux segments are instant and work
  // with 1; transcode needs 3 when adaptive is the inputstream.
  const bool useAdaptive = overrides.inputstream.value_or("") == "inputstream.adaptive";

  // TranscodingProfiles:
  // - Remux mode (forceRemux ON + unlimited bitrate + no per-channel forceTranscode)
  //   → codec-copy the allowed codecs.
  // - Otherwise → preferred codec only.
  Json::Value transcodingProfiles(Json::arrayValue);

  const bool remuxMode = forceRemux && bitrateUnlimited && !forceTranscodeOverride;

  if (remuxMode)
  {
    Json::Value tp;
    tp["Container"] = "ts";
    tp["Type"] = "Video";
    tp["AudioCodec"] = audioCodecs;
    // TS can't carry AV1; drop it from the remux codec list.
    std::string remuxCodecs;
    std::string::size_type rStart = 0;
    while (rStart < allowedVideoCodecsCsv.length())
    {
      auto rEnd = allowedVideoCodecsCsv.find(',', rStart);
      if (rEnd == std::string::npos)
        rEnd = allowedVideoCodecsCsv.length();
      std::string c = allowedVideoCodecsCsv.substr(rStart, rEnd - rStart);
      if (c != "av1")
      {
        if (!remuxCodecs.empty())
          remuxCodecs += ",";
        remuxCodecs += c;
      }
      rStart = rEnd + 1;
    }
    tp["VideoCodec"] = remuxCodecs;
    tp["Context"] = "Streaming";
    tp["Protocol"] = "hls";
    tp["MaxAudioChannels"] = std::to_string(m_settings->GetMaxAudioChannels());
    tp["MinSegments"] = useAdaptive ? "3" : "1";
    tp["BreakOnNonKeyFrames"] = true;
    transcodingProfiles.append(tp);
  }
  else
  {
    Json::Value tp;
    tp["Container"] = transcodeContainer;
    tp["Type"] = "Video";
    tp["AudioCodec"] = audioCodecs;
    tp["VideoCodec"] = preferredVideo;
    tp["Context"] = "Streaming";
    tp["Protocol"] = "hls";
    tp["MaxAudioChannels"] = std::to_string(m_settings->GetMaxAudioChannels());
    tp["MinSegments"] = useAdaptive ? "3" : "1";
    tp["BreakOnNonKeyFrames"] = true;
    transcodingProfiles.append(tp);
  }
  profile["TranscodingProfiles"] = transcodingProfiles;

  // Direct play only when neither remux nor transcode is forced AND no
  // bitrate limit. For live TV (Protocol=Http), Jellyfin ignores
  // MaxStreamingBitrate for DirectPlay decisions, so we must clear
  // DirectPlayProfiles to force TranscodingProfiles when a limit is set.
  Json::Value directPlayProfiles(Json::arrayValue);
  if (!forceRemux && !forceTranscodeOverride && bitrateUnlimited && !allowedVideoCodecsCsv.empty())
  {
    Json::Value v;
    v["Type"] = "Video";
    v["VideoCodec"] = allowedVideoCodecsCsv;
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
  Logger::Log(LEVEL_DEBUG, "%s - Getting recording stream URL for %s", __FUNCTION__, recordingId.c_str());

  // Close previous live stream if any
  CloseLiveStream();

  const std::string endpoint = "/Items/" + recordingId + "/PlaybackInfo";

  Json::Value body;
  body["UserId"] = m_client->GetUserId();
  ChannelOverrides effectiveOverrides = overrides;
  if (inProgress && !effectiveOverrides.forceTranscode.value_or(false))
    effectiveOverrides.forceRemux = true;
  Json::Value deviceProfile = BuildDeviceProfile(effectiveOverrides);
  if (inProgress)
    deviceProfile["DirectPlayProfiles"] = Json::Value(Json::arrayValue);
  // Recordings always play via inputstream.adaptive but overrides.inputstream
  // isn't set, so BuildDeviceProfile leaves MinSegments=1 in the transcode
  // branch. Bump it to 3 — adaptive needs a populated variant playlist.
  // Only touch the transcode profile (single codec); leave remux (multi-codec) at 1.
  if (!deviceProfile["TranscodingProfiles"].empty())
  {
    Json::Value& tp = deviceProfile["TranscodingProfiles"][0];
    const std::string vc = tp.get("VideoCodec", "").asString();
    if (vc.find(',') == std::string::npos)
      tp["MinSegments"] = "3";
  }
  body["DeviceProfile"] = deviceProfile;
  body["AutoOpenLiveStream"] = true;

  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  const std::string bodyStr = Json::writeString(writer, body);

  Json::Value response = m_client->SendPost(endpoint, bodyStr);

  if (response.isNull() || !response.isMember("MediaSources") || response["MediaSources"].empty())
  {
    Logger::Log(LEVEL_ERROR, "%s - PlaybackInfo returned no media sources for recording %s",
                __FUNCTION__, recordingId.c_str());
    return "";
  }

  const Json::Value& source = response["MediaSources"][0];

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

  if (hasTranscodingUrl)
  {
    streamUrl = m_client->GetBaseUrl() + source["TranscodingUrl"].asString();
  }
  else
  {
    streamUrl = m_client->GetBaseUrl() + "/Videos/" + recordingId
      + "/stream?static=true&api_key=" + m_client->GetAccessToken();
  }

  RewriteLocalhost(streamUrl);

  // Append api_key if it's a Jellyfin URL
  const std::string baseUrl = m_client->GetBaseUrl();
  if (streamUrl.find(baseUrl) == 0 &&
      streamUrl.find("api_key=") == std::string::npos &&
      streamUrl.find("ApiKey=") == std::string::npos)
  {
    streamUrl += (streamUrl.find('?') != std::string::npos ? "&" : "?");
    streamUrl += "api_key=" + m_client->GetAccessToken();
  }

  Logger::Log(LEVEL_DEBUG, "%s - Recording stream URL: %s", __FUNCTION__,
              WebUtils::RedactUrl(streamUrl).c_str());

  // Persist session metadata for Python playback reporter.
  // Each SetSettingString triggers TransferSettings which re-delivers
  // list[string] codec settings with stale values — suppress capture.
  m_settings->SetSuppressCodecCapture(true);
  kodi::addon::SetSettingString("sessionItemId", m_activeItemId);
  kodi::addon::SetSettingString("sessionMediaSourceId", m_activeMediaSourceId);
  kodi::addon::SetSettingString("sessionPlaySessionId", m_activePlaySessionId);
  kodi::addon::SetSettingString("sessionLiveStreamId", m_activeLiveStreamId);
  kodi::addon::SetSettingString("sessionPlayMethod", m_activePlayMethod);
  m_settings->SetSuppressCodecCapture(false);

  return streamUrl;
}

std::string JellyfinChannelLoader::PostProcessTranscodingUrl(
    const std::string& transcodingUrl, bool keepMaster, bool isRemux)
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
  const bool bitrateUnlimited = (maxBitrateBps >= 1000000000);
  int videoBitrate;
  if (!isRemux && bitrateUnlimited)
  {
    const int sourceBps = m_activeSourceBitrateBps > 0
      ? m_activeSourceBitrateBps : 30000000;
    videoBitrate = sourceBps - audioBitrate;
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

  Json::Value response = m_client->SendPost(endpoint, bodyStr);

  if (response.isNull() || !response.isMember("MediaSources") || response["MediaSources"].empty())
  {
    Logger::Log(LEVEL_ERROR, "%s - PlaybackInfo returned no media sources for item %s",
                __FUNCTION__, itemId.c_str());
    return "";
  }

  const Json::Value& source = response["MediaSources"][0];

  m_activeSourceBitrateBps = source.get("Bitrate", 0).asInt();

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
              source.get("Bitrate", 0).asInt());

  std::string streamUrl;

  // Determine if this session is remux (codec-copy) vs real transcode.
  const int maxBitrateBps = overrides.bitrateBps.value_or(m_settings->GetMaxBitrateBps());
  const bool bitrateUnlimited = (maxBitrateBps >= 1000000000);
  const bool forceRemux = overrides.forceRemux.value_or(m_settings->GetForceTranscode());
  const bool forceTranscodeOverride = overrides.forceTranscode.value_or(false);
  const bool isRemux = forceRemux && bitrateUnlimited && !forceTranscodeOverride;

  if (hasTranscodingUrl)
  {
    const bool keepMaster = overrides.inputstream.value_or("") == "inputstream.adaptive";
    streamUrl = PostProcessTranscodingUrl(source["TranscodingUrl"].asString(), keepMaster, isRemux);
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
      + "&api_key=" + m_client->GetAccessToken();
  }

  // Fix localhost/127.0.0.1 references from tuner providers
  RewriteLocalhost(streamUrl);

  // Only append api_key to Jellyfin server URLs, not third-party tuner URLs
  const std::string baseUrl = m_client->GetBaseUrl();
  if (streamUrl.find(baseUrl) == 0 &&
      streamUrl.find("api_key=") == std::string::npos &&
      streamUrl.find("ApiKey=") == std::string::npos)
  {
    streamUrl += (streamUrl.find('?') != std::string::npos ? "&" : "?");
    streamUrl += "api_key=" + m_client->GetAccessToken();
  }

  Logger::Log(LEVEL_DEBUG, "%s - Stream URL: %s", __FUNCTION__,
              WebUtils::RedactUrl(streamUrl).c_str());

  // Persist session metadata for Python playback reporter.
  m_settings->SetSuppressCodecCapture(true);
  kodi::addon::SetSettingString("sessionItemId", m_activeItemId);
  kodi::addon::SetSettingString("sessionMediaSourceId", m_activeMediaSourceId);
  kodi::addon::SetSettingString("sessionPlaySessionId", m_activePlaySessionId);
  kodi::addon::SetSettingString("sessionLiveStreamId", m_activeLiveStreamId);
  kodi::addon::SetSettingString("sessionPlayMethod", m_activePlayMethod);
  m_settings->SetSuppressCodecCapture(false);

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


const std::string& JellyfinChannelLoader::GetJellyfinId(int channelUid) const
{
  static const std::string empty;
  auto it = m_uidToJellyfinId.find(channelUid);
  if (it != m_uidToJellyfinId.end())
    return it->second;
  return empty;
}

const std::string& JellyfinChannelLoader::GetJellyfinProgramId(unsigned int epgBroadcastUid) const
{
  static const std::string empty;
  auto it = m_epgUidToJellyfinProgramId.find(epgBroadcastUid);
  if (it != m_epgUidToJellyfinProgramId.end())
    return it->second;
  return empty;
}

int JellyfinChannelLoader::GetChannelUid(const std::string& jellyfinId) const
{
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
