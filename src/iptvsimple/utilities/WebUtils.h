/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include <cstddef>
#include <map>
#include <string>

namespace iptvsimple
{
  namespace utilities
  {
    static const std::string HTTP_PREFIX = "http://";
    static const std::string HTTPS_PREFIX = "https://";
    static const std::string NFS_PREFIX = "nfs://";
    static const std::string SPECIAL_PREFIX = "special://";

    class WebUtils
    {
    public:
      // Offsets into a URL: the host runs from hostStart to hostEnd, and the
      // authority (userinfo, host and port) ends at end, where the path,
      // query or fragment begins.
      struct UrlAuthority
      {
        size_t hostStart = 0;
        size_t hostEnd = 0;
        size_t end = 0;
      };

      // Locates the host of a "scheme://" URL: after any userinfo, before any
      // port. An IPv6 literal holds colons of its own, so it keeps its
      // brackets and runs to the closing one. False when there is no "://" or
      // the bracket is never closed.
      static bool SplitAuthority(const std::string& url, UrlAuthority& authority);
      // Whether a host, as SplitAuthority finds it, names this machine:
      // localhost in any case, a dotted-quad 127.0.0.0/8 address, [::1] in
      // either spelling or an IPv4-mapped loopback, with or without a zone.
      static bool IsLoopbackHost(const std::string& host);
      static const std::string UrlEncode(const std::string& value);
      static bool IsHttpUrl(const std::string& url);
      static bool IsNfsUrl(const std::string& url);
      static bool IsSpecialUrl(const std::string& url);
      static std::string RedactUrl(const std::string& url);
      static bool Check(const std::string& url, int connectionTimeoutSecs,
                        bool isLocalPath = false);
    };
  } // namespace utilities
} // namespace iptvsimple
