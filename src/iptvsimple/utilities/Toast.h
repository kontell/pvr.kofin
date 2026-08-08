/*
 *  Copyright (C) 2005-2025 Team Kodi (https://kodi.tv)
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
    /**
     * Every user-visible notification the addon raises.
     *
     * The level a caller asks for is what picks the icon:
     *
     *  - Info    kofin reporting that something happened. The addon icon says
     *            who is talking, which is the useful information; Kodi's blue
     *            "i" says nothing a viewer needs.
     *  - Warning
     *  - Error   something was refused or went wrong, and Kodi's own glyph is
     *            the fastest way to read that at a glance. Branding a failure
     *            only softens it.
     *
     * This mirrors plugin.video.kofin's lib/kofin/core/toast.py, but it cannot
     * mirror its implementation. Python takes the icon as a plain string and
     * has no level argument at all, so any path can be handed to Kodi
     * (interfaces/legacy/Dialog.cpp). The binary API instead carries a type
     * enum *and* an image argument, and the two contradict each other: with
     * QUEUE_INFO/WARNING/ERROR Kodi discards the image and logs "To use given
     * image file ... must be type value set to 'QUEUE_OWN_STYLE'", pins the
     * display time to 3000 ms and forces sound on for warning and error
     * (addons/interfaces/General.cpp). QUEUE_OWN_STYLE is the only route that
     * accepts an image — which is also how the adverse levels keep Kodi's
     * glyph while still choosing their own duration and silence.
     */
    class Toast
    {
    public:
      static void Info(const std::string& message);
      static void Warning(const std::string& message);
      static void Error(const std::string& message);

      /** Overloads for the localized-string case, which is every call site. */
      static void Info(int localizedStringId);
      static void Warning(int localizedStringId);
      static void Error(int localizedStringId);
    };
  } // namespace utilities
} // namespace iptvsimple
