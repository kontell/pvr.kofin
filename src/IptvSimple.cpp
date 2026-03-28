/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "IptvSimple.h"

#include "iptvsimple/InstanceSettings.h"
#include "iptvsimple/utilities/Logger.h"
#include "iptvsimple/utilities/StreamUtils.h"
#include "iptvsimple/utilities/TimeUtils.h"
#include "iptvsimple/utilities/WebUtils.h"

#include <chrono>
#include <ctime>
#include <memory>

#include <kodi/General.h>
#include <kodi/gui/dialogs/Keyboard.h>
#include <kodi/gui/dialogs/Progress.h>
#include <kodi/gui/dialogs/Select.h>
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
  FetchAndStoreServerName();

  m_channelLoader = std::make_shared<iptvsimple::jellyfin::JellyfinChannelLoader>(m_jellyfinClient, m_settings);
  m_channelLoader->LoadChannels(m_channels, m_channelGroups);

  // Create recording manager and load initial data
  m_recordingManager = std::make_shared<iptvsimple::jellyfin::JellyfinRecordingManager>(
      m_jellyfinClient, m_channelLoader, m_settings);
  m_recordingManager->Reload();

  kodi::Log(ADDON_LOG_INFO, "%s Starting separate client update thread...", __FUNCTION__);

  m_running = true;
  m_thread = std::thread([&] { Process(); });
}

void IptvSimple::TestConnection()
{
  m_settings->ReadSettings();

  if (m_settings->GetJellyfinServerAddress().empty())
  {
    kodi::QueueNotification(QUEUE_ERROR, "Kofin PVR", kodi::addon::GetLocalizedString(30714));
    return;
  }

  auto testClient = std::make_shared<iptvsimple::jellyfin::JellyfinClient>(m_settings);

  if (testClient->Authenticate())
  {
    kodi::QueueNotification(QUEUE_INFO, "Kofin PVR", "Connection successful!");
    Logger::Log(LEVEL_INFO, "%s - Test connection successful", __FUNCTION__);
  }
  else
  {
    kodi::QueueNotification(QUEUE_ERROR, "Kofin PVR", "Connection failed.");
    Logger::Log(LEVEL_ERROR, "%s - Test connection failed", __FUNCTION__);
  }
}

void IptvSimple::RunLogin()
{
  m_settings->ReadSettings();

  if (m_settings->GetJellyfinServerAddress().empty())
  {
    kodi::QueueNotification(QUEUE_ERROR, "Kofin PVR", kodi::addon::GetLocalizedString(30714));
    return;
  }

  // Show Select dialog: Username/Password or Quick Connect
  std::vector<std::string> entries;
  entries.push_back(kodi::addon::GetLocalizedString(30706)); // "Username/Password"
  entries.push_back(kodi::addon::GetLocalizedString(30707)); // "Quick Connect"

  int selected = kodi::gui::dialogs::Select::Show(
    kodi::addon::GetLocalizedString(30708), entries); // "Choose login method"

  if (selected < 0)
    return; // cancelled

  bool success = false;
  if (selected == 0)
    success = LoginWithPassword();
  else
    success = LoginWithQuickConnect();

  if (success)
  {
    FetchAndStoreServerName();
    m_settings->SetIsLoggedIn(true);
    kodi::QueueNotification(QUEUE_INFO, "Kofin PVR", kodi::addon::GetLocalizedString(30711));
  }
}

bool IptvSimple::LoginWithPassword()
{
  std::string username;
  if (!kodi::gui::dialogs::Keyboard::ShowAndGetInput(username,
      kodi::addon::GetLocalizedString(30709), false, false))
    return false;

  if (username.empty())
    return false;

  std::string password;
  if (!kodi::gui::dialogs::Keyboard::ShowAndGetInput(password,
      kodi::addon::GetLocalizedString(30710), false, true))
    return false;

  auto client = std::make_shared<iptvsimple::jellyfin::JellyfinClient>(m_settings);

  if (!client->AuthenticateByPassword(username, password))
  {
    kodi::QueueNotification(QUEUE_ERROR, "Kofin PVR", kodi::addon::GetLocalizedString(30713));
    return false;
  }

  // AuthenticateByPassword -> SetAuthFromResponse already persisted token, userId, displayUsername
  return true;
}

