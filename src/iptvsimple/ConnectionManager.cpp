/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "ConnectionManager.h"

#include "IConnectionListener.h"
#include "InstanceSettings.h"
#include "utilities/Logger.h"
#include "utilities/WebUtils.h"

#include <chrono>

#include <kodi/tools/StringUtils.h>
#include <kodi/Network.h>

using namespace iptvsimple;
using namespace iptvsimple::utilities;
using namespace kodi::tools;

/*
 * Iptvsimple Connection handler
 */

ConnectionManager::ConnectionManager(IConnectionListener& connectionListener, std::shared_ptr<iptvsimple::InstanceSettings> settings)
  : m_connectionListener(connectionListener), m_settings(settings), m_suspended(false), m_state(PVR_CONNECTION_STATE_UNKNOWN)
{
}

ConnectionManager::~ConnectionManager()
{
  Stop();
}

void ConnectionManager::Start()
{
  // Note: "connecting" must only be set one time, before the very first connection attempt, not on every reconnect.
  SetState(PVR_CONNECTION_STATE_CONNECTING);
  m_running = true;
  m_thread = std::thread([&] { Process(); });
}

void ConnectionManager::Stop()
{
  m_running = false;
  if (m_thread.joinable())
    m_thread.join();

  Disconnect();
}

void ConnectionManager::OnSleep()
{
  std::lock_guard<std::mutex> lock(m_mutex);

  Logger::Log(LogLevel::LEVEL_DEBUG, "%s going to sleep", __func__);

  m_suspended = true;
}

void ConnectionManager::OnWake()
{
  std::lock_guard<std::mutex> lock(m_mutex);

  Logger::Log(LogLevel::LEVEL_DEBUG, "%s Waking up", __func__);

  m_suspended = false;
}

void ConnectionManager::SetState(PVR_CONNECTION_STATE state)
{
  PVR_CONNECTION_STATE prevState(PVR_CONNECTION_STATE_UNKNOWN);
  PVR_CONNECTION_STATE newState(PVR_CONNECTION_STATE_UNKNOWN);

  {
    std::lock_guard<std::mutex> lock(m_mutex);

    /* No notification if no state change or while suspended. */
    if (m_state != state && !m_suspended)
    {
      prevState = m_state;
      newState = state;
      m_state = newState;

      Logger::Log(LogLevel::LEVEL_DEBUG, "connection state change (%d -> %d)", prevState, newState);
    }
  }

  if (prevState != newState)
  {
    if (newState == PVR_CONNECTION_STATE_SERVER_UNREACHABLE)
    {
      m_connectionListener.ConnectionLost();
    }
    else if (newState == PVR_CONNECTION_STATE_CONNECTED)
    {
      m_connectionListener.ConnectionEstablished();
    }

    /* Notify connection state change (callback!) */
      m_connectionListener.ConnectionStateChange(m_settings->GetJellyfinBaseUrl(), newState, "");
  }
}

void ConnectionManager::Disconnect()
{
  std::lock_guard<std::mutex> lock(m_mutex);

  m_connectionListener.ConnectionLost();
}

void ConnectionManager::Process()
{
  int fastReconnectIntervalMs = (m_settings->GetConnectioncCheckIntervalSecs() * 1000) / 2;
  int intervalMs = m_settings->GetConnectioncCheckIntervalSecs() * 1000;

  while (m_running)
  {
    // Also check m_running: without it a Stop() while suspended spins here
    // forever (SteppedSleep returns immediately when not running) and the
    // destructor's join never completes.
    while (m_suspended && m_running)
    {
      Logger::Log(LogLevel::LEVEL_DEBUG, "%s - suspended, waiting for wakeup...", __func__);

      /* Wait for wakeup */
      SteppedSleep(intervalMs);
    }

    const std::string url = m_settings->GetConnectionCheckUrl();
    int tcpTimeout = m_settings->GetConnectioncCheckTimeoutSecs();
    bool isLocalPath = false; // Jellyfin is always remote

    /* URL is set */
    if (m_settings->GetJellyfinServerAddress().empty())
    {
      /* wait for URL to be set */
      SteppedSleep(intervalMs);
      continue;
    }

    /* Connect. Ping on every iteration: the old (firstRun || !m_onStartupOnly)
     * gate with m_onStartupOnly hardcoded true stopped health-checking after
     * the first success, so outages were never detected and the addon never
     * reconnected/reloaded after a server restart. IptvSimple's
     * ConnectionEstablished is written to be re-entered on reconnects. */
    if (!WebUtils::Check(url, tcpTimeout, isLocalPath))
    {
      /* Unable to connect */
      if (m_retryAttempt == 0)
        Logger::Log(LogLevel::LEVEL_ERROR, "%s - unable to connect to: %s", __func__, url.c_str());
      SetState(PVR_CONNECTION_STATE_SERVER_UNREACHABLE);

      // Retry a few times with a short interval, after that with the default timeout
      if (++m_retryAttempt <= FAST_RECONNECT_ATTEMPTS)
        SteppedSleep(fastReconnectIntervalMs);
      else
        SteppedSleep(intervalMs);

      continue;
    }

    SetState(PVR_CONNECTION_STATE_CONNECTED);
    m_retryAttempt = 0;

    SteppedSleep(intervalMs);
  }
}

void ConnectionManager::SteppedSleep(int intervalMs)
{
  int sleepCountMs = 0;

  while (sleepCountMs <= intervalMs)
  {
    if (m_running)
      std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_INTERVAL_STEP_MS));

    sleepCountMs += SLEEP_INTERVAL_STEP_MS;
  }
}
