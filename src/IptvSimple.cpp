/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "IptvSimple.h"

#include "iptvsimple/CatchupController.h"
#include "iptvsimple/InstanceSettings.h"
#include "iptvsimple/utilities/Logger.h"
#include "iptvsimple/utilities/StreamUtils.h"
#include "iptvsimple/utilities/TimeUtils.h"
#include "iptvsimple/utilities/WebUtils.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <memory>

#include <kodi/General.h>
#include <kodi/tools/StringUtils.h>

using namespace iptvsimple;
using namespace iptvsimple::data;
using namespace iptvsimple::utilities;
using namespace kodi::tools;

namespace
{

// Append the stream URL, MIME type, inputstream and manifest_type for a catchup
// stream played via inputstream.ffmpegdirect. MIME and manifest_type are derived
// from the resolved catchup URL (and the channel's catchup-TS flag) rather than
// assuming HLS: TS catchup sources — Xtream-codes, Flussonic "mpegts", and
// shift/append on a .ts base — must NOT be labelled HLS, or ffmpegdirect tries to
// parse raw MPEG-TS as an HLS playlist and fails. GetMimeType()/GetManifestType()
// return "" for TS/unknown types, matching pvr.iptvsimple's SetAllStreamProperties.
void AppendFfmpegDirectCatchupProperties(std::vector<kodi::addon::PVRStreamProperty>& properties,
                                         const std::string& streamURL, bool isCatchupTSStream)
{
  const StreamType streamType = StreamUtils::GetStreamType(streamURL, "", isCatchupTSStream);
  const std::string mimeType = StreamUtils::GetMimeType(streamType);
  const std::string manifestType = StreamUtils::GetManifestType(streamType);

  properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, streamURL);
  if (!mimeType.empty())
    properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, mimeType);
  properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.ffmpegdirect");
  if (!manifestType.empty())
    properties.emplace_back("inputstream.ffmpegdirect.manifest_type", manifestType);
}

} // unnamed namespace

IptvSimple::IptvSimple(const kodi::addon::IInstanceInfo& instance) : iptvsimple::IConnectionListener(instance), m_settings(new InstanceSettings())
{
  m_channels.Clear();
  m_channelGroups.Clear();
  m_providers.Clear();
  connectionManager = new ConnectionManager(*this, m_settings);
  m_auth = std::make_shared<iptvsimple::jellyfin::JellyfinAuth>(m_settings);

  // Register PVR settings menu hook so "Client specific settings" works
  AddMenuHook(kodi::addon::PVRMenuhook(1, 30724, PVR_MENUHOOK_SETTING));
}

IptvSimple::~IptvSimple()
{
  Logger::Log(LEVEL_DEBUG, "%s Stopping update thread...", __FUNCTION__);
  m_running = false;
  if (m_thread.joinable())
    m_thread.join();

  // Close any active live stream before teardown
  if (m_channelLoader)
    m_channelLoader->CloseLiveStream();

  std::lock_guard<std::mutex> lock(m_mutex);
  m_channels.Clear();
  m_channelGroups.Clear();
  m_providers.Clear();

  if (connectionManager)
    connectionManager->Stop();
  delete connectionManager;
}

/* **************************************************************************
 * Connection
 * *************************************************************************/

void IptvSimple::ConnectionLost()
{
  Logger::Log(LEVEL_INFO, "%s Connection lost.", __func__);
}

void IptvSimple::ConnectionEstablished()
{
  m_settings->ReadSettings();

  // Don't attempt auth if user hasn't logged in via the Account settings
  if (!m_settings->GetIsLoggedIn())
  {
    Logger::Log(LEVEL_INFO, "%s - Not logged in, skipping authentication. Use addon settings to log in.", __FUNCTION__);
    return;
  }

  m_channels.Init();
  m_channelGroups.Init();
  m_providers.Init();

  // Create Jellyfin client
  m_jellyfinClient = std::make_shared<iptvsimple::jellyfin::JellyfinClient>(m_settings);
  m_auth->SetClient(m_jellyfinClient);

  Logger::Log(LEVEL_INFO, "%s Connecting to Jellyfin at %s", __FUNCTION__,
              m_jellyfinClient->GetBaseUrl().c_str());

  // Validate stored token
  if (!m_jellyfinClient->Authenticate())
  {
    Logger::Log(LEVEL_ERROR, "%s - Authentication failed. Token may have expired — please log in again.", __FUNCTION__);
    kodi::QueueNotification(QUEUE_ERROR, "Kofin PVR", "Session expired. Please log in again.");
    return;
  }

  Logger::Log(LEVEL_INFO, "%s Authenticated, loading channels", __FUNCTION__);

  // Fetch/update server name
  m_auth->FetchAndStoreServerName();

  // Reuse existing loader/manager to preserve EPG program ID mappings across reconnects
  if (!m_channelLoader)
    m_channelLoader = std::make_shared<iptvsimple::jellyfin::JellyfinChannelLoader>(m_jellyfinClient, m_settings);
  else
    m_channelLoader->SetClient(m_jellyfinClient);
  m_channelLoader->LoadChannels(m_channels, m_channelGroups);

  if (!m_recordingManager)
    m_recordingManager = std::make_shared<iptvsimple::jellyfin::JellyfinRecordingManager>(
        m_jellyfinClient, m_channelLoader, m_settings);
  else
    m_recordingManager->SetClient(m_jellyfinClient);
  m_recordingManager->SetChannels(&m_channels);
  m_recordingManager->Reload();

  kodi::Log(ADDON_LOG_INFO, "%s Starting separate client update thread...", __FUNCTION__);

  // ConnectionEstablished fires again on every reconnect (ConnectionManager
  // re-enters CONNECTED after an outage). Stop and join the previous update
  // thread first — assigning to a joinable std::thread calls std::terminate.
  m_running = false;
  if (m_thread.joinable())
    m_thread.join();

  m_running = true;
  m_thread = std::thread([this] { Process(); });
}

