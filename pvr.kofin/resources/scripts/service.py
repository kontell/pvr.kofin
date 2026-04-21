"""
pvr.kofin playback reporter — reports playback state to Jellyfin dashboard.

Uses xbmc.Player callbacks for start/stop/pause detection and urllib for HTTP
(bypasses Kodi's curl pool entirely, avoiding the UI-hang bug from C++ reporter).
"""
import json
import time
import urllib.request
import urllib.error

import xbmc
import xbmcaddon


ADDON_ID = 'pvr.kofin'
REPORT_INTERVAL = 10  # seconds between progress reports
SESSION_KEYS = (
    'sessionItemId',
    'sessionMediaSourceId',
    'sessionPlaySessionId',
    'sessionLiveStreamId',
    'sessionPlayMethod',
)


def get_device_name():
    name = xbmc.getInfoLabel('System.FriendlyName') or ''
    name = name.strip()
    if not name or name.lower() == 'kodi':
        ip = (xbmc.getInfoLabel('Network.IPAddress') or '').strip()
        name = f'Kodi ({ip})' if ip else 'Kodi'
    return name


def build_auth_header(token, device_id):
    version = xbmcaddon.Addon(ADDON_ID).getAddonInfo('version')
    device = get_device_name().replace('"', '')
    header = (
        f'MediaBrowser Client="Kofin PVR", Device="{device}"'
        f', DeviceId="{device_id}"'
        f', Version="{version}"'
    )
    if token:
        header += f', Token="{token}"'
    return header


def post_json(base_url, endpoint, body, token, device_id):
    """POST JSON to Jellyfin. Fire-and-forget — errors are logged, not raised."""
    url = base_url.rstrip('/') + endpoint
    data = json.dumps(body).encode('utf-8')
    headers = {
        'Content-Type': 'application/json',
        'X-Emby-Authorization': build_auth_header(token, device_id),
    }
    req = urllib.request.Request(url, data=data, headers=headers, method='POST')
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            resp.read()
    except (urllib.error.URLError, OSError) as e:
        xbmc.log(f'pvr.kofin reporter: POST {endpoint} failed: {e}', xbmc.LOGWARNING)


class PlaybackReporter(xbmc.Player):
    def __init__(self):
        super().__init__()
        self.session = None
        self.paused = False
        self.start_time = None

    def onAVStarted(self):
        """Stream is up — read session data written by C++ addon and report start."""
        # Player callbacks fire for ALL playback. Skip if this isn't PVR —
        # getPlayingFile() returns the resolved stream URL (not a pvr:// URL)
        # so we gate on the PVR playback condition instead.
        if not (xbmc.getCondVisibility('PVR.IsPlayingTV') or
                xbmc.getCondVisibility('PVR.IsPlayingRecording')):
            return

        addon = xbmcaddon.Addon(ADDON_ID)
        item_id = addon.getSetting('sessionItemId')
        if not item_id:
            return

        self.session = {
            'ItemId': item_id,
            'MediaSourceId': addon.getSetting('sessionMediaSourceId'),
            'PlaySessionId': addon.getSetting('sessionPlaySessionId'),
            'LiveStreamId': addon.getSetting('sessionLiveStreamId'),
            'PlayMethod': addon.getSetting('sessionPlayMethod'),
            'BaseUrl': addon.getSetting('jellyfinServerAddress'),
            'Token': addon.getSetting('jellyfinAccessToken'),
            'DeviceId': addon.getSetting('deviceId'),
        }
        self.paused = False
        self.start_time = time.time()

        content = 'live TV' if xbmc.getCondVisibility('PVR.IsPlayingTV') else \
                  'recording' if xbmc.getCondVisibility('PVR.IsPlayingRecording') else \
                  'video'
        xbmc.log(f'pvr.kofin reporter: playback started ({content})', xbmc.LOGINFO)

        self._send('/Sessions/Playing', self._build_body())

    def onPlayBackStopped(self):
        self._stop()

    def onPlayBackEnded(self):
        self._stop()

    def onPlayBackError(self):
        self._stop()

    def onPlayBackPaused(self):
        self.paused = True

    def onPlayBackResumed(self):
        self.paused = False

    def _stop(self):
        if not self.session:
            return
        session = self.session
        self.session = None

        # Clear persisted session so the next non-kofin playback (e.g. a music
        # track from another addon) isn't misreported as a resumed kofin session.
        addon = xbmcaddon.Addon(ADDON_ID)
        for key in SESSION_KEYS:
            addon.setSetting(key, '')

        xbmc.log('pvr.kofin reporter: playback stopped', xbmc.LOGINFO)

        # Report playback stopped
        self._send_with(session, '/Sessions/Playing/Stopped', {
            'ItemId': session['ItemId'],
            'MediaSourceId': session['MediaSourceId'],
            'PlaySessionId': session['PlaySessionId'],
        })

        # Close the live stream if one was opened
        live_stream_id = session.get('LiveStreamId', '')
        if live_stream_id:
            self._send_with(session, '/LiveStreams/Close', {
                'LiveStreamId': live_stream_id,
            })

    def report_progress(self):
        """Called from main loop. Sends progress if session is active."""
        if not self.session:
            return
        self._send('/Sessions/Playing/Progress', self._build_body())

    def _build_body(self):
        elapsed = time.time() - self.start_time if self.start_time else 0
        return {
            'QueueableMediaTypes': 'Video,Audio',
            'CanSeek': True,
            'ItemId': self.session['ItemId'],
            'MediaSourceId': self.session['MediaSourceId'],
            'PlayMethod': self.session['PlayMethod'],
            'PlaySessionId': self.session['PlaySessionId'],
            'PositionTicks': int(elapsed * 10_000_000),
            'IsPaused': self.paused,
            'IsMuted': False,
            'VolumeLevel': 100,
        }

    def _send(self, endpoint, body):
        if not self.session:
            return
        self._send_with(self.session, endpoint, body)

    def _send_with(self, session, endpoint, body):
        base_url = session.get('BaseUrl', '')
        token = session.get('Token', '')
        device_id = session.get('DeviceId', '')
        if not base_url or not token:
            return
        post_json(base_url, endpoint, body, token, device_id)


if __name__ == '__main__':
    monitor = xbmc.Monitor()
    player = PlaybackReporter()
    xbmc.log('pvr.kofin reporter: started', xbmc.LOGINFO)

    while not monitor.abortRequested():
        if monitor.waitForAbort(REPORT_INTERVAL):
            break
        player.report_progress()

    # Clean up on exit — report stopped if still playing
    if player.session:
        player._stop()

    xbmc.log('pvr.kofin reporter: stopped', xbmc.LOGINFO)
