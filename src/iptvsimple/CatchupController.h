/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include "InstanceSettings.h"
#include "data/Channel.h"
#include "utilities/StreamUtils.h"

#include <map>
#include <memory>
#include <string>

#include <kodi/addon-instance/pvr/EPG.h>

namespace iptvsimple
{

class CatchupController
{
public:
  CatchupController(std::shared_ptr<iptvsimple::InstanceSettings>& settings);

  void ProcessChannelForPlayback(const data::Channel& channel, std::map<std::string, std::string>& catchupProperties);
  void ProcessEPGTagForTimeshiftedPlayback(const kodi::addon::PVREPGTag& epgTag, const data::Channel& channel, std::map<std::string, std::string>& catchupProperties);
  void ProcessEPGTagForVideoPlayback(const kodi::addon::PVREPGTag& epgTag, const data::Channel& channel, std::map<std::string, std::string>& catchupProperties);

  std::string GetCatchupUrlFormatString(const data::Channel& channel) const;
  std::string GetCatchupUrl(const data::Channel& channel) const;
  std::string ProcessStreamUrl(const data::Channel& channel) const;

  bool ControlsLiveStream() const { return m_controlsLiveStream; }
  void ResetCatchupState()
  {
    // Don't reset if we just processed a timeshifted EPG tag — the state
    // needs to carry over to ProcessChannelForPlayback.
    if (!m_fromTimeshiftedEpgTagCall)
      m_resetCatchupState = true;
  }

private:
  void SetCatchupInputStreamProperties(bool playbackAsLive, const data::Channel& channel, std::map<std::string, std::string>& catchupProperties);

  void UpdateProgrammeFrom(const kodi::addon::PVREPGTag& epgTag, int tvgShift);
  void ClearProgramme();

  int GetTimezoneShift(const data::Channel& channel) const;

  // State of current stream
  time_t m_catchupStartTime = 0;
  time_t m_catchupEndTime = 0;
  time_t m_timeshiftBufferStartTime = 0;
  long long m_timeshiftBufferOffset = 0;
  bool m_resetCatchupState = true;
  bool m_playbackIsVideo = false;
  bool m_fromTimeshiftedEpgTagCall = false;

  // Current programme details
  time_t m_programmeStartTime = 0;
  time_t m_programmeEndTime = 0;
  std::string m_programmeTitle;
  unsigned int m_programmeUniqueChannelId = 0;
  int m_programmeChannelTvgShift = 0;
  std::string m_programmeCatchupId;

  bool m_controlsLiveStream = false;

  std::shared_ptr<iptvsimple::InstanceSettings> m_settings;
};

} // namespace iptvsimple
