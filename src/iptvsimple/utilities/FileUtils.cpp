/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "FileUtils.h"

#include <kodi/Filesystem.h>

using namespace iptvsimple;
using namespace iptvsimple::utilities;

bool FileUtils::FileExists(const std::string& file)
{
  return kodi::vfs::FileExists(file, false);
}
