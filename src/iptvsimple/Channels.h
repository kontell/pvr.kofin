/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include "InstanceSettings.h"
#include "data/Channel.h"

#include <memory>
#include <string>
#include <vector>

#include <kodi/addon-instance/pvr/Channels.h>

namespace iptvsimple
{
  class ChannelGroups;

  namespace data
  {
    class ChannelGroup;
  }

  class Channels
  {
  public:
    Channels(std::shared_ptr<iptvsimple::InstanceSettings>& settings);

    bool Init();

    int GetChannelsAmount() const;
    PVR_ERROR GetChannels(kodi::addon::PVRChannelsResultSet& results, bool radio) const;
    bool GetChannel(const kodi::addon::PVRChannel& channel, iptvsimple::data::Channel& myChannel) const;
    bool GetChannel(int uniqueId, iptvsimple::data::Channel& myChannel) const;

    bool AddChannel(iptvsimple::data::Channel& channel, std::vector<int>& groupIdList, iptvsimple::ChannelGroups& channelGroups, bool channelHadGroups);
    const std::vector<data::Channel>& GetChannelsList() const { return m_channels; }
    void Clear();

    // Adopt the data of a freshly loaded instance (built off-lock) without
    // changing this object's identity — ChannelGroups and the recording
    // manager hold references/pointers to the member instance. Call with the
    // model mutex held.
    void MoveFrom(Channels&& other)
    {
      m_channels = std::move(other.m_channels);
      m_channelsLoadFailed = other.m_channelsLoadFailed;
    }

    void ChannelsLoadFailed() { m_channelsLoadFailed = true; };

  private:
    int GenerateChannelId(const char* channelName, const char* streamUrl);

    bool m_channelsLoadFailed = false;

    std::vector<iptvsimple::data::Channel> m_channels;

    std::shared_ptr<iptvsimple::InstanceSettings> m_settings;
  };
} //namespace iptvsimple
