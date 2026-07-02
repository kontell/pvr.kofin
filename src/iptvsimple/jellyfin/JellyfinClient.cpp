/*
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "JellyfinClient.h"

#include "../utilities/JsonUtils.h"
#include "../utilities/Logger.h"
#include "../utilities/WebUtils.h"

#include <sstream>

#include <kodi/Filesystem.h>

using namespace iptvsimple;
using namespace iptvsimple::jellyfin;
using namespace iptvsimple::utilities;

JellyfinClient::JellyfinClient(std::shared_ptr<iptvsimple::InstanceSettings> settings)
  : m_settings(settings)
{
  // Restore persisted credentials
  m_accessToken = m_settings->GetJellyfinAccessToken();
  m_userId = m_settings->GetJellyfinUserId();
}

/***************************************************************************
 * Authentication
 **************************************************************************/

std::string JellyfinClient::BuildAuthHeader() const
{
  std::ostringstream header;
  header << "MediaBrowser Client=\"Kofin PVR\", Device=\"Kodi\""
         << ", DeviceId=\"" << m_settings->GetDeviceId() << "\""
         << ", Version=\"" << STR(KOFIN_VERSION) << "\"";
  if (!m_accessToken.empty())
    header << ", Token=\"" << m_accessToken << "\"";
  return header.str();
}

bool JellyfinClient::Authenticate()
{
  // Token-only validation — no username/password fallback.
  // Login is handled interactively via the Account settings UI.
  if (!m_accessToken.empty() && !m_userId.empty())
  {
    if (ValidateToken())
    {
      Logger::Log(LEVEL_INFO, "%s - Existing token is valid for user %s",
                  __FUNCTION__, m_userId.c_str());
      return true;
    }
    Logger::Log(LEVEL_WARNING, "%s - Stored token invalid, clearing credentials", __FUNCTION__);
    m_accessToken.clear();
    m_userId.clear();
    m_settings->SetJellyfinAccessToken("");
    m_settings->SetJellyfinUserId("");
    m_settings->SetIsLoggedIn(false);
  }

  return false;
}

bool JellyfinClient::AuthenticateByPassword(const std::string& username, const std::string& password)
{
  Logger::Log(LEVEL_INFO, "%s - Authenticating user '%s'", __FUNCTION__, username.c_str());

  // Build the body with jsoncpp so quotes/backslashes in credentials are
  // escaped properly (string concatenation broke logins for passwords
  // containing '"' or '\'). Field order does not matter to the server.
  Json::Value authBody;
  authBody["Username"] = username;
  authBody["Pw"] = password;

  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  const std::string body = Json::writeString(writer, authBody);

  Json::Value response = DoRequest(BuildUrl("/Users/AuthenticateByName"), body);

  if (response.isNull() || !response.isMember("AccessToken"))
  {
    Logger::Log(LEVEL_ERROR, "%s - Authentication failed for user '%s'", __FUNCTION__, username.c_str());
    return false;
  }

  SetAuthFromResponse(response);
  return true;
}

bool JellyfinClient::StartQuickConnect(std::string& code)
{
  Logger::Log(LEVEL_INFO, "%s - Initiating Quick Connect", __FUNCTION__);

  Json::Value response = DoRequest(BuildUrl("/QuickConnect/Initiate"));

  if (response.isNull() || !response.isMember("Code") || !response.isMember("Secret"))
  {
    Logger::Log(LEVEL_ERROR, "%s - Quick Connect initiation failed", __FUNCTION__);
    return false;
  }

  code = response["Code"].asString();
  m_quickConnectSecret = response["Secret"].asString();

  Logger::Log(LEVEL_INFO, "%s - Quick Connect code: %s", __FUNCTION__, code.c_str());
  return true;
}

bool JellyfinClient::CheckQuickConnect(std::string& userId, std::string& accessToken)
{
  if (m_quickConnectSecret.empty())
    return false;

  // Step 1: Check if user has authorized the Quick Connect code
  const std::string url = BuildUrl("/QuickConnect/Connect?secret=" + m_quickConnectSecret);
  Json::Value response = DoRequest(url);

  if (response.isNull())
    return false;

  if (!response.get("Authenticated", false).asBool())
    return false;

  Logger::Log(LEVEL_INFO, "%s - Quick Connect authorized, exchanging for access token", __FUNCTION__);

  // Step 2: Exchange the secret for an access token via AuthenticateWithQuickConnect
  Json::Value authBody;
  authBody["Secret"] = m_quickConnectSecret;

  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  const std::string bodyStr = Json::writeString(writer, authBody);

  Json::Value authResponse = SendPost("/Users/AuthenticateWithQuickConnect", bodyStr);
  if (!authResponse.isNull())
  {
    SetAuthFromResponse(authResponse);
    if (!m_accessToken.empty() && !m_userId.empty())
    {
      userId = m_userId;
      accessToken = m_accessToken;
      m_quickConnectSecret.clear();

      Logger::Log(LEVEL_INFO, "%s - Quick Connect authenticated, userId: %s",
                  __FUNCTION__, m_userId.c_str());
      return true;
    }
  }

  Logger::Log(LEVEL_ERROR, "%s - Quick Connect authorized but token exchange failed", __FUNCTION__);
  m_quickConnectSecret.clear();
  return false;
}

