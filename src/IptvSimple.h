/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include "iptvsimple/CatchupController.h"
#include "iptvsimple/Channels.h"
#include "iptvsimple/ChannelGroups.h"
#include "iptvsimple/ConnectionManager.h"
#include "iptvsimple/Providers.h"
#include "iptvsimple/IConnectionListener.h"
#include "iptvsimple/data/Channel.h"
#include "iptvsimple/jellyfin/JellyfinClient.h"
#include "iptvsimple/jellyfin/JellyfinChannelLoader.h"
#include "iptvsimple/jellyfin/JellyfinRecordingManager.h"

#include <atomic>
#include <mutex>
#include <thread>

#include <kodi/addon-instance/PVR.h>
#include <kodi/Filesystem.h>

class ATTR_DLL_LOCAL IptvSimple : public iptvsimple::IConnectionListener
{
public:
  IptvSimple(const kodi::addon::IInstanceInfo& instance);
  ~IptvSimple() override;

  // IConnectionListener implementation
  void ConnectionLost() override;
  void ConnectionEstablished() override;

  bool Initialise();

  // kodi::addon::CInstancePVRClient functions
  //@{
  PVR_ERROR GetCapabilities(kodi::addon::PVRCapabilities& capabilities) override;
  PVR_ERROR GetBackendName(std::string& name) override;
  PVR_ERROR GetBackendVersion(std::string& version) override;
  PVR_ERROR GetConnectionString(std::string& connection) override;

  PVR_ERROR OnSystemSleep() override;
  PVR_ERROR OnSystemWake() override;
  PVR_ERROR OnPowerSavingActivated() override { return PVR_ERROR_NO_ERROR; }
  PVR_ERROR OnPowerSavingDeactivated() override { return PVR_ERROR_NO_ERROR; }

  PVR_ERROR GetProvidersAmount(int& amount) override;
  PVR_ERROR GetProviders(kodi::addon::PVRProvidersResultSet& results) override;

  PVR_ERROR GetChannelsAmount(int& amount) override;
  PVR_ERROR GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results) override;
  // Kodi v22 (PVR API 9.x) added PVR_SOURCE parameter
#ifdef KODI_PVR_API_V9
  PVR_ERROR GetChannelStreamProperties(const kodi::addon::PVRChannel& channel, PVR_SOURCE source, std::vector<kodi::addon::PVRStreamProperty>& properties) override;
#else
  PVR_ERROR GetChannelStreamProperties(const kodi::addon::PVRChannel& channel, std::vector<kodi::addon::PVRStreamProperty>& properties) override;
#endif
  bool OpenLiveStream(const kodi::addon::PVRChannel& channel) override;
  void CloseLiveStream() override;

  PVR_ERROR GetChannelGroupsAmount(int& amount) override;
  PVR_ERROR GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results) override;
  PVR_ERROR GetChannelGroupMembers(const kodi::addon::PVRChannelGroup& group, kodi::addon::PVRChannelGroupMembersResultSet& results) override;

  PVR_ERROR GetEPGForChannel(int channelUid, time_t start, time_t end, kodi::addon::PVREPGTagsResultSet& results) override;
  PVR_ERROR SetEPGMaxPastDays(int epgMaxPastDays) override;
  PVR_ERROR SetEPGMaxFutureDays(int epgMaxFutureDays) override;
  PVR_ERROR IsEPGTagPlayable(const kodi::addon::PVREPGTag& tag, bool& isPlayable) override;
  PVR_ERROR GetEPGTagStreamProperties(const kodi::addon::PVREPGTag& tag, std::vector<kodi::addon::PVRStreamProperty>& properties) override;

  PVR_ERROR GetSignalStatus(int channelUid, kodi::addon::PVRSignalStatus& signalStatus) override;
  PVR_ERROR CallSettingsMenuHook(const kodi::addon::PVRMenuhook& menuhook) override;

  PVR_ERROR GetTimerTypes(std::vector<kodi::addon::PVRTimerType>& types) override;
  PVR_ERROR GetTimersAmount(int& amount) override;
  PVR_ERROR GetTimers(kodi::addon::PVRTimersResultSet& results) override;
  PVR_ERROR AddTimer(const kodi::addon::PVRTimer& timer) override;
  PVR_ERROR DeleteTimer(const kodi::addon::PVRTimer& timer, bool forceDelete) override;
  PVR_ERROR UpdateTimer(const kodi::addon::PVRTimer& timer) override;

  PVR_ERROR GetRecordingsAmount(bool deleted, int& amount) override;
  PVR_ERROR GetRecordings(bool deleted, kodi::addon::PVRRecordingsResultSet& results) override;
  PVR_ERROR DeleteRecording(const kodi::addon::PVRRecording& recording) override;
  PVR_ERROR SetRecordingPlayCount(const kodi::addon::PVRRecording& recording, int count) override;
  PVR_ERROR SetRecordingLastPlayedPosition(const kodi::addon::PVRRecording& recording, int lastplayedposition) override;
  PVR_ERROR GetRecordingLastPlayedPosition(const kodi::addon::PVRRecording& recording, int& position) override;
  PVR_ERROR GetRecordingStreamProperties(const kodi::addon::PVRRecording& recording, std::vector<kodi::addon::PVRStreamProperty>& properties) override;

