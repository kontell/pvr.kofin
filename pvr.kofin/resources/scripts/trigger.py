import sys

import xbmcaddon

# Round-trips a settings action button into the C++ addon's SetSetting
# callback (see the settings.xml button definitions). Only the four known
# button IDs may be poked: RunScript is callable by any addon or skin, so
# arbitrary setting names must not be writable through here.
ALLOWED_BUTTONS = ('loginButton', 'logoutButton', 'testConnection', 'restartAddon')

if len(sys.argv) > 1 and sys.argv[1] in ALLOWED_BUTTONS:
    xbmcaddon.Addon('pvr.kofin').setSetting(sys.argv[1], 'trigger')
