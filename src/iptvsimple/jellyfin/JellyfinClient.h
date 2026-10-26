/*
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include "../InstanceSettings.h"

#include <json/json.h>
#include <kodi/Filesystem.h>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

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
  // Public for callers that stream via their own CFile (recorded-stream byte
  // path) so the token can travel as a header instead of a URL query param.
  std::string BuildAuthHeader() const;
  // Apply the sslVerify setting to a handle opened outside this class, for the
  // same reason. Call after CURLCreate and before CURLOpen.
  void ApplyTlsOptions(kodi::vfs::CFile& file) const;
  bool AuthenticateByPassword(const std::string& username, const std::string& password);
  // Whether the server offers Quick Connect at all. Fails *open*: a
  // transport failure and a refusal are indistinguishable through Kodi's
  // curl layer, so anything short of a definite "false" answers true and
  // the caller still offers the choice.
  bool IsQuickConnectEnabled();
  bool StartQuickConnect(std::string& code);
  bool CheckQuickConnect(std::string& userId, std::string& accessToken);
  bool ValidateToken();
  void Logout();
  bool IsAuthenticated() const { return !m_accessToken.empty() && !m_userId.empty(); }

  // Server info
  bool FetchServerInfo(std::string& serverName);
  bool GetStorageInfo(uint64_t& totalBytes, uint64_t& usedBytes);

  // HTTP methods
  Json::Value SendGet(const std::string& endpoint);
  Json::Value SendPost(const std::string& endpoint, const std::string& body);
  // Like SendPost, but reports whether the request itself succeeded. Needed
  // for endpoints that return 204 No Content: an empty response body and a
  // transport/HTTP failure both yield a null Json::Value from SendPost.
  bool SendPostExpectSuccess(const std::string& endpoint, const std::string& body);
  bool SendDelete(const std::string& endpoint);

  std::string BuildImageUrl(const std::string& itemId, const std::string& imageTag = "");
  std::string GetBaseUrl() const;
  const std::string& GetAccessToken() const { return m_accessToken; }
  const std::string& GetUserId() const { return m_userId; }

private:
  bool AuthenticateInternal();
  bool GetStorageInfoInternal(uint64_t& totalBytes, uint64_t& usedBytes);
  // requestOk (optional) reports transport/HTTP success: Kodi's curl layer
  // fails CURLOpen on HTTP >= 400, so reaching a readable response means 2xx/3xx.
  Json::Value DoRequest(const std::string& url, const std::string& postData = "",
                        bool* requestOk = nullptr);
  std::string BuildUrl(const std::string& endpoint) const;
  static std::string Base64Encode(const std::string& input);

  void SetAuthFromResponse(const Json::Value& response);

  std::shared_ptr<iptvsimple::InstanceSettings> m_settings;
  std::string m_accessToken;
  std::string m_userId;
  std::string m_quickConnectSecret;
};

// The client a component sends its requests through. Every reconnect replaces
// it from the connection-manager thread while Kodi's callback threads may be
// mid-request on the old one. Get() returns a reference that keeps the client
// it read alive for as long as the caller holds it, so take it once at the
// start of an operation and use it throughout: the operation then neither
// straddles a reconnect nor dereferences a client replaced under it.
class SharedClient
{
public:
  SharedClient() = default;
  explicit SharedClient(std::shared_ptr<JellyfinClient> client) : m_client(std::move(client)) {}

  std::shared_ptr<JellyfinClient> Get() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_client;
  }

  void Set(std::shared_ptr<JellyfinClient> client)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_client = std::move(client);
  }

private:
  mutable std::mutex m_mutex;
  std::shared_ptr<JellyfinClient> m_client;
};

} // namespace jellyfin
} // namespace iptvsimple
