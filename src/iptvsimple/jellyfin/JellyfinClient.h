/*
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include "../InstanceSettings.h"

#include <json/json.h>
#include <memory>
#include <string>

namespace iptvsimple
{
namespace jellyfin
{

class JellyfinClient
{
public:
  JellyfinClient(std::shared_ptr<iptvsimple::InstanceSettings> settings);

  // Authentication
  bool Authenticate();
  bool AuthenticateByPassword(const std::string& username, const std::string& password);
  bool StartQuickConnect(std::string& code);
  bool CheckQuickConnect(std::string& userId, std::string& accessToken);
  bool ValidateToken();
  bool IsAuthenticated() const { return !m_accessToken.empty() && !m_userId.empty(); }

  // Server info
  bool FetchServerInfo(std::string& serverName);

  // HTTP methods
  Json::Value SendGet(const std::string& endpoint);
  Json::Value SendPost(const std::string& endpoint, const std::string& body);
  bool SendDelete(const std::string& endpoint);

  std::string BuildImageUrl(const std::string& itemId, const std::string& imageTag = "");
  std::string GetBaseUrl() const;
  const std::string& GetAccessToken() const { return m_accessToken; }
  const std::string& GetUserId() const { return m_userId; }

private:
  Json::Value DoRequest(const std::string& url, const std::string& postData = "");
  std::string BuildUrl(const std::string& endpoint) const;
  std::string BuildAuthHeader() const;
  static std::string Base64Encode(const std::string& input);

  void SetAuthFromResponse(const Json::Value& response);

  std::shared_ptr<iptvsimple::InstanceSettings> m_settings;
  std::string m_accessToken;
  std::string m_userId;
  std::string m_quickConnectSecret;
};

} // namespace jellyfin
} // namespace iptvsimple
