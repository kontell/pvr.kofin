/*
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "JellyfinChannelLoader.h"

#include "../utilities/Logger.h"
#include "../utilities/WebUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>

using namespace iptvsimple;
using namespace iptvsimple::data;
using namespace iptvsimple::jellyfin;
using namespace iptvsimple::utilities;

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

  // Create "All Channels" group
  ChannelGroup allGroup;
  allGroup.SetGroupName("Jellyfin");
  allGroup.SetRadio(false);
  int groupId = channelGroups.AddChannelGroup(allGroup);

  int channelNumber = 1;

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

    // Add channel (AddChannel sets the unique ID via its hash function)
    std::vector<int> groupIdList = {groupId};
    if (channels.AddChannel(channel, groupIdList, channelGroups, true))
    {
      int uid = channel.GetUniqueId();
      m_jellyfinIdToUid[jellyfinId] = uid;
      m_uidToJellyfinId[uid] = jellyfinId;

      Logger::Log(LEVEL_DEBUG, "%s - Added channel '%s' (uid=%d, jellyfinId=%s)",
                  __FUNCTION__, name.c_str(), uid, jellyfinId.c_str());
    }
  }

  channelGroups.RemoveEmptyGroups();

  Logger::Log(LEVEL_INFO, "%s - Loaded %d channels from Jellyfin",
              __FUNCTION__, static_cast<int>(items.size()));
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

Json::Value JellyfinChannelLoader::BuildDeviceProfile()
{
  const std::string preferredVideo = m_settings->GetPreferredVideoCodecName();
  const std::string preferredAudio = m_settings->GetPreferredAudioCodecName();
  const int maxBitrateBps = m_settings->GetMaxBitrateBps();
  const bool forceTranscode = m_settings->GetForceTranscode();

  Json::Value profile;
  profile["Name"] = "Kodi";
  profile["MaxStreamingBitrate"] = maxBitrateBps;
  profile["MaxStaticBitrate"] = maxBitrateBps;
  profile["MusicStreamingTranscodingBitrate"] = 1280000;
  profile["TimelineOffsetSeconds"] = 5;

  // Build audio codec list with preferred codec first
  std::string audioCodecs = preferredAudio;
  for (const char* codec : {"aac", "mp3", "ac3", "eac3", "opus", "flac"})
  {
    if (preferredAudio != codec)
      audioCodecs += std::string(",") + codec;
  }

  const bool bitrateUnlimited = (maxBitrateBps >= 1000000000);

  // TranscodingProfiles:
  // - Force remux ON + unlimited bitrate → codec-copy into TS (remux)
  // - All other cases → preferred codec only (AV1→fMP4, others→TS)
  //   Covers: force remux with bitrate limit, no force remux with bitrate
  //   limit exceeded, codec profile mismatch, etc.
  Json::Value transcodingProfiles(Json::arrayValue);

  if (forceTranscode && bitrateUnlimited)
  {
    // Remux: codec-copy all non-AV1 codecs into TS.
    Json::Value tp;
    tp["Container"] = "ts";
    tp["Type"] = "Video";
    tp["AudioCodec"] = audioCodecs;
    tp["VideoCodec"] = "h264,hevc,mpeg2video";
    tp["Context"] = "Streaming";
    tp["Protocol"] = "hls";
    tp["MaxAudioChannels"] = "6";
    tp["MinSegments"] = "1";
    tp["BreakOnNonKeyFrames"] = true;
    transcodingProfiles.append(tp);
  }
  else
  {
    // Transcode to preferred codec.
    // AV1 → fMP4 (AV1 can't go in MPEG-TS), everything else → TS.
    Json::Value tp;
    tp["Container"] = (preferredVideo == "av1") ? "mp4" : "ts";
    tp["Type"] = "Video";
    tp["AudioCodec"] = audioCodecs;
    tp["VideoCodec"] = preferredVideo;
    tp["Context"] = "Streaming";
    tp["Protocol"] = "hls";
    tp["MaxAudioChannels"] = "6";
    tp["MinSegments"] = "1";
    tp["BreakOnNonKeyFrames"] = true;
    transcodingProfiles.append(tp);
  }
  profile["TranscodingProfiles"] = transcodingProfiles;

  // Direct play only when force remux is off AND no bitrate limit.
  // For live TV (Protocol=Http), Jellyfin ignores MaxStreamingBitrate
  // for DirectPlay decisions, so we must clear DirectPlayProfiles to
  // force the server to use TranscodingProfiles when a limit is set.
  Json::Value directPlayProfiles(Json::arrayValue);
  if (!forceTranscode && bitrateUnlimited)
  {
    Json::Value v;
    v["Type"] = "Video";
    v["VideoCodec"] = "av1,h264,hevc,mpeg2video";
    directPlayProfiles.append(v);
    Json::Value a;
    a["Type"] = "Audio";
    directPlayProfiles.append(a);
  }
  profile["DirectPlayProfiles"] = directPlayProfiles;

  // Codec profiles: constrain direct play to codecs the device can actually decode
  Json::Value codecProfiles(Json::arrayValue);
  if (m_settings->GetTranscodeHi10P())
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
  if (m_settings->GetTranscodeHevcRext())
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

Json::Value JellyfinChannelLoader::BuildRecordingDeviceProfile()
{
  // Recording profile: always remux (no DirectPlay), respects bitrate limit
  // and preferred codec. Unlimited bitrate = codec copy, limited = server
  // re-encodes to preferred codec within limit.
  const int maxBitrateBps = m_settings->GetMaxBitrateBps();
  const std::string preferredVideo = m_settings->GetPreferredVideoCodecName();
  const bool bitrateUnlimited = (maxBitrateBps >= 1000000000);

  Json::Value profile;
  profile["Name"] = "Kodi";
  profile["MaxStreamingBitrate"] = maxBitrateBps;
  profile["MaxStaticBitrate"] = maxBitrateBps;
  profile["MusicStreamingTranscodingBitrate"] = 1280000;

  Json::Value transcodingProfiles(Json::arrayValue);
  if (bitrateUnlimited)
  {
    // Codec copy into TS
    Json::Value tp;
    tp["Container"] = "ts";
    tp["Type"] = "Video";
    tp["AudioCodec"] = "aac,mp3,ac3,eac3,opus,flac";
    tp["VideoCodec"] = "h264,hevc,av1,mpeg2video";
    tp["Context"] = "Streaming";
    tp["Protocol"] = "hls";
    tp["MaxAudioChannels"] = "6";
    tp["MinSegments"] = "1";
    tp["BreakOnNonKeyFrames"] = true;
    transcodingProfiles.append(tp);
  }
  else
  {
    // Transcode to preferred codec
    Json::Value tp;
    tp["Container"] = (preferredVideo == "av1") ? "mp4" : "ts";
    tp["Type"] = "Video";
    tp["AudioCodec"] = "aac,mp3,ac3,eac3,opus,flac";
    tp["VideoCodec"] = preferredVideo;
    tp["Context"] = "Streaming";
    tp["Protocol"] = "hls";
    tp["MaxAudioChannels"] = "6";
    tp["MinSegments"] = "1";
    tp["BreakOnNonKeyFrames"] = true;
    transcodingProfiles.append(tp);
  }
  profile["TranscodingProfiles"] = transcodingProfiles;

  // Empty direct play profiles → Jellyfin will always remux
  profile["DirectPlayProfiles"] = Json::Value(Json::arrayValue);
  profile["CodecProfiles"] = Json::Value(Json::arrayValue);
  profile["SubtitleProfiles"] = Json::Value(Json::arrayValue);
  profile["ResponseProfiles"] = Json::Value(Json::arrayValue);
  profile["ContainerProfiles"] = Json::Value(Json::arrayValue);

  return profile;
}

std::string JellyfinChannelLoader::GetRecordingStreamUrl(const std::string& recordingId)
{
  Logger::Log(LEVEL_DEBUG, "%s - Getting recording stream URL for %s", __FUNCTION__, recordingId.c_str());

  // Close previous live stream if any
  CloseLiveStream();

  const std::string endpoint = "/Items/" + recordingId + "/PlaybackInfo";

  Json::Value body;
  body["UserId"] = m_client->GetUserId();
  body["DeviceProfile"] = BuildRecordingDeviceProfile();
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
    // Prepend base URL — no live rewrite or bitrate recalc needed for recordings
    streamUrl = m_client->GetBaseUrl() + source["TranscodingUrl"].asString();
  }
  else if (source.isMember("Path") && !source["Path"].asString().empty())
  {
    streamUrl = source["Path"].asString();
  }

  if (streamUrl.empty())
  {
    Logger::Log(LEVEL_ERROR, "%s - No stream URL resolved for recording %s",
                __FUNCTION__, recordingId.c_str());
    return "";
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

  ScheduleDeferredPlayingReport();

  return streamUrl;
}

std::string JellyfinChannelLoader::PostProcessTranscodingUrl(
    const std::string& transcodingUrl)
{
  // Post-process TranscodingUrl to match jellyfin-kodi behaviour:
  // - Replace "stream"/"master" with "live" for live TV
  // - Recalculate audio/video bitrates
  // SegmentContainer is left as-is (set correctly by server via dual TranscodingProfiles)

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

  // Strip existing AudioBitrate and VideoBitrate (recalculated below)
  params.erase(std::remove_if(params.begin(), params.end(), [](const std::string& p) {
    return p.find("AudioBitrate=") == 0 || p.find("VideoBitrate=") == 0;
  }), params.end());

  // Recalculate bitrates
  const int maxBitrateBps = m_settings->GetMaxBitrateBps();
  const int audioBitrate = 384000;
  const int videoBitrate = maxBitrateBps - audioBitrate;

  // Rebuild query string
  std::string newParams;
  for (const auto& p : params)
  {
    if (!newParams.empty())
      newParams += "&";
    newParams += p;
  }
  newParams += "&VideoBitrate=" + std::to_string(videoBitrate);
  newParams += "&AudioBitrate=" + std::to_string(audioBitrate);

  // Replace "stream" or "master" with "live" in URL path for live TV
  std::string search = (base.find("stream") != std::string::npos) ? "stream" : "master";
  auto pos = base.find(search);
  if (pos != std::string::npos)
    base.replace(pos, search.length(), "live");

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

std::string JellyfinChannelLoader::GetLiveStreamUrl(const std::string& channelId)
{
  return GetItemStreamUrl(channelId);
}

std::string JellyfinChannelLoader::GetItemStreamUrl(const std::string& itemId)
{
  Logger::Log(LEVEL_DEBUG, "%s - Getting stream URL for item %s", __FUNCTION__, itemId.c_str());

  // Close previous live stream if any
  CloseLiveStream();

  // POST to PlaybackInfo — all params in body, matching jellyfin-kodi
  const std::string endpoint = "/Items/" + itemId + "/PlaybackInfo";

  Json::Value body;
  body["UserId"] = m_client->GetUserId();
  body["DeviceProfile"] = BuildDeviceProfile();
  body["AutoOpenLiveStream"] = true;

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

  // For force remux or server-initiated transcode: post-process the URL
  // matching jellyfin-kodi's transcode() method (stream→live rewrite, bitrate recalc).
  if (hasTranscodingUrl)
  {
    streamUrl = PostProcessTranscodingUrl(source["TranscodingUrl"].asString());
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

  // Schedule deferred Sessions/Playing report — gives Kodi time to connect
  // to the stream so the server has an active transcode job before we report.
  ScheduleDeferredPlayingReport();

  return streamUrl;
}

void JellyfinChannelLoader::CloseLiveStream()
{
  if (m_activeLiveStreamId.empty() && m_activePlaySessionId.empty())
    return;

  // Increment generation — cancels any pending deferred Sessions/Playing report
  ++m_sessionGen;

  const std::string liveStreamId = m_activeLiveStreamId;
  const std::string playSessionId = m_activePlaySessionId;
  const std::string mediaSourceId = m_activeMediaSourceId;
  const std::string itemId = m_activeItemId;
  m_activeLiveStreamId.clear();
  m_activePlaySessionId.clear();
  m_activeMediaSourceId.clear();
  m_activeItemId.clear();
  m_activePlayMethod.clear();
  const bool isRecording = m_activeIsRecording;
  m_activeIsRecording = false;

  Logger::Log(LEVEL_INFO, "%s - Closing session %s, stream %s (async, recording=%s)",
              __FUNCTION__, playSessionId.c_str(), liveStreamId.c_str(),
              isRecording ? "yes" : "no");

  auto client = m_client;
  std::thread([client, playSessionId, mediaSourceId, itemId, liveStreamId, isRecording]() {
    // Report playback stopped — skip for recordings because the server
    // treats Sessions/Playing/Stopped as "fully watched" (sets Played=true,
    // resets position to 0). Recording watched state is handled by
    // SetRecordingPlayCount/SetRecordingLastPlayedPosition instead.
    if (!isRecording && !playSessionId.empty() && !itemId.empty())
    {
      Json::Value stopBody;
      stopBody["ItemId"] = itemId;
      stopBody["MediaSourceId"] = mediaSourceId;
      stopBody["PlaySessionId"] = playSessionId;
      Json::StreamWriterBuilder w;
      w["indentation"] = "";
      client->SendPost("/Sessions/Playing/Stopped", Json::writeString(w, stopBody));
    }

    // Close the live stream (query param format; "{}" body forces POST via CFile)
    if (!liveStreamId.empty())
    {
      const std::string endpoint = "/LiveStreams/Close?liveStreamId="
        + WebUtils::UrlEncode(liveStreamId);
      client->SendPost(endpoint, "{}");
    }
  }).detach();
}

Json::Value JellyfinChannelLoader::BuildSessionBody() const
{
  Json::Value body;
  body["QueueableMediaTypes"] = "Video,Audio";
  body["CanSeek"] = true;
  body["ItemId"] = m_activeItemId;
  body["MediaSourceId"] = m_activeMediaSourceId;
  body["PlayMethod"] = m_activePlayMethod;
  body["PlaySessionId"] = m_activePlaySessionId;
  body["PositionTicks"] = 0;
  body["IsPaused"] = false;
  body["IsMuted"] = false;
  body["VolumeLevel"] = 100;
  return body;
}

void JellyfinChannelLoader::ScheduleDeferredPlayingReport()
{
  const uint32_t gen = m_sessionGen.load();
  auto client = m_client;
  auto body = BuildSessionBody();

  Logger::Log(LEVEL_INFO, "%s - Scheduling Sessions/Playing (gen=%u, method=%s)",
              __FUNCTION__, gen, m_activePlayMethod.c_str());

  std::thread([this, client, body, gen]() {
    std::this_thread::sleep_for(std::chrono::seconds(3));

    if (m_sessionGen.load() != gen)
    {
      Logger::Log(LEVEL_INFO, "Deferred Sessions/Playing skipped — session generation changed");
      return;
    }

    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    client->SendPost("/Sessions/Playing", Json::writeString(w, body));
  }).detach();
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
  struct tm tm;
  gmtime_r(&time, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return std::string(buf);
}

time_t JellyfinChannelLoader::ParseIso8601(const std::string& dateStr)
{
  if (dateStr.empty())
    return 0;

  struct tm tm = {};
  // Parse "2024-01-15T20:00:00.0000000Z" or "2024-01-15T20:00:00Z"
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