bool JellyfinClient::ValidateToken()
{
  if (m_accessToken.empty() || m_userId.empty())
    return false;

  Json::Value response = DoRequest(BuildUrl("/Users/" + m_userId));
  return !response.isNull() && response.isMember("Id");
}

void JellyfinClient::Logout()
{
  if (m_accessToken.empty())
    return;

  // POST /Sessions/Logout revokes the access token used to authenticate the
  // call — the server deletes it and drops the device session. Best-effort:
  // success returns 204 with no body, and failures (server unreachable) are
  // non-fatal because the caller clears local credentials regardless.
  SendPost("/Sessions/Logout", "{}");
  Logger::Log(LEVEL_INFO, "%s - Requested server-side token revocation", __FUNCTION__);
}

void JellyfinClient::SetAuthFromResponse(const Json::Value& response)
{
  m_accessToken = response["AccessToken"].asString();

  if (response.isMember("User") && response["User"].isMember("Id"))
    m_userId = response["User"]["Id"].asString();

  // Extract display username
  if (response.isMember("User") && response["User"].isMember("Name"))
    m_settings->SetJellyfinDisplayUsername(response["User"]["Name"].asString());

  // Persist to settings
  m_settings->SetJellyfinAccessToken(m_accessToken);
  m_settings->SetJellyfinUserId(m_userId);

  Logger::Log(LEVEL_INFO, "%s - Authenticated, userId: %s", __FUNCTION__, m_userId.c_str());
}

bool JellyfinClient::FetchServerInfo(std::string& serverName)
{
  Json::Value response = DoRequest(BuildUrl("/System/Info/Public"));
  if (response.isNull() || !response.isMember("ServerName"))
    return false;
  serverName = response["ServerName"].asString();
  return true;
}

bool JellyfinClient::GetStorageInfo(uint64_t& totalBytes, uint64_t& usedBytes)
{
  // Exception firewall: this runs inside Kodi's GetDriveSpace callback, and
  // a jsoncpp type error must not cross the C ABI.
  try
  {
    return GetStorageInfoInternal(totalBytes, usedBytes);
  }
  catch (const std::exception& e)
  {
    Logger::Log(LEVEL_ERROR, "%s - Exception parsing storage info: %s", __FUNCTION__, e.what());
    return false;
  }
}

bool JellyfinClient::GetStorageInfoInternal(uint64_t& totalBytes, uint64_t& usedBytes)
{
  Json::Value response = SendGet("/System/Info/Storage");
  if (response.isNull() || !response.isMember("Libraries"))
    return false;

  totalBytes = 0;
  usedBytes = 0;

  for (const auto& lib : response["Libraries"])
  {
    if (lib.get("Name", "").asString() != "Recordings")
      continue;
    if (!lib.isMember("Folders") || lib["Folders"].empty())
      break;
    const Json::Value& folder = lib["Folders"][0];
    // SafeUInt64: the server reports these as signed longs and can send
    // negative values when the drive query fails.
    const uint64_t free = SafeUInt64(folder["FreeSpace"]);
    const uint64_t used = SafeUInt64(folder["UsedSpace"]);
    totalBytes = free + used;
    usedBytes = used;
    return true;
  }

  return false;
}

/***************************************************************************
 * HTTP Methods
 **************************************************************************/

std::string JellyfinClient::GetBaseUrl() const
{
  return m_settings->GetJellyfinBaseUrl();
}

std::string JellyfinClient::BuildUrl(const std::string& endpoint) const
{
  return GetBaseUrl() + endpoint;
}

std::string JellyfinClient::BuildImageUrl(const std::string& itemId, const std::string& imageTag)
{
  std::string url = GetBaseUrl() + "/Items/" + itemId + "/Images/Primary";
  if (!imageTag.empty())
    url += "?tag=" + imageTag;
  return url;
}

Json::Value JellyfinClient::SendGet(const std::string& endpoint)
{
  return DoRequest(BuildUrl(endpoint));
}

Json::Value JellyfinClient::SendPost(const std::string& endpoint, const std::string& body)
{
  return DoRequest(BuildUrl(endpoint), body);
}

bool JellyfinClient::SendPostExpectSuccess(const std::string& endpoint, const std::string& body)
{
  bool requestOk = false;
  DoRequest(BuildUrl(endpoint), body, &requestOk);
  return requestOk;
}