bool IptvSimple::Initialise()
{
  std::lock_guard<std::mutex> lock(m_mutex);

  connectionManager->Start();

  return true;
}

PVR_ERROR IptvSimple::OnSystemSleep()
{
  connectionManager->OnSleep();
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::OnSystemWake()
{
  connectionManager->OnWake();
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::GetCapabilities(kodi::addon::PVRCapabilities& capabilities)
{
  capabilities.SetSupportsEPG(true);
  capabilities.SetSupportsTV(true);
  capabilities.SetSupportsRadio(false);
  capabilities.SetSupportsChannelGroups(true);
  capabilities.SetSupportsProviders(true);
  capabilities.SetSupportsTimers(true);
  capabilities.SetSupportsRecordings(true);
  capabilities.SetSupportsRecordingsRename(false);
  capabilities.SetSupportsRecordingsLifetimeChange(false);
  capabilities.SetSupportsRecordingsDelete(true);
  capabilities.SetSupportsRecordingPlayCount(true);
  capabilities.SetSupportsLastPlayedPosition(true);
  capabilities.SetSupportsDescrambleInfo(false);

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::GetBackendName(std::string& name)
{
  name = "Kofin PVR";
  return PVR_ERROR_NO_ERROR;
}
PVR_ERROR IptvSimple::GetBackendVersion(std::string& version)
{
  version = std::string(STR(KOFIN_VERSION));
  return PVR_ERROR_NO_ERROR;
}
PVR_ERROR IptvSimple::GetConnectionString(std::string& connection)
{
  connection = "connected";
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::GetDriveSpace(uint64_t& total, uint64_t& used)
{
  // Kodi's PVR GUI-info thread polls drive space on every tick that a backend
  // info-label (PVR.BackendDiskspace etc.) is on screen. When the client is not
  // yet constructed, or the user is logged out, we have no storage info to give.
  // Return NOT_IMPLEMENTED (which Kodi swallows silently) rather than
  // SERVER_ERROR (which Kodi logs as an error), and skip the doomed
  // /System/Info/Storage request that would fail while logged out.
  if (!m_jellyfinClient || m_settings->GetJellyfinAccessToken().empty())
    return PVR_ERROR_NOT_IMPLEMENTED;

  uint64_t totalBytes = 0;
  uint64_t usedBytes = 0;
  if (!m_jellyfinClient->GetStorageInfo(totalBytes, usedBytes))
    return PVR_ERROR_NOT_IMPLEMENTED;

  total = totalBytes / 1024;
  used = usedBytes / 1024;
  return PVR_ERROR_NO_ERROR;
}

void IptvSimple::Process()
{
  static const int TIMER_RECORDING_POLL_SECS = 60;

  unsigned int refreshTimer = 0;
  unsigned int timerRecordingPollTimer = 0;
  time_t lastRefreshTimeSeconds = std::time(nullptr);

  while (m_running)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(PROCESS_LOOP_WAIT_SECS * 1000));

    time_t currentRefreshTimeSeconds = std::time(nullptr);
    unsigned int elapsed = static_cast<unsigned int>(currentRefreshTimeSeconds - lastRefreshTimeSeconds);
    lastRefreshTimeSeconds = currentRefreshTimeSeconds;
    refreshTimer += elapsed;
    timerRecordingPollTimer += elapsed;

    // Clamp to a minimum of 1 hour: the settings control has no minimum, so a
    // 0 or negative interval would make this always true and reload every loop.
    const int updateIntervalHours = std::max(1, m_settings->GetJellyfinUpdateIntervalHours());
    if (refreshTimer >= static_cast<unsigned int>(updateIntervalHours * 3600))
      m_reloadChannelsGroupsAndEPG = true;

    // While logged out, skip all server polling: the requests would only get
    // a 401 and spam the log. The timers keep advancing so a poll/reload fires
    // promptly once the user logs back in.
    const bool loggedIn = !m_settings->GetJellyfinAccessToken().empty();

    // Poll timers/recordings every 60s so Kodi learns about new/changed
    // recordings without a restart (EPG recording indicators, widgets, etc.)
    if (m_running && loggedIn && timerRecordingPollTimer >= TIMER_RECORDING_POLL_SECS && m_recordingManager)
    {
      timerRecordingPollTimer = 0;
      Logger::Log(LEVEL_DEBUG, "%s - Polling timers/recordings", __FUNCTION__);
      m_recordingManager->Reload();
      TriggerTimerUpdate();
      TriggerRecordingUpdate();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running && loggedIn && m_reloadChannelsGroupsAndEPG)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));

      m_settings->ReadSettings();

      // Reload channels from Jellyfin
      if (m_channelLoader)
      {
        m_channels.Init();
        m_channelGroups.Init();
        m_channelLoader->LoadChannels(m_channels, m_channelGroups);
      }

      m_reloadChannelsGroupsAndEPG = false;
      refreshTimer = 0;
    }
  }
}

