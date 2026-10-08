#  Copyright (C) 2026 Kofin
#
#  SPDX-License-Identifier: GPL-2.0-or-later
#  See LICENSE.md for more information.

import sys

import xbmcaddon

# The add-on's one RunScript entry point. addon.xml declares this file as the
# xbmc.python.library extension, which is what lets settings.xml say
# RunScript(pvr.kofin,<action>) without naming the directory the add-on is
# installed in.
#
# A button action round-trips into the C++ addon's SetSetting callback (see the
# settings.xml button definitions). Only the four known button IDs may be
# poked: RunScript is callable by any addon or skin, so arbitrary setting names
# must not be writable through here.
ALLOWED_BUTTONS = ('loginButton', 'logoutButton', 'testConnection', 'restartAddon')

action = sys.argv[1] if len(sys.argv) > 1 else ''

if action in ALLOWED_BUTTONS:
    xbmcaddon.Addon('pvr.kofin').setSetting(action, 'trigger')
elif action == 'discover':
    import discover
    discover.run()
