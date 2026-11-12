/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "WebUtils.h"

#include "FileUtils.h"
#include "Logger.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <regex>
#include <sstream>

#include <kodi/Filesystem.h>
#include <kodi/tools/StringUtils.h>

using namespace kodi::tools;
using namespace iptvsimple;
using namespace iptvsimple::utilities;

namespace
{
// Four decimal octets, each 0-255, the first 127. Shorthands such as 127.1
// are not dotted quads and do not match.
bool IsLoopbackDottedQuad(const std::string& host)
{
  int octets = 0;
  int first = -1;
  size_t pos = 0;
  while (true)
  {
    const size_t dot = host.find('.', pos);
    const std::string part = host.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
    if (part.empty() || part.size() > 3 ||
        !std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isdigit(c); }))
      return false;
    const int value = std::stoi(part);
    if (value > 255)
      return false;
    if (octets == 0)
      first = value;
    ++octets;
    if (dot == std::string::npos)
      break;
    pos = dot + 1;
  }
  return octets == 4 && first == 127;
}
} // namespace

bool WebUtils::SplitAuthority(const std::string& url, UrlAuthority& authority)
{
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos)
    return false;

  const size_t start = schemeEnd + 3;
  size_t end = url.find_first_of("/?#", start);
  if (end == std::string::npos)
    end = url.length();

  size_t hostStart = start;
  const size_t at = url.find('@', start);
  if (at != std::string::npos && at < end)
    hostStart = at + 1;

  size_t hostEnd;
  if (hostStart < end && url[hostStart] == '[')
  {
    hostEnd = url.find(']', hostStart);
    if (hostEnd == std::string::npos || hostEnd >= end)
      return false;
    ++hostEnd;
  }
  else
  {
    hostEnd = url.find(':', hostStart);
    if (hostEnd == std::string::npos || hostEnd > end)
      hostEnd = end;
  }

  authority.hostStart = hostStart;
  authority.hostEnd = hostEnd;
  authority.end = end;
  return true;
}

bool WebUtils::IsOnOrigin(const std::string& url, const std::string& origin)
{
  if (origin.empty() || url.compare(0, origin.size(), origin) != 0)
    return false;
  if (url.size() == origin.size())
    return true;
  const char next = url[origin.size()];
  return next == '/' || next == '?' || next == '#';
}

bool WebUtils::IsLoopbackHost(const std::string& rawHost)
{
  std::string host = rawHost;
  std::transform(host.begin(), host.end(), host.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (host == "localhost")
    return true;

  if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
  {
    host = host.substr(1, host.size() - 2);
    const size_t zone = host.find('%');
    if (zone != std::string::npos)
      host.erase(zone);
    if (host == "::1" || host == "0:0:0:0:0:0:0:1")
      return true;
    static const std::string mapped = "::ffff:";
    return host.rfind(mapped, 0) == 0 && IsLoopbackDottedQuad(host.substr(mapped.size()));
  }

  return IsLoopbackDottedQuad(host);
}

// http://stackoverflow.com/a/17708801
const std::string WebUtils::UrlEncode(const std::string& value)
{
  std::ostringstream escaped;
  escaped.fill('0');
  escaped << std::hex;

  for (auto c : value)
  {
    // Keep alphanumeric and other accepted characters intact.
    // unsigned char cast: std::isalnum on a negative plain char is UB
    // (MSVC debug asserts) — latent while all callers pass ASCII.
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~')
    {
      escaped << c;
      continue;
    }

    // Any other characters are percent-encoded
    escaped << '%' << std::setw(2) << int(static_cast<unsigned char>(c));
  }

  return escaped.str();
}

bool WebUtils::IsHttpUrl(const std::string& url)
{
  return StringUtils::StartsWith(url, HTTP_PREFIX) || StringUtils::StartsWith(url, HTTPS_PREFIX);
}

bool WebUtils::IsNfsUrl(const std::string& url)
{
  return StringUtils::StartsWith(url, NFS_PREFIX);
}

bool WebUtils::IsSpecialUrl(const std::string& url)
{
  return StringUtils::StartsWith(url, SPECIAL_PREFIX);
}

std::string WebUtils::RedactUrl(const std::string& url)
{
  std::string redactedUrl = url;
  static const std::regex regex("^(http:|https:)//[^@/]+:[^@/]+@.*$");
  if (std::regex_match(url, regex))
  {
    std::string protocol = url.substr(0, url.find_first_of(":"));
    std::string fullPrefix = url.substr(url.find_first_of("@") + 1);

    redactedUrl = protocol + "://USERNAME:PASSWORD@" + fullPrefix;
  }

  // Redact credential-bearing query parameters. The Jellyfin access token
  // rides on stream URLs as "ApiKey=" and Quick Connect polls with "secret=".
  // Kodi writes INFO-level logs by default, so any URL that reaches the log
  // must have these values masked.
  static const std::regex credentialParams(
      R"(([?&](?:api_?key|token|secret|x-emby-token)=)[^&#]*)",
      std::regex::icase);
  redactedUrl = std::regex_replace(redactedUrl, credentialParams, "$1REDACTED");

  return redactedUrl;
}

bool WebUtils::Check(const std::string& strURL, int connectionTimeoutSecs, bool isLocalPath)
{
  // For local paths we only need to check existence of the file
  if ((isLocalPath || IsSpecialUrl(strURL)) && FileUtils::FileExists(strURL))
    return true;

  //Otherwise it's remote
  kodi::vfs::CFile fileHandle;
  if (!fileHandle.CURLCreate(strURL))
  {
    Logger::Log(LEVEL_ERROR, "%s Unable to create curl handle for %s", __func__, WebUtils::RedactUrl(strURL).c_str());
    return false;
  }

  if (!IsNfsUrl(strURL))
    fileHandle.CURLAddOption(ADDON_CURL_OPTION_PROTOCOL, "connection-timeout", std::to_string(connectionTimeoutSecs));

  if (!fileHandle.CURLOpen(ADDON_READ_NO_CACHE))
  {
    Logger::Log(LEVEL_DEBUG, "%s Unable to open url: %s", __func__, WebUtils::RedactUrl(strURL).c_str());
    return false;
  }

  return true;
}