/***************************************************************************
 * Providers
 **************************************************************************/

PVR_ERROR IptvSimple::GetProvidersAmount(int& amount)
{
  amount = m_providers.GetNumProviders();

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::GetProviders(kodi::addon::PVRProvidersResultSet& results)
{
  std::vector<kodi::addon::PVRProvider> providers;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_providers.GetProviders(providers);
  }

  Logger::Log(LEVEL_DEBUG, "%s - providers available '%d'", __func__, providers.size());

  for (const auto& provider : providers)
    results.Add(provider);

  return PVR_ERROR_NO_ERROR;
}

/***************************************************************************
 * Channels
 **************************************************************************/

PVR_ERROR IptvSimple::GetChannelsAmount(int& amount)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  amount = m_channels.GetChannelsAmount();
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results)
{
  if (radio)
    return PVR_ERROR_NO_ERROR; // No radio support

  std::lock_guard<std::mutex> lock(m_mutex);

  return m_channels.GetChannels(results, radio);
}

#ifdef KODI_PVR_API_V9
PVR_ERROR IptvSimple::GetChannelStreamProperties(const kodi::addon::PVRChannel& channel, PVR_SOURCE /*source*/, std::vector<kodi::addon::PVRStreamProperty>& properties)
#else
PVR_ERROR IptvSimple::GetChannelStreamProperties(const kodi::addon::PVRChannel& channel, std::vector<kodi::addon::PVRStreamProperty>& properties)
#endif
{
  if (GetChannel(channel, m_currentChannel))
  {
    // Resolve live stream URL from Jellyfin via PlaybackInfo (with KofinProps overrides).
    // If the channel didn't pin an inputstream via M3U, fall back to the global
    // setting so BuildDeviceProfile / PostProcessTranscodingUrl pick the right
    // container + URL endpoint for the actual inputstream Kodi will use.
    auto overrides = iptvsimple::jellyfin::ChannelOverrides::FromChannel(m_currentChannel);
    if (!overrides.inputstream)
    {
      switch (m_settings->GetInputStream())
      {
        case 0: overrides.inputstream = "inputstream.ffmpegdirect"; break;
        case 1: overrides.inputstream = "inputstream.adaptive"; break;
        case 2: overrides.inputstream = "inputstream.ffmpeg"; break;
      }
    }
    std::string streamURL;
    if (m_channelLoader)
    {
      const std::string jellyfinId = m_channelLoader->GetJellyfinId(m_currentChannel.GetUniqueId());
      if (!jellyfinId.empty())
        streamURL = m_channelLoader->GetLiveStreamUrl(jellyfinId, overrides);
    }

    if (streamURL.empty())
    {
      Logger::Log(LogLevel::LEVEL_ERROR, "%s - Failed to resolve stream URL", __FUNCTION__);
      return PVR_ERROR_SERVER_ERROR;
    }

    // Check if this is a direct play URL (tuner URL, not a Jellyfin transcode URL)
    const bool isDirectPlay = streamURL.find(m_settings->GetJellyfinBaseUrl()) != 0;

    // Detect stream type: transcoded URLs are always Jellyfin HLS; direct play
    // URLs come straight from the tuner and could be HLS, DASH, TS, etc.
    const StreamType streamType = isDirectPlay
        ? StreamUtils::GetStreamType(streamURL, "", false)
        : StreamType::HLS;
    const std::string mimeType = StreamUtils::GetMimeType(streamType);
    const std::string manifestType = StreamUtils::GetManifestType(streamType);

    // Set stream properties
    properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, streamURL);
    if (!mimeType.empty())
      properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, mimeType);

    // Honor an inputstream override set via M3U KodiProps. Read directly
    // from the property map (not GetInputStreamName(), which is cached when
    // SetStreamURL() was called — before KodiProps were applied).
    std::string channelInputstream = m_currentChannel.GetProperty(PVR_STREAM_PROPERTY_INPUTSTREAM);
    if (channelInputstream.empty())
      channelInputstream = m_currentChannel.GetProperty("inputstream");
    const int inputStream = m_settings->GetInputStream();

    // Catchup: if direct play + ffmpegdirect + channel supports catchup,
    // use catchup mode instead of plain timeshift.
    // CatchupController is persistent — if GetEPGTagStreamProperties was called
    // first, it stored the programme times and ProcessChannelForPlayback will
    // use them (same pattern as pvr.iptvsimple).
    const bool useFfmpegDirect = !channelInputstream.empty()
      ? channelInputstream == "inputstream.ffmpegdirect"
      : inputStream == 0;
    const bool useAdaptive = !channelInputstream.empty()
      ? channelInputstream == "inputstream.adaptive"
      : inputStream == 1;

    if (isDirectPlay && useFfmpegDirect && m_currentChannel.IsCatchupSupported())
    {
      // Update channel's stream URL to the actual tuner URL
      m_currentChannel.SetStreamURL(streamURL);
      m_currentChannel.ConfigureCatchupMode();

      if (!m_catchupController)
        m_catchupController = std::make_unique<CatchupController>(m_settings);

      // Reset catchup state for live channel switch. Has no effect if we just
      // came from GetEPGTagStreamProperties (timeshifted EPG tag playback).
      m_catchupController->ResetCatchupState();

      std::map<std::string, std::string> catchupProperties;
      m_catchupController->ProcessChannelForPlayback(m_currentChannel, catchupProperties);

      // Use the resolved catchup URL if available (has timestamps baked in),
      // otherwise process the stream URL for now-only time specifiers.
      const std::string catchupUrl = m_catchupController->GetCatchupUrl(m_currentChannel);
      if (!catchupUrl.empty())
        streamURL = catchupUrl;
      else
        streamURL = m_catchupController->ProcessStreamUrl(m_currentChannel);

      // Update the stream URL property (was set to raw tuner URL earlier).
      // Derive MIME + manifest_type from the resolved catchup URL so TS catchup
      // sources aren't mislabelled as HLS (see AppendFfmpegDirectCatchupProperties).
      properties.clear();
      AppendFfmpegDirectCatchupProperties(properties, streamURL, m_currentChannel.IsCatchupTSStream());

      for (const auto& prop : catchupProperties)
        properties.emplace_back(prop.first, prop.second);

      Logger::Log(LogLevel::LEVEL_INFO, "%s - %s: %s (days=%d)",
                  __FUNCTION__, catchupUrl.empty() ? "Live stream" : "Catchup stream",
                  WebUtils::RedactUrl(streamURL).c_str(),
                  m_currentChannel.GetCatchupDays());
    }
    else if (useFfmpegDirect)
    {
      properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.ffmpegdirect");
      if (!manifestType.empty())
        properties.emplace_back("inputstream.ffmpegdirect.manifest_type", manifestType);
      properties.emplace_back("inputstream.ffmpegdirect.is_realtime_stream", "true");
      if (m_settings->GetTimeshiftEnabled())
        properties.emplace_back("inputstream.ffmpegdirect.stream_mode", "timeshift");
    }
    else if (useAdaptive)
    {
      properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.adaptive");
      if (!manifestType.empty())
        properties.emplace_back("inputstream.adaptive.manifest_type", manifestType);
    }
    else if (!channelInputstream.empty())
    {
      // Channel pinned a non-ffmpegdirect/adaptive inputstream (e.g. internal)
      properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, channelInputstream);
    }
    // else inputStream == 2: Kodi internal — no inputstream addon

    // Forward channel-level KodiProps from the M3U (inputstream.*, mimetype,
    // http-* etc.). Skip kofin-* (consumed by ChannelOverrides) and skip the
    // inputstream key itself (already set above). M3U props win over our
    // defaults via emplace order — Kodi keeps the last value for a given key.
    for (const auto& kv : m_currentChannel.GetProperties())
    {
      const std::string& key = kv.first;
      if (key.rfind("kofin-", 0) == 0)
        continue;
      if (key == PVR_STREAM_PROPERTY_INPUTSTREAM || key == "inputstream")
        continue;
      properties.emplace_back(key, kv.second);
    }

    // DEBUG, not INFO: Kodi writes INFO to kodi.log by default and this line
    // fires on every channel play — keep resolved stream URLs out of routine logs.
    Logger::Log(LogLevel::LEVEL_DEBUG, "%s - Stream URL: %s", __FUNCTION__, WebUtils::RedactUrl(streamURL).c_str());

    return PVR_ERROR_NO_ERROR;
  }

  return PVR_ERROR_SERVER_ERROR;
}

