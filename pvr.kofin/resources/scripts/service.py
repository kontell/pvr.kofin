import xbmc
import xbmcaddon


class PlayerMonitor(xbmc.Player):
    def onPlayBackStopped(self):
        xbmc.log("pvr.kofin service: playback stopped", xbmc.LOGINFO)
        try:
            addon = xbmcaddon.Addon('pvr.kofin')
            addon.setSetting('playbackStopped', 'trigger')
        except Exception:
            pass

    def onPlayBackEnded(self):
        xbmc.log("pvr.kofin service: playback ended", xbmc.LOGINFO)
        try:
            addon = xbmcaddon.Addon('pvr.kofin')
            addon.setSetting('playbackStopped', 'trigger')
        except Exception:
            pass

    def onPlayBackError(self):
        xbmc.log("pvr.kofin service: playback error", xbmc.LOGINFO)
        try:
            addon = xbmcaddon.Addon('pvr.kofin')
            addon.setSetting('playbackStopped', 'trigger')
        except Exception:
            pass


if __name__ == '__main__':
    monitor = xbmc.Monitor()
    player = PlayerMonitor()
    xbmc.log("pvr.kofin service: started", xbmc.LOGINFO)

    while not monitor.abortRequested():
        if monitor.waitForAbort(1):
            break

    xbmc.log("pvr.kofin service: stopped", xbmc.LOGINFO)
