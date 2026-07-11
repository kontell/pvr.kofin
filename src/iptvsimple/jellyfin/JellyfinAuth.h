/*
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include "../InstanceSettings.h"
#include "JellyfinClient.h"

#include <memory>

namespace iptvsimple
{
namespace jellyfin
{

// Account/session orchestration for the Jellyfin connection: connection test,
// login (password or Quick Connect), logout, and server-name fetch. The
// low-level protocol lives in JellyfinClient; this class drives the Kodi UI
// dialogs, the Quick Connect polling loop, and settings persistence.
class JellyfinAuth
{
public:
  JellyfinAuth(std::shared_ptr<iptvsimple::InstanceSettings> settings);

  // Optional live client, reused by FetchAndStoreServerName. The login flows
  // always create their own fresh clients, so this is only an optimization.
  void SetClient(std::shared_ptr<JellyfinClient> client) { m_client = client; }

  void TestConnection();
  void RunLogin();
  void RunLogout();
  void FetchAndStoreServerName();

private:
  // *Internal carry the logic; the public entry points wrap them in exception
  // firewalls. These flows parse server-controlled JSON and are reached from
  // Kodi's SetSetting callback and the connection-manager thread, where an
  // escaped jsoncpp type error crosses the C ABI or std::terminates.
  void TestConnectionInternal();
  void RunLoginInternal();
  void FetchAndStoreServerNameInternal();

  bool LoginWithPassword();
  bool LoginWithQuickConnect();

  std::shared_ptr<iptvsimple::InstanceSettings> m_settings;
  std::shared_ptr<JellyfinClient> m_client;
};

} // namespace jellyfin
} // namespace iptvsimple