bool IptvSimple::OpenLiveStream(const kodi::addon::PVRChannel& channel)
{
  Logger::Log(LEVEL_DEBUG, "%s - Opening live stream for channel UID %d", __FUNCTION__, channel.GetUniqueId());
  return true;
}

void IptvSimple::CloseLiveStream()
{
  Logger::Log(LEVEL_INFO, "%s - CloseLiveStream called by Kodi", __FUNCTION__);
  if (m_channelLoader)
    m_channelLoader->CloseLiveStream();
}

bool IptvSimple::GetChannel(const kodi::addon::PVRChannel& channel, Channel& myChannel)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  return m_channels.GetChannel(channel, myChannel);
}

bool IptvSimple::GetChannel(unsigned int uniqueChannelId, iptvsimple::data::Channel& myChannel)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  return m_channels.GetChannel(uniqueChannelId, myChannel);
}

/***************************************************************************
 * Channel Groups
 **************************************************************************/

PVR_ERROR IptvSimple::GetChannelGroupsAmount(int& amount)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  amount = m_channelGroups.GetChannelGroupsAmount();
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results)
{
  if (radio)
    return PVR_ERROR_NO_ERROR; // No radio support

  std::lock_guard<std::mutex> lock(m_mutex);

  return m_channelGroups.GetChannelGroups(results, radio);
}

