/*
 *  Copyright (C) 2005-2025 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "Toast.h"

#include <kodi/AddonBase.h>
#include <kodi/General.h>

using namespace iptvsimple::utilities;

namespace
{
// "Kofin PVR" rather than the sibling addon's plain "Kofin": both can be
// installed at once, and the heading is what says which of them spoke.
constexpr const char* HEADING = "Kofin PVR";

// plugin.video.kofin's toast defaults. Kodi's own are 3000 ms and sound on;
// a notification that beeps for every refused button press wears thin.
constexpr unsigned int DISPLAY_TIME_MS = 5000;
constexpr unsigned int MESSAGE_TIME_MS = 1000;

// The textures GUIDialogKaiToast falls back to when it picks an icon for a
// message type itself (dialogs/GUIDialogKaiToast.cpp; same names in Omega and
// Piers). Naming them explicitly is what lets an adverse toast keep Kodi's
// glyph and still set its own duration and silence.
constexpr const char* ICON_WARNING = "DefaultIconWarning.png";
constexpr const char* ICON_ERROR = "DefaultIconError.png";

void ShowWithIcon(const std::string& message, const std::string& icon)
{
  kodi::QueueNotification(QUEUE_OWN_STYLE, HEADING, message, icon, DISPLAY_TIME_MS, false,
                          MESSAGE_TIME_MS);
}
} // unnamed namespace

void Toast::Info(const std::string& message)
{
  const std::string icon = kodi::addon::GetAddonPath("icon.png");
  if (icon.empty())
  {
    // Kodi could not resolve the addon's own directory. Fall back to its glyph
    // rather than hand it a path to nowhere, which draws a blank icon.
    kodi::QueueNotification(QUEUE_INFO, HEADING, message);
    return;
  }

  ShowWithIcon(message, icon);
}

void Toast::Warning(const std::string& message)
{
  ShowWithIcon(message, ICON_WARNING);
}

void Toast::Error(const std::string& message)
{
  ShowWithIcon(message, ICON_ERROR);
}

void Toast::Info(int localizedStringId)
{
  Info(kodi::addon::GetLocalizedString(localizedStringId));
}

void Toast::Warning(int localizedStringId)
{
  Warning(kodi::addon::GetLocalizedString(localizedStringId));
}

void Toast::Error(int localizedStringId)
{
  Error(kodi::addon::GetLocalizedString(localizedStringId));
}