bool IptvSimple::LoginWithQuickConnect()
{
  auto qcClient = std::make_shared<iptvsimple::jellyfin::JellyfinClient>(m_settings);

  std::string code;
  if (!qcClient->StartQuickConnect(code))
  {
    Logger::Log(LEVEL_ERROR, "%s - Failed to initiate Quick Connect", __FUNCTION__);
    kodi::QueueNotification(QUEUE_ERROR, "Kofin PVR", "Quick Connect initiation failed");
    return false;
  }

  auto progress = std::make_unique<kodi::gui::dialogs::CProgress>();
  progress->SetHeading("Kofin PVR - Quick Connect");
  progress->SetLine(1, "Enter this code in your Jellyfin dashboard:");
  progress->SetLine(2, "Code: " + code);
  progress->SetLine(3, "Waiting for authorization...");
  progress->SetCanCancel(true);
  progress->SetPercentage(0);
  progress->ShowProgressBar(true);
  progress->Open();

  const int maxAttempts = 100;
  std::string userId, accessToken;

  for (int attempt = 0; attempt < maxAttempts; ++attempt)
  {
    if (progress->IsCanceled())
    {
      Logger::Log(LEVEL_INFO, "%s - Quick Connect cancelled by user", __FUNCTION__);
      return false;
    }

    progress->SetPercentage(attempt * 100 / maxAttempts);

    if (qcClient->CheckQuickConnect(userId, accessToken))
    {
      progress.reset();
      Logger::Log(LEVEL_INFO, "%s - Quick Connect authenticated", __FUNCTION__);
      return true;
    }

    // Stepped sleep: check cancel every 200ms instead of blocking 3s
    for (int ms = 0; ms < 3000; ms += 200)
    {
      if (progress->IsCanceled())
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }

  progress.reset();
  Logger::Log(LEVEL_ERROR, "%s - Quick Connect timed out", __FUNCTION__);
  kodi::QueueNotification(QUEUE_ERROR, "Kofin PVR", "Quick Connect timed out");
  return false;
}

void IptvSimple::RunLogout()
{
  m_settings->SetJellyfinAccessToken("");
  m_settings->SetJellyfinUserId("");
  m_settings->SetJellyfinServerName("");
  m_settings->SetJellyfinDisplayUsername("");
  m_settings->SetIsLoggedIn(false);

  kodi::QueueNotification(QUEUE_INFO, "Kofin PVR", kodi::addon::GetLocalizedString(30712));
  Logger::Log(LEVEL_INFO, "%s - User logged out", __FUNCTION__);
}

void IptvSimple::FetchAndStoreServerName()
{
  auto client = m_jellyfinClient;
  if (!client)
    client = std::make_shared<iptvsimple::jellyfin::JellyfinClient>(m_settings);

  // Fetch server name (public endpoint, no auth needed)
  std::string serverName;
  if (client->FetchServerInfo(serverName))
    m_settings->SetJellyfinServerName(serverName);

  // If display username not yet set (e.g. Quick Connect), fetch it
  if (m_settings->GetJellyfinDisplayUsername().empty() && !m_settings->GetJellyfinUserId().empty())
  {
    Json::Value userInfo = client->SendGet("/Users/" + m_settings->GetJellyfinUserId());
    if (!userInfo.isNull() && userInfo.isMember("Name"))
      m_settings->SetJellyfinDisplayUsername(userInfo["Name"].asString());
  }
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
  unsigned int refreshTimer = 0;
  time_t lastRefreshTimeSeconds = std::time(nullptr);

  while (m_running)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(PROCESS_LOOP_WAIT_SECS * 1000));

    time_t currentRefreshTimeSeconds = std::time(nullptr);
    refreshTimer += static_cast<unsigned int>(currentRefreshTimeSeconds - lastRefreshTimeSeconds);
    lastRefreshTimeSeconds = currentRefreshTimeSeconds;

    if (refreshTimer >= static_cast<unsigned int>(m_settings->GetJellyfinUpdateIntervalMins() * 60))
      m_reloadChannelsGroupsAndEPG = true;

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
    // Resolve live stream URL from Jellyfin via PlaybackInfo
    std::string streamURL;
    if (m_channelLoader)
    {
      const std::string& jellyfinId = m_channelLoader->GetJellyfinId(m_currentChannel.GetUniqueId());
      if (!jellyfinId.empty())
        streamURL = m_channelLoader->GetLiveStreamUrl(jellyfinId);
    }

    if (streamURL.empty())
    {
      Logger::Log(LogLevel::LEVEL_ERROR, "%s - Failed to resolve stream URL", __FUNCTION__);
      return PVR_ERROR_SERVER_ERROR;
    }

    // Set stream properties directly for Jellyfin HLS streams (matching jellyfin-kodi)
    properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, streamURL);
    properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, "application/vnd.apple.mpegurl");

    const int inputStream = m_settings->GetInputStream();
    if (inputStream == 0) // FFmpegDirect
    {
      properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.ffmpegdirect");
      properties.emplace_back("inputstream.ffmpegdirect.manifest_type", "hls");
      properties.emplace_back("inputstream.ffmpegdirect.is_realtime_stream", "true");
      if (m_settings->GetTimeshiftEnabled())
        properties.emplace_back("inputstream.ffmpegdirect.stream_mode", "timeshift");
    }
    else if (inputStream == 1) // Adaptive
    {
      properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.adaptive");
      properties.emplace_back("inputstream.adaptive.manifest_type", "hls");
    }
    // inputStream == 2: Kodi internal — no inputstream addon

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
    return m_recordingManager->AddTimer(timer);
  return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR IptvSimple::DeleteTimer(const kodi::addon::PVRTimer& timer, bool forceDelete)
{
  if (m_recordingManager)
    return m_recordingManager->DeleteTimer(timer, forceDelete);
  return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR IptvSimple::UpdateTimer(const kodi::addon::PVRTimer& timer)
{
  if (m_recordingManager)
    return m_recordingManager->UpdateTimer(timer);
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
    return m_recordingManager->DeleteRecording(recording);
  return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR IptvSimple::GetRecordingStreamProperties(const kodi::addon::PVRRecording& recording, std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  if (m_recordingManager)
    return m_recordingManager->GetRecordingStreamProperties(recording, properties);
  return PVR_ERROR_NOT_IMPLEMENTED;
}

/***************************************************************************
 * Signal Status
 **************************************************************************/

PVR_ERROR IptvSimple::GetSignalStatus(int channelUid, kodi::addon::PVRSignalStatus& signalStatus)
{
  signalStatus.SetAdapterName("Kofin Jellyfin PVR");
  signalStatus.SetAdapterStatus("OK");

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
      RunLogin();
    }
    return;
  }
  else if (settingName == "logoutButton")
  {
    if (settingValue.GetString() == "trigger")
    {
      kodi::addon::SetSettingString("logoutButton", "");
      RunLogout();
    }
    return;
  }
  else if (settingName == "testConnection")
  {
    if (settingValue.GetString() == "trigger")
    {
      kodi::addon::SetSettingString("testConnection", "");
      TestConnection();
    }
    return;
  }

  // Ignore programmatic updates to prevent callback loops
  if (settingName == "isLoggedIn" || settingName == "jellyfinServerName" ||
      settingName == "jellyfinDisplayUsername" || settingName == "jellyfinAccessToken" ||
      settingName == "jellyfinUserId")
  {
    return;
  }

  // For regular settings, re-read all settings and flag for reload
  m_settings->ReadSettings();

  if (!m_reloadChannelsGroupsAndEPG)
    m_reloadChannelsGroupsAndEPG = true;
}
