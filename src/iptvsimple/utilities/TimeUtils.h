/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include <cstdlib>
#include <ctime>

inline std::tm SafeLocaltime(const std::time_t& time)
{
  std::tm tm_snapshot;
#if (defined(WIN32) || defined(_WIN32) || defined(__WIN32__))
  localtime_s(&tm_snapshot, &time);
#else
  localtime_r(&time, &tm_snapshot); // POSIX
#endif
  return tm_snapshot;
}

inline std::tm SafeGmtime(const std::time_t& time)
{
  std::tm tm_snapshot;
#if (defined(WIN32) || defined(_WIN32) || defined(__WIN32__))
  gmtime_s(&tm_snapshot, &time);
#else
  gmtime_r(&time, &tm_snapshot); // POSIX
#endif
  return tm_snapshot;
}

inline std::time_t SafeTimegm(std::tm* tm)
{
#if (defined(WIN32) || defined(_WIN32) || defined(__WIN32__))
  return _mkgmtime(tm);
#else
  return timegm(tm); // POSIX (BSD/glibc extension)
#endif
}
