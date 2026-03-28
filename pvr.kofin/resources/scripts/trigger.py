import sys
import xbmcaddon

addon = xbmcaddon.Addon('pvr.kofin')
if len(sys.argv) > 1:
    addon.setSetting(sys.argv[1], 'trigger')
