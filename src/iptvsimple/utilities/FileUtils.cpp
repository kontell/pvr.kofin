/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "FileUtils.h"

#include "Logger.h"

using namespace iptvsimple;
using namespace iptvsimple::utilities;

int FileUtils::GetFileContents(const std::string& url, std::string& content)
{
  content.clear();
  kodi::vfs::CFile file;
  if (file.OpenFile(url))
  {
    char buffer[1024];
    while (int bytesRead = file.Read(buffer, 1024))
      content.append(buffer, bytesRead);
  }

  return content.length();
}

bool FileUtils::FileExists(const std::string& file)
{
  return kodi::vfs::FileExists(file, false);
}

bool FileUtils::CopyFile(const std::string& sourceFile, const std::string& targetFile)
{
  kodi::vfs::CFile file;
  bool copySuccessful = true;

  Logger::Log(LEVEL_DEBUG, "%s - Copying file: %s, to %s", __FUNCTION__, sourceFile.c_str(), targetFile.c_str());

  if (file.OpenFile(sourceFile, ADDON_READ_NO_CACHE))
  {
    const std::string fileContents = ReadFileContents(file);

    file.Close();

    if (file.OpenFileForWrite(targetFile, true))
    {
      file.Write(fileContents.c_str(), fileContents.length());
    }
    else
    {
      Logger::Log(LEVEL_ERROR, "%s - Could not open target file to copy to: %s", __FUNCTION__, targetFile.c_str());
      copySuccessful = false;
    }
  }
  else
  {
    Logger::Log(LEVEL_ERROR, "%s - Could not open source file to copy: %s", __FUNCTION__, sourceFile.c_str());
    copySuccessful = false;
  }

  return copySuccessful;
}

bool FileUtils::CopyDirectory(const std::string& sourceDir, const std::string& targetDir, bool recursiveCopy)
{
  bool copySuccessful = true;

  kodi::vfs::CreateDirectory(targetDir);

  std::vector<kodi::vfs::CDirEntry> entries;
  if (kodi::vfs::GetDirectory(sourceDir, "", entries))
  {
    for (const auto& entry : entries)
    {
      if (entry.IsFolder() && recursiveCopy)
      {
        copySuccessful = CopyDirectory(sourceDir + "/" + entry.Label(), targetDir + "/" + entry.Label(), true);
      }
      else if (!entry.IsFolder())
      {
        copySuccessful = CopyFile(sourceDir + "/" + entry.Label(), targetDir + "/" + entry.Label());
      }
    }
  }
  else
  {
    Logger::Log(LEVEL_ERROR, "%s - Could not copy directory: %s, to directory: %s", __FUNCTION__, sourceDir.c_str(), targetDir.c_str());
    copySuccessful = false;
  }
  return copySuccessful;
}

std::string FileUtils::GetResourceDataPath()
{
  return kodi::addon::GetAddonPath("/resources/data");
}

std::string FileUtils::ReadFileContents(kodi::vfs::CFile& file)
{
  std::string fileContents;

  char buffer[1024];
  int bytesRead = 0;

  // Read until EOF or explicit error
  while ((bytesRead = file.Read(buffer, sizeof(buffer) - 1)) > 0)
    fileContents.append(buffer, bytesRead);

  return fileContents;
}
