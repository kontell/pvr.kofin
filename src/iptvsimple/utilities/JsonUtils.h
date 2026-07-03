/*
 *  Copyright (C) 2025 Kofin
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include <cstdint>

#include <json/json.h>

namespace iptvsimple
{
namespace utilities
{
  // jsoncpp's asInt()/asInt64()/asUInt64() throw Json::LogicError when the
  // value has an incompatible type or is out of range (e.g. a negative
  // FreeSpace read as uint64, or a string where a number is expected).
  // Exceptions must never escape through the Kodi C ABI, so numeric reads of
  // server-supplied JSON go through these range-checked helpers instead.
  inline int SafeInt(const Json::Value& value, int defaultValue = 0)
  {
    return value.isInt() ? value.asInt() : defaultValue;
  }

  inline int64_t SafeInt64(const Json::Value& value, int64_t defaultValue = 0)
  {
    return value.isInt64() ? value.asInt64() : defaultValue;
  }

  inline uint64_t SafeUInt64(const Json::Value& value, uint64_t defaultValue = 0)
  {
    return value.isUInt64() ? value.asUInt64() : defaultValue;
  }
} // namespace utilities
} // namespace iptvsimple
