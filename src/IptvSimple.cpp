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

#include <chrono>
#include <ctime>
#include <memory>

#include <kodi/General.h>
#include <kodi/tools/StringUtils.h>

using namespace iptvsimple;
using namespace iptvsimple::data;
using namespace iptvsimple::utilities;
using namespace kodi::tools;

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

  m_running = true;
  m_thread = std::thread([&] { Process(); });
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

    if (refreshTimer >= static_cast<unsigned int>(m_settings->GetJellyfinUpdateIntervalHours() * 3600))
      m_reloadChannelsGroupsAndEPG = true;

    // Poll timers/recordings every 60s so Kodi learns about new/changed
    // recordings without a restart (EPG recording indicators, widgets, etc.)
    if (m_running && timerRecordingPollTimer >= TIMER_RECORDING_POLL_SECS && m_recordingManager)
    {
      timerRecordingPollTimer = 0;
      Logger::Log(LEVEL_DEBUG, "%s - Polling timers/recordings", __FUNCTION__);
      m_recordingManager->Reload();
      TriggerTimerUpdate();
      TriggerRecordingUpdate();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running && m_reloadChannelsGroupsAndEPG)
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
      const std::string& jellyfinId = m_channelLoader->GetJellyfinId(m_currentChannel.GetUniqueId());
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

      // Update the stream URL property (was set to raw tuner URL earlier)
      properties.clear();
      properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, streamURL);
      properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, "application/vnd.apple.mpegurl");
      properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.ffmpegdirect");
      properties.emplace_back("inputstream.ffmpegdirect.manifest_type", "hls");

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

    Logger::Log(LogLevel::LEVEL_INFO, "%s - Stream URL: %s", __FUNCTION__, WebUtils::RedactUrl(streamURL).c_str());

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
    const std::string& jellyfinId = m_channelLoader->GetJellyfinId(channel.GetUniqueId());
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
    properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, catchupUrl);
    properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, "application/vnd.apple.mpegurl");
    properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.ffmpegdirect");
    properties.emplace_back("inputstream.ffmpegdirect.manifest_type", "hls");

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

PVR_ERROR IptvSimple::AddTimer(const kodi::addon::PVRTimer& timer)
{
  if (m_recordingManager)
  {
    PVR_ERROR ret = m_recordingManager->AddTimer(timer);
    if (ret == PVR_ERROR_NO_ERROR)
    {
      TriggerTimerUpdate();
      TriggerEpgUpdate(timer.GetClientChannelUid());
    }
    return ret;
  }
  return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR IptvSimple::DeleteTimer(const kodi::addon::PVRTimer& timer, bool forceDelete)
{
  if (m_recordingManager)
  {
    PVR_ERROR ret = m_recordingManager->DeleteTimer(timer, forceDelete);
    if (ret == PVR_ERROR_NO_ERROR)
    {
      TriggerTimerUpdate();
      TriggerEpgUpdate(timer.GetClientChannelUid());
    }
    return ret;
  }
  return PVR_ERROR_NOT_IMPLEMENTED;
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

bool IptvSimple::OpenRecordedStreamImpl(const kodi::addon::PVRRecording& recording)
{
  CloseRecordedStreamImpl();

  const std::string recordingId = recording.GetRecordingId();
  if (!m_jellyfinClient)
    return false;

  const std::string streamUrl = m_jellyfinClient->GetBaseUrl()
    + "/Videos/" + recordingId + "/stream?static=true&api_key="
    + m_jellyfinClient->GetAccessToken();

  Logger::Log(LEVEL_INFO, "%s - Opening recording stream for %s", __FUNCTION__, recordingId.c_str());

  if (!m_recordingStream.OpenFile(streamUrl, ADDON_READ_NO_CACHE))
  {
    Logger::Log(LEVEL_ERROR, "%s - Failed to open recording stream: %s", __FUNCTION__, recordingId.c_str());
    return false;
  }

  m_recordingStreamOpen = true;
  Logger::Log(LEVEL_INFO, "%s - Recording stream open, length: %lld",
              __FUNCTION__, static_cast<long long>(m_recordingStream.GetLength()));
  return true;
}

void IptvSimple::CloseRecordedStreamImpl()
{
  if (m_recordingStreamOpen)
  {
    Logger::Log(LEVEL_INFO, "%s - Closing recording stream", __FUNCTION__);
    m_recordingStream.Close();
    m_recordingStreamOpen = false;
  }
}

int IptvSimple::ReadRecordedStreamImpl(unsigned char* buffer, unsigned int size)
{
  if (!m_recordingStreamOpen)
    return -1;

  ssize_t bytesRead = m_recordingStream.Read(buffer, size);
  return static_cast<int>(bytesRead);
}

int64_t IptvSimple::SeekRecordedStreamImpl(int64_t position, int whence)
{
  if (!m_recordingStreamOpen)
    return -1;

  return m_recordingStream.Seek(position, whence);
}

int64_t IptvSimple::LengthRecordedStreamImpl()
{
  if (!m_recordingStreamOpen)
    return -1;

  return m_recordingStream.GetLength();
}

// Kodi v22 (PVR API 9.x) adds streamId parameter to all recording stream methods
#ifdef KODI_PVR_API_V9
bool IptvSimple::OpenRecordedStream(const kodi::addon::PVRRecording& recording, int64_t& streamId)
{
  streamId = 1;
  return OpenRecordedStreamImpl(recording);
}
void IptvSimple::CloseRecordedStream(int64_t) { CloseRecordedStreamImpl(); }
int IptvSimple::ReadRecordedStream(int64_t, unsigned char* buffer, unsigned int size) { return ReadRecordedStreamImpl(buffer, size); }
int64_t IptvSimple::SeekRecordedStream(int64_t, int64_t position, int whence) { return SeekRecordedStreamImpl(position, whence); }
int64_t IptvSimple::LengthRecordedStream(int64_t) { return LengthRecordedStreamImpl(); }
#else
bool IptvSimple::OpenRecordedStream(const kodi::addon::PVRRecording& recording) { return OpenRecordedStreamImpl(recording); }
void IptvSimple::CloseRecordedStream() { CloseRecordedStreamImpl(); }
int IptvSimple::ReadRecordedStream(unsigned char* buffer, unsigned int size) { return ReadRecordedStreamImpl(buffer, size); }
int64_t IptvSimple::SeekRecordedStream(int64_t position, int whence) { return SeekRecordedStreamImpl(position, whence); }
int64_t IptvSimple::LengthRecordedStream() { return LengthRecordedStreamImpl(); }
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
  if (settingName == "directPlayVideoCodecs" || settingName == "directPlayAudioCodecs")
  {
    if (settingName == "directPlayVideoCodecs")
      m_settings->SetDirectPlayVideoCodecs(settingValue.GetString());
    else
      m_settings->SetDirectPlayAudioCodecs(settingValue.GetString());
    return;
  }

  // For regular settings, re-read all settings and flag for reload
  m_settings->ReadSettings();

  if (!m_reloadChannelsGroupsAndEPG)
    m_reloadChannelsGroupsAndEPG = true;
}