bool JellyfinClient::SendDelete(const std::string& endpoint)
{
  std::string url = BuildUrl(endpoint);

  kodi::vfs::CFile file;
  if (!file.CURLCreate(url))
  {
    Logger::Log(LEVEL_ERROR, "%s - CURLCreate failed for: %s", __FUNCTION__, WebUtils::RedactUrl(url).c_str());
    return false;
  }

  file.CURLAddOption(ADDON_CURL_OPTION_PROTOCOL, "customrequest", "DELETE");
  file.CURLAddOption(ADDON_CURL_OPTION_HEADER, "Authorization", BuildAuthHeader());
  file.CURLAddOption(ADDON_CURL_OPTION_HEADER, "Content-Type", "application/json");
  file.CURLAddOption(ADDON_CURL_OPTION_HEADER, "Connection", "close");
  file.CURLAddOption(ADDON_CURL_OPTION_PROTOCOL, "connection-timeout", "10");
  file.CURLAddOption(ADDON_CURL_OPTION_PROTOCOL, "acceptencoding", "gzip, deflate");

  unsigned int flags = ADDON_READ_NO_CACHE;
  if (!file.CURLOpen(flags))
  {
    Logger::Log(LEVEL_ERROR, "%s - DELETE failed for: %s", __FUNCTION__, WebUtils::RedactUrl(url).c_str());
    file.Close();
    return false;
  }

  file.Close();
  return true;
}

Json::Value JellyfinClient::DoRequest(const std::string& url, const std::string& postData,
                                      bool* requestOk)
{
  Json::Value result;
  if (requestOk)
    *requestOk = false;

  kodi::vfs::CFile file;
  if (!file.CURLCreate(url))
  {
    Logger::Log(LEVEL_ERROR, "%s - CURLCreate failed for: %s", __FUNCTION__, WebUtils::RedactUrl(url).c_str());
    return result;
  }

  file.CURLAddOption(ADDON_CURL_OPTION_HEADER, "Authorization", BuildAuthHeader());
  file.CURLAddOption(ADDON_CURL_OPTION_HEADER, "Content-Type", "application/json");
  // Don't let Kodi's curl pool cache this connection. A long-running playback
  // produced dozens of idle keep-alive connections that Kodi's stop flow had
  // to iterate at teardown, producing proportional UI hangs. Each POST opens
  // a fresh TCP/TLS — cheap relative to the pool-teardown cost.
  file.CURLAddOption(ADDON_CURL_OPTION_HEADER, "Connection", "close");
  file.CURLAddOption(ADDON_CURL_OPTION_PROTOCOL, "connection-timeout", "10");
  file.CURLAddOption(ADDON_CURL_OPTION_PROTOCOL, "acceptencoding", "gzip, deflate");

  if (!postData.empty())
  {
    std::string encoded = Base64Encode(postData);
    file.CURLAddOption(ADDON_CURL_OPTION_PROTOCOL, "postdata", encoded);
  }

  unsigned int flags = ADDON_READ_NO_CACHE;
  if (!file.CURLOpen(flags))
  {
    Logger::Log(LEVEL_ERROR, "%s - CURLOpen failed for: %s", __FUNCTION__, WebUtils::RedactUrl(url).c_str());
    file.Close();
    return result;
  }

  // Kodi's curl layer fails CURLOpen on HTTP >= 400, so the request reached
  // the server and returned 2xx/3xx — success even if the body is empty (204).
  if (requestOk)
    *requestOk = true;

  // Read response
  std::string response;
  char buffer[4096];
  ssize_t bytesRead;
  while ((bytesRead = file.Read(buffer, sizeof(buffer))) > 0)
    response.append(buffer, static_cast<size_t>(bytesRead));

  file.Close();

  if (response.empty())
    return result;

  // Parse JSON
  Json::CharReaderBuilder builder;
  std::string errors;
  std::istringstream stream(response);
  if (!Json::parseFromStream(builder, stream, &result, &errors))
  {
    Logger::Log(LEVEL_ERROR, "%s - JSON parse error: %s (url: %s)", __FUNCTION__, errors.c_str(), WebUtils::RedactUrl(url).c_str());
    return Json::Value();
  }

  return result;
}

std::string JellyfinClient::Base64Encode(const std::string& input)
{
  static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  encoded.reserve(((input.size() / 3) + 1) * 4);

  int i = 0;
  unsigned char a3[3];
  unsigned char a4[4];

  size_t inLen = input.size();
  const char* ptr = input.c_str();

  while (inLen--)
  {
    a3[i++] = static_cast<unsigned char>(*(ptr++));
    if (i == 3)
    {
      a4[0] = (a3[0] & 0xfc) >> 2;
      a4[1] = ((a3[0] & 0x03) << 4) + ((a3[1] & 0xf0) >> 4);
      a4[2] = ((a3[1] & 0x0f) << 2) + ((a3[2] & 0xc0) >> 6);
      a4[3] = a3[2] & 0x3f;
      for (i = 0; i < 4; i++)
        encoded += chars[a4[i]];
      i = 0;
    }
  }

  if (i)
  {
    for (int j = i; j < 3; j++)
      a3[j] = 0;

    a4[0] = (a3[0] & 0xfc) >> 2;
    a4[1] = ((a3[0] & 0x03) << 4) + ((a3[1] & 0xf0) >> 4);
    a4[2] = ((a3[1] & 0x0f) << 2) + ((a3[2] & 0xc0) >> 6);

    for (int j = 0; j < i + 1; j++)
      encoded += chars[a4[j]];

    while (i++ < 3)
      encoded += '=';
  }

  return encoded;
}
