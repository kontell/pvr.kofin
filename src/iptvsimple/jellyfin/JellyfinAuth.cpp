/*
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "JellyfinAuth.h"

#include "JellyfinClient.h"
#include "../utilities/Logger.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <json/json.h>
#include <kodi/General.h>
#include <kodi/gui/dialogs/Keyboard.h>
#include <kodi/gui/dialogs/Progress.h>
#include <kodi/gui/dialogs/Select.h>

using namespace iptvsimple;
using namespace iptvsimple::jellyfin;
using namespace iptvsimple::utilities;

JellyfinAuth::JellyfinAuth(std::shared_ptr<iptvsimple::InstanceSettings> settings)
  : m_settings(settings)
{
}

void JellyfinAuth::TestConnection()
{
  // Exception firewall: reached from Kodi's SetSetting callback. The flow
  // parses server-controlled JSON, and "user typed a wrong address that
  // points at something hostile" is exactly this flow's threat model — a
  // jsoncpp type error must not unwind across the C ABI.
  try
  {
    TestConnectionInternal();
  }
  catch (const std::exception& e)
  {
    Logger::Log(LEVEL_ERROR, "%s - Exception during connection test: %s", __FUNCTION__, e.what());
    kodi::QueueNotification(QUEUE_ERROR, "Kofin PVR", "Connection failed.");
  }
}

void JellyfinAuth::TestConnectionInternal()
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

void JellyfinAuth::RunLogin()
{
  // Exception firewall — see TestConnection. Covers the password and Quick
  // Connect flows, whose response parsing (SetAuthFromResponse,
  // StartQuickConnect, CheckQuickConnect) converts server-controlled values.
  try
  {
    RunLoginInternal();
  }
  catch (const std::exception& e)
  {
    Logger::Log(LEVEL_ERROR, "%s - Exception during login: %s", __FUNCTION__, e.what());
    kodi::QueueNotification(QUEUE_ERROR, "Kofin PVR", kodi::addon::GetLocalizedString(30713));
  }
}

void JellyfinAuth::RunLoginInternal()
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

bool JellyfinAuth::LoginWithPassword()
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

bool JellyfinAuth::LoginWithQuickConnect()
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

void JellyfinAuth::RunLogout()
{
  // Revoke the token server-side before clearing local state. Jellyfin
  // tokens never expire on their own, so without this the credential would
  // stay valid on the server (and the device would linger in its dashboard)
  // indefinitely after "logging out".
  if (!m_settings->GetJellyfinAccessToken().empty())
  {
    // Firewall only the server call: whatever the revocation attempt does,
    // the local credential clearing below must always run.
    try
    {
      auto client = m_client;
      if (!client)
        client = std::make_shared<iptvsimple::jellyfin::JellyfinClient>(m_settings);
      client->Logout();
    }
    catch (const std::exception& e)
    {
      Logger::Log(LEVEL_ERROR, "%s - Exception during server-side logout: %s", __FUNCTION__, e.what());
    }
  }

  m_settings->SetJellyfinAccessToken("");
  m_settings->SetJellyfinUserId("");
  m_settings->SetJellyfinServerName("");
  m_settings->SetJellyfinDisplayUsername("");
  m_settings->SetIsLoggedIn(false);

  kodi::QueueNotification(QUEUE_INFO, "Kofin PVR", kodi::addon::GetLocalizedString(30712));
  Logger::Log(LEVEL_INFO, "%s - User logged out", __FUNCTION__);
}

void JellyfinAuth::FetchAndStoreServerName()
{
  // Exception firewall — also called directly from the connection-manager
  // thread, where an escaped exception means std::terminate.
  try
  {
    FetchAndStoreServerNameInternal();
  }
  catch (const std::exception& e)
  {
    Logger::Log(LEVEL_ERROR, "%s - Exception fetching server info: %s", __FUNCTION__, e.what());
  }
}

void JellyfinAuth::FetchAndStoreServerNameInternal()
{
  auto client = m_client;
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
