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
  namespace utilities
  {
    class FileUtils
    {
    public:
      static bool FileExists(const std::string& file);
    };
  } // namespace utilities
} // namespace iptvsimple
