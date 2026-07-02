/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include <kodi/Filesystem.h>
#include <string>

namespace iptvsimple
{
  namespace utilities
  {
    class FileUtils
    {
    public:
      static int GetFileContents(const std::string& url, std::string& content);
      static bool FileExists(const std::string& file);
      static bool CopyFile(const std::string& sourceFile, const std::string& targetFile);
      static bool CopyDirectory(const std::string& sourceDir, const std::string& targetDir, bool recursiveCopy);
      static std::string GetResourceDataPath();

    private:
      static std::string ReadFileContents(kodi::vfs::CFile& fileHandle);
    };
  } // namespace utilities
} // namespace iptvsimple