PVR_ERROR IptvSimple::GetChannelGroupMembers(const kodi::addon::PVRChannelGroup& group, kodi::addon::PVRChannelGroupMembersResultSet& results)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  return m_channelGroups.GetChannelGroupMembers(group, results);
}

/***************************************************************************
 * EPG
 **************************************************************************/

PVR_ERROR IptvSimple::GetEPGForChannel(int channelUid, time_t start, time_t end, kodi::addon::PVREPGTagsResultSet& results)
{
  if (m_channelLoader)
    return m_channelLoader->LoadEpg(channelUid, start, end, results);

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::SetEPGMaxPastDays(int epgMaxPastDays)
{
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::SetEPGMaxFutureDays(int epgMaxFutureDays)
{
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::IsEPGTagPlayable(const kodi::addon::PVREPGTag& tag, bool& isPlayable)
{
  // A past EPG entry is playable if the channel supports catchup
  Channel channel(m_settings);
  if (GetChannel(tag.GetUniqueChannelId(), channel) && channel.IsCatchupSupported())
  {
    time_t now = std::time(nullptr);
    time_t catchupWindowStart = now - channel.GetCatchupDaysInSeconds();
    // Playable if the programme has started, is within the catchup window,
    // and (if setting enabled) has finished
    isPlayable = tag.GetStartTime() >= catchupWindowStart && tag.GetStartTime() <= now
      && (!m_settings->CatchupOnlyOnFinishedProgrammes() || tag.GetEndTime() < now);
  }
  else
  {
    isPlayable = false;
  }

  // Also playable if a completed recording exists for this programme
  if (!isPlayable && m_recordingManager)
  {
    isPlayable = m_recordingManager->HasRecordingForEpg(
        tag.GetUniqueBroadcastId(), static_cast<int>(tag.GetUniqueChannelId()));
  }

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::IsEPGTagRecordable(const kodi::addon::PVREPGTag& tag, bool& isRecordable)
{
  Channel channel(m_settings);
  if (GetChannel(tag.GetUniqueChannelId(), channel))
  {
    const auto& props = channel.GetProperties();
    auto it = props.find("kofin-disable-pvr");
    if (it != props.end() && it->second == "true")
    {
      isRecordable = false;
      return PVR_ERROR_NO_ERROR;
    }
  }
  isRecordable = true;
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::GetEPGTagStreamProperties(const kodi::addon::PVREPGTag& tag, std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  // If a recording exists for this EPG entry, play the recording directly
  if (m_recordingManager)
  {
    std::string recordingId = m_recordingManager->GetRecordingIdForEpg(
        tag.GetUniqueBroadcastId(), static_cast<int>(tag.GetUniqueChannelId()));
    if (!recordingId.empty())
    {
      Logger::Log(LogLevel::LEVEL_INFO, "%s - Playing recording %s for EPG '%s'",
                  __FUNCTION__, recordingId.c_str(), tag.GetTitle().c_str());
      kodi::addon::PVRRecording rec;
      rec.SetRecordingId(recordingId);
      return m_recordingManager->GetRecordingStreamProperties(rec, properties);
    }
  }

  Channel channel(m_settings);
  if (!GetChannel(tag.GetUniqueChannelId(), channel) || !channel.IsCatchupSupported())
    return PVR_ERROR_FAILED;

  // Resolve the direct play URL for this channel (with KofinProps overrides).
  // Same global-inputstream fallback as the live channel path.
  auto epgOverrides = iptvsimple::jellyfin::ChannelOverrides::FromChannel(channel);
  if (!epgOverrides.inputstream)
  {
    switch (m_settings->GetInputStream())
    {
      case 0: epgOverrides.inputstream = "inputstream.ffmpegdirect"; break;
      case 1: epgOverrides.inputstream = "inputstream.adaptive"; break;
      case 2: epgOverrides.inputstream = "inputstream.ffmpeg"; break;
    }
  }
  std::string streamURL;
  if (m_channelLoader)
  {
    const std::string jellyfinId = m_channelLoader->GetJellyfinId(channel.GetUniqueId());
    if (!jellyfinId.empty())
      streamURL = m_channelLoader->GetLiveStreamUrl(jellyfinId, epgOverrides);
  }

  if (streamURL.empty())
    return PVR_ERROR_FAILED;

  const bool isDirectPlay = streamURL.find(m_settings->GetJellyfinBaseUrl()) != 0;
  if (!isDirectPlay)
    return PVR_ERROR_FAILED; // Catchup only works with direct play

  // Update channel stream URL to the tuner URL and regenerate catchup source
  channel.SetStreamURL(streamURL);
  channel.ConfigureCatchupMode();

  if (!m_catchupController)
    m_catchupController = std::make_unique<CatchupController>(m_settings);

  std::map<std::string, std::string> catchupProperties;

  // Process the EPG tag — stores programme times in the persistent controller.
  // For timeshifted playback, GetChannelStreamProperties picks up state next.
  // For video playback, we return the catchup URL directly.
  if (m_settings->CatchupPlayEpgAsLive() && (channel.CatchupSupportsTimeshifting() || channel.GetCatchupMode() == CatchupMode::VOD))
  {
    m_catchupController->ProcessEPGTagForTimeshiftedPlayback(tag, channel, catchupProperties);
  }
  else
  {
    m_catchupController->ResetCatchupState();
    m_catchupController->ProcessEPGTagForVideoPlayback(tag, channel, catchupProperties);
  }

  const std::string catchupUrl = m_catchupController->GetCatchupUrl(channel);
  if (!catchupUrl.empty())
  {
    // Derive MIME + manifest_type from the resolved catchup URL so TS catchup
    // sources aren't mislabelled as HLS (see AppendFfmpegDirectCatchupProperties).
    AppendFfmpegDirectCatchupProperties(properties, catchupUrl, channel.IsCatchupTSStream());

    for (const auto& prop : catchupProperties)
      properties.emplace_back(prop.first, prop.second);

    Logger::Log(LogLevel::LEVEL_INFO, "%s - EPG catchup for '%s' on '%s': %s",
                __FUNCTION__, tag.GetTitle().c_str(), channel.GetChannelName().c_str(),
                WebUtils::RedactUrl(catchupUrl).c_str());
  }
  else
  {
    Logger::Log(LogLevel::LEVEL_WARNING, "%s - No catchup URL for '%s'", __FUNCTION__, tag.GetTitle().c_str());
    return PVR_ERROR_FAILED;
  }

  return PVR_ERROR_NO_ERROR;
}

/***************************************************************************
 * Timers
 **************************************************************************/

PVR_ERROR IptvSimple::GetTimerTypes(std::vector<kodi::addon::PVRTimerType>& types)
{
  if (m_recordingManager)
    return m_recordingManager->GetTimerTypes(types);
  return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR IptvSimple::GetTimersAmount(int& amount)
{
  if (m_recordingManager)
  {
    amount = m_recordingManager->GetTimersAmount();
    return PVR_ERROR_NO_ERROR;
  }
  amount = 0;
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::GetTimers(kodi::addon::PVRTimersResultSet& results)
{
  if (m_recordingManager)
    return m_recordingManager->GetTimers(results);
  return PVR_ERROR_NO_ERROR;
}

void IptvSimple::RunTimerOpAsync(std::function<void()> op)
{
  std::thread(std::move(op)).detach();
}

PVR_ERROR IptvSimple::AddTimer(const kodi::addon::PVRTimer& timer)
{
  if (!m_recordingManager)
    return PVR_ERROR_NOT_IMPLEMENTED;

  // Run the create (program lookup + POST + model reload) off Kodi's main
  // thread: synchronously it blocks the UI for several seconds. PVRTimer is a
  // POD-backed value type, so capturing it by value is a safe deep copy. The
  // triggers fire only after Reload() completes, so Kodi re-reads fresh data
  // (the in-progress recording, EPG "play recording" entry, widgets).
  RunTimerOpAsync([this, timer]() {
    if (m_recordingManager->AddTimer(timer) != PVR_ERROR_NO_ERROR)
    {
      kodi::QueueNotification(QUEUE_ERROR, "Kofin PVR", "Failed to add timer");
      return;
    }

    TriggerTimerUpdate();
    TriggerRecordingUpdate();
    TriggerEpgUpdate(timer.GetClientChannelUid());

    // Jellyfin creates the timer synchronously but materializes the in-progress
    // recording a few seconds later, so the reload above (run right after the
    // POST) doesn't see it yet. If this timer is recording now, reload a few
    // more times until the recording appears, so the recordings widgets and the
    // EPG "play recording" entry update promptly instead of waiting for the next
    // 60s poll. Future timers skip this (nothing to materialize yet).
    const time_t now = std::time(nullptr);
    const bool recordingNow = timer.GetStartTime() <= now && now < timer.GetEndTime();
    if (recordingNow)
    {
      for (int attempt = 0; attempt < 5 && m_running &&
           !m_recordingManager->HasRecordingForEpg(timer.GetEPGUid(), timer.GetClientChannelUid());
           ++attempt)
      {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        m_recordingManager->Reload();
        // TriggerTimerUpdate too: once the recording materialises, the timer's
        // state flips Scheduled -> Recording (see JellyfinRecordingManager::
        // Reload), which is what turns the guide's clock icon into the red dot.
        TriggerTimerUpdate();
        TriggerRecordingUpdate();
        TriggerEpgUpdate(timer.GetClientChannelUid());
      }
    }
  });
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::DeleteTimer(const kodi::addon::PVRTimer& timer, bool forceDelete)
{
  if (!m_recordingManager)
    return PVR_ERROR_NOT_IMPLEMENTED;

  // As with AddTimer, run the delete + reload off the main thread and trigger
  // UI updates only after the reload completes. Previously the triggers fired
  // before the async reload, so Kodi briefly re-read stale data (the deleted
  // timer / still-in-progress recording).
  RunTimerOpAsync([this, timer, forceDelete]() {
    if (m_recordingManager->DeleteTimer(timer, forceDelete) == PVR_ERROR_NO_ERROR)
    {
      TriggerTimerUpdate();
      TriggerRecordingUpdate();
      TriggerEpgUpdate(timer.GetClientChannelUid());
    }
    else
    {
      kodi::QueueNotification(QUEUE_ERROR, "Kofin PVR", "Failed to delete timer");
    }
  });
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::UpdateTimer(const kodi::addon::PVRTimer& timer)
{
  if (m_recordingManager)
  {
    PVR_ERROR ret = m_recordingManager->UpdateTimer(timer);
    if (ret == PVR_ERROR_NO_ERROR)
      TriggerTimerUpdate();
    return ret;
  }
  return PVR_ERROR_NOT_IMPLEMENTED;
}

/***************************************************************************
 * Recordings
 **************************************************************************/

PVR_ERROR IptvSimple::GetRecordingsAmount(bool deleted, int& amount)
{
  if (deleted)
  {
    amount = 0;
    return PVR_ERROR_NO_ERROR;
  }
  if (m_recordingManager)
  {
    amount = m_recordingManager->GetRecordingsAmount();
    return PVR_ERROR_NO_ERROR;
  }
  amount = 0;
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::GetRecordings(bool deleted, kodi::addon::PVRRecordingsResultSet& results)
{
  if (deleted)
    return PVR_ERROR_NO_ERROR;
  if (m_recordingManager)
    return m_recordingManager->GetRecordings(results);
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::DeleteRecording(const kodi::addon::PVRRecording& recording)
{
  if (m_recordingManager)
  {
    PVR_ERROR ret = m_recordingManager->DeleteRecording(recording);
    if (ret == PVR_ERROR_NO_ERROR)
      TriggerRecordingUpdate();
    return ret;
  }
  return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR IptvSimple::GetRecordingStreamProperties(const kodi::addon::PVRRecording& recording, std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  if (m_recordingManager)
    return m_recordingManager->GetRecordingStreamProperties(recording, properties);
  return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR IptvSimple::SetRecordingPlayCount(const kodi::addon::PVRRecording& recording, int count)
{
  if (m_recordingManager)
    return m_recordingManager->SetRecordingPlayCount(recording, count);
  return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR IptvSimple::SetRecordingLastPlayedPosition(const kodi::addon::PVRRecording& recording, int lastplayedposition)
{
  if (m_recordingManager)
    return m_recordingManager->SetRecordingLastPlayedPosition(recording, lastplayedposition);
  return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR IptvSimple::GetRecordingLastPlayedPosition(const kodi::addon::PVRRecording& recording, int& position)
{
  if (m_recordingManager)
    return m_recordingManager->GetRecordingLastPlayedPosition(recording, position);
  return PVR_ERROR_NOT_IMPLEMENTED;
}

/***************************************************************************
 * Recording Byte-Stream (Recordings section playback path)
 *
 * Kodi's PVR Recordings window does NOT call GetRecordingStreamProperties.
 * It goes directly to OpenRecordedStream / ReadRecordedStream.
 **************************************************************************/

// The byte-stream itself lives in JellyfinRecordingManager; these overrides
// just adapt the Kodi v21/v22 signatures (v22 adds a streamId) and delegate.
#ifdef KODI_PVR_API_V9
bool IptvSimple::OpenRecordedStream(const kodi::addon::PVRRecording& recording, int64_t& streamId)
{
  streamId = 1;
  return m_recordingManager && m_recordingManager->OpenRecordedStream(recording);
}
void IptvSimple::CloseRecordedStream(int64_t) { if (m_recordingManager) m_recordingManager->CloseRecordedStream(); }
int IptvSimple::ReadRecordedStream(int64_t, unsigned char* buffer, unsigned int size) { return m_recordingManager ? m_recordingManager->ReadRecordedStream(buffer, size) : -1; }
int64_t IptvSimple::SeekRecordedStream(int64_t, int64_t position, int whence) { return m_recordingManager ? m_recordingManager->SeekRecordedStream(position, whence) : -1; }
int64_t IptvSimple::LengthRecordedStream(int64_t) { return m_recordingManager ? m_recordingManager->LengthRecordedStream() : -1; }
#else
bool IptvSimple::OpenRecordedStream(const kodi::addon::PVRRecording& recording) { return m_recordingManager && m_recordingManager->OpenRecordedStream(recording); }
void IptvSimple::CloseRecordedStream() { if (m_recordingManager) m_recordingManager->CloseRecordedStream(); }
int IptvSimple::ReadRecordedStream(unsigned char* buffer, unsigned int size) { return m_recordingManager ? m_recordingManager->ReadRecordedStream(buffer, size) : -1; }
int64_t IptvSimple::SeekRecordedStream(int64_t position, int whence) { return m_recordingManager ? m_recordingManager->SeekRecordedStream(position, whence) : -1; }
int64_t IptvSimple::LengthRecordedStream() { return m_recordingManager ? m_recordingManager->LengthRecordedStream() : -1; }
#endif

/***************************************************************************
 * Signal Status
 **************************************************************************/

PVR_ERROR IptvSimple::GetSignalStatus(int channelUid, kodi::addon::PVRSignalStatus& signalStatus)
{
  signalStatus.SetAdapterName("Kofin Jellyfin PVR");
  signalStatus.SetAdapterStatus("OK");

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::CallSettingsMenuHook(const kodi::addon::PVRMenuhook& menuhook)
{
  kodi::addon::OpenSettings();
  return PVR_ERROR_NO_ERROR;
}

/***************************************************************************
 * Stream State
 **************************************************************************/

/***************************************************************************
 * InstanceSettings
 **************************************************************************/

void IptvSimple::OnSettingChanged(const std::string& settingName, const kodi::addon::CSettingValue& settingValue)
{
  // Handle action buttons — only fire when the Python trigger script sets value to "trigger".
  // Kodi calls SetSetting for ALL settings on dialog close, so we must check the value
  // to avoid firing all buttons at once. Reset after handling to prevent re-triggering.
  if (settingName == "loginButton")
  {
    if (settingValue.GetString() == "trigger")
    {
      kodi::addon::SetSettingString("loginButton", "");
      m_auth->RunLogin();
    }
    return;
  }
  else if (settingName == "logoutButton")
  {
    if (settingValue.GetString() == "trigger")
    {
      kodi::addon::SetSettingString("logoutButton", "");
      m_auth->RunLogout();
    }
    return;
  }
  else if (settingName == "testConnection")
  {
    if (settingValue.GetString() == "trigger")
    {
      kodi::addon::SetSettingString("testConnection", "");
      m_auth->TestConnection();
    }
    return;
  }
  else if (settingName == "restartAddon")
  {
    if (settingValue.GetString() == "trigger")
    {
      kodi::addon::SetSettingString("restartAddon", "");
      m_needsRestart = true;
    }
    return;
  }
  // Ignore programmatic updates to prevent callback loops
  if (settingName == "isLoggedIn" || settingName == "jellyfinServerName" ||
      settingName == "jellyfinDisplayUsername" || settingName == "jellyfinAccessToken" ||
      settingName == "jellyfinUserId" ||
      settingName == "sessionItemId" || settingName == "sessionMediaSourceId" ||
      settingName == "sessionPlaySessionId" || settingName == "sessionLiveStreamId" ||
      settingName == "sessionPlayMethod")
  {
    return;
  }

  // list[string] settings can't be read by GetSettingString — capture them
  // here where TransferSettings delivers the value as a plain string.
  if (settingName == "directPlayVideoCodecs" || settingName == "directPlayAudioCodecs" ||
      settingName == "allowedHdrTypes")
  {
    if (settingName == "directPlayVideoCodecs")
      m_settings->SetDirectPlayVideoCodecs(settingValue.GetString());
    else if (settingName == "directPlayAudioCodecs")
      m_settings->SetDirectPlayAudioCodecs(settingValue.GetString());
    else
      m_settings->SetAllowedHdrTypes(settingValue.GetString());
    return;
  }

  // For regular settings, re-read all settings and flag for reload
  m_settings->ReadSettings();

  if (!m_reloadChannelsGroupsAndEPG)
    m_reloadChannelsGroupsAndEPG = true;
}