#ifdef KODI_PVR_API_V9
  bool OpenRecordedStream(const kodi::addon::PVRRecording& recording, int64_t& streamId) override;
  void CloseRecordedStream(int64_t streamId) override;
  int ReadRecordedStream(int64_t streamId, unsigned char* buffer, unsigned int size) override;
  int64_t SeekRecordedStream(int64_t streamId, int64_t position, int whence) override;
  int64_t LengthRecordedStream(int64_t streamId) override;
#else
  bool OpenRecordedStream(const kodi::addon::PVRRecording& recording) override;
  void CloseRecordedStream() override;
  int ReadRecordedStream(unsigned char* buffer, unsigned int size) override;
  int64_t SeekRecordedStream(int64_t position, int whence) override;
  int64_t LengthRecordedStream() override;
#endif

  //@}

  // Internal functions
  //@{
  bool GetChannel(const kodi::addon::PVRChannel& channel, iptvsimple::data::Channel& myChannel);
  bool GetChannel(unsigned int uniqueChannelId, iptvsimple::data::Channel& myChannel);
  void OnSettingChanged(const std::string& settingName, const kodi::addon::CSettingValue& settingValue);
  bool NeedsRestart() const { return m_needsRestart; }
  //@}

protected:
  void Process();
  void TestConnection();
  void RunLogin();
  bool LoginWithPassword();
  bool LoginWithQuickConnect();
  void RunLogout();
  void FetchAndStoreServerName();

private:
  static const int PROCESS_LOOP_WAIT_SECS = 2;

  std::shared_ptr<iptvsimple::InstanceSettings> m_settings;

  iptvsimple::data::Channel m_currentChannel{m_settings};
  iptvsimple::Providers m_providers{m_settings};
  iptvsimple::Channels m_channels{m_settings};
  iptvsimple::ChannelGroups m_channelGroups{m_channels, m_settings};
  iptvsimple::ConnectionManager* connectionManager;
  std::shared_ptr<iptvsimple::jellyfin::JellyfinClient> m_jellyfinClient;
  std::shared_ptr<iptvsimple::jellyfin::JellyfinChannelLoader> m_channelLoader;
  std::shared_ptr<iptvsimple::jellyfin::JellyfinRecordingManager> m_recordingManager;

  std::atomic<bool> m_running{false};
  std::thread m_thread;
  std::mutex m_mutex;
  std::atomic_bool m_reloadChannelsGroupsAndEPG{false};
  std::atomic_bool m_needsRestart{false};
  std::unique_ptr<iptvsimple::CatchupController> m_catchupController;

  // Recording byte-stream (used by Recordings section playback path)
  bool OpenRecordedStreamImpl(const kodi::addon::PVRRecording& recording);
  void CloseRecordedStreamImpl();
  int ReadRecordedStreamImpl(unsigned char* buffer, unsigned int size);
  int64_t SeekRecordedStreamImpl(int64_t position, int whence);
  int64_t LengthRecordedStreamImpl();

  kodi::vfs::CFile m_recordingStream;
  bool m_recordingStreamOpen{false};
};
