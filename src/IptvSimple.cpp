/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "IptvSimple.h"

#include "iptvsimple/InstanceSettings.h"
#include "iptvsimple/utilities/Logger.h"
#include "iptvsimple/utilities/TimeUtils.h"
#include "iptvsimple/utilities/WebUtils.h"

#include <ctime>
#include <chrono>

#include <kodi/tools/StringUtils.h>

using namespace iptvsimple;
using namespace iptvsimple::data;
using namespace iptvsimple::utilities;
using namespace kodi::tools;

IptvSimple::IptvSimple(const kodi::addon::IInstanceInfo& instance) : iptvsimple::IConnectionListener(instance), m_settings(new InstanceSettings(*this, instance))
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
  m_channels.Init();
  m_channelGroups.Init();
  m_providers.Init();

  // TODO: Phase 2 - JellyfinChannelLoader will load channels and EPG here

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
  capabilities.SetSupportsRecordings(false); // TODO: Phase 3 - enable when JellyfinRecordingManager is wired
  capabilities.SetSupportsRecordingsRename(false);
  capabilities.SetSupportsRecordingsLifetimeChange(false);
  capabilities.SetSupportsRecordingsDelete(false);
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

    // TODO: Phase 2 - reload from Jellyfin on interval
    // if (refreshTimer >= (m_settings->GetJellyfinUpdateIntervalMins() * 60))
    //   m_reloadChannelsGroupsAndEPG = true;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running && m_reloadChannelsGroupsAndEPG)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));

      m_settings->ReloadAddonInstanceSettings();
      // TODO: Phase 2 - reload channels/EPG from Jellyfin

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

PVR_ERROR IptvSimple::GetChannelStreamProperties(const kodi::addon::PVRChannel& channel, PVR_SOURCE source, std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  if (GetChannel(channel, m_currentChannel))
  {
    std::string streamURL = m_currentChannel.GetStreamURL();

    StreamUtils::SetAllStreamProperties(properties, m_currentChannel, streamURL, true, {}, m_settings);

    Logger::Log(LogLevel::LEVEL_INFO, "%s - Stream URL: %s", __FUNCTION__, WebUtils::RedactUrl(streamURL).c_str());

    return PVR_ERROR_NO_ERROR;
  }

  return PVR_ERROR_SERVER_ERROR;
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
  // TODO: Phase 2 - delegate to JellyfinChannelLoader
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::SetEPGMaxPastDays(int epgMaxPastDays)
{
  // TODO: Phase 2 - store for Jellyfin EPG queries
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::SetEPGMaxFutureDays(int epgMaxFutureDays)
{
  // TODO: Phase 2 - store for Jellyfin EPG queries
  return PVR_ERROR_NO_ERROR;
}

/***************************************************************************
 * Recordings (stub - Phase 3)
 **************************************************************************/

PVR_ERROR IptvSimple::GetRecordingsAmount(bool deleted, int& amount)
{
  amount = 0;
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::GetRecordings(bool deleted, kodi::addon::PVRRecordingsResultSet& results)
{
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR IptvSimple::GetRecordingStreamProperties(const kodi::addon::PVRRecording& recording, std::vector<kodi::addon::PVRStreamProperty>& properties)
{
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

PVR_ERROR IptvSimple::StreamClosed()
{
  Logger::Log(LEVEL_INFO, "%s - Stream Closed", __FUNCTION__);

  return PVR_ERROR_NO_ERROR;
}

/***************************************************************************
 * InstanceSettings
 **************************************************************************/

ADDON_STATUS IptvSimple::SetInstanceSetting(const std::string& settingName, const kodi::addon::CSettingValue& settingValue)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  if (!m_reloadChannelsGroupsAndEPG)
    m_reloadChannelsGroupsAndEPG = true;

  return m_settings->SetSetting(settingName, settingValue);
}
