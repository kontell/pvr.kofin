/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include <kodi/addon-instance/PVR.h>

namespace iptvsimple
{
  class ATTR_DLL_LOCAL IConnectionListener : public kodi::addon::CInstancePVRClient
  {
  public:
    IConnectionListener(const kodi::addon::IInstanceInfo& instance)
      : kodi::addon::CInstancePVRClient(instance) { }
    virtual ~IConnectionListener() = default;

    virtual void ConnectionLost() = 0;
    // False when the server answered the health check but the connect did not
    // complete (authentication or the channel load failed); the connection
    // manager then calls it again at the next successful check.
    virtual bool ConnectionEstablished() = 0;
  };
} // namespace iptvsimple
