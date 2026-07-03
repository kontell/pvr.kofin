/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include <string>

namespace iptvsimple
{
  enum class StreamType
    : int // same type as addon settings
  {
    HLS = 0,
    DASH,
    SMOOTH_STREAMING,
    TS,
    PLUGIN,
    MIME_TYPE_UNRECOGNISED,
    OTHER_TYPE
  };

  namespace utilities
  {
    class StreamUtils
    {
    public:
      static const StreamType GetStreamType(const std::string& url, const std::string& mimeType, bool isCatchupTSStream);
      static const std::string GetManifestType(const StreamType& streamType);
      static const std::string GetMimeType(const StreamType& streamType);
      static std::string AddHeader(const std::string& headerTarget, const std::string& headerName, const std::string& headerValue, bool encodeHeaderValue);
      static std::string AddHeaderToStreamUrl(const std::string& streamUrl, const std::string& headerName, const std::string& headerValue);
    };
  } // namespace utilities
} // namespace iptvsimple
