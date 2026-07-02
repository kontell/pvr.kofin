"""
pvr.kofin playback reporter — reports playback state to the Jellyfin server.

This lives in Python because the binary PVR addon API exposes no player-event
callbacks. Once a channel or recording is handed to an inputstream via
GetChannelStreamProperties/GetRecordingStreamProperties, the C++ addon drops
out of the data path and never sees start/stop/pause/resume/seek — and even
CloseLiveStream is unreliable under the stream-properties path. Kodi delivers
those events only through xbmc.Player/xbmc.Monitor (the script API, with no
binary-addon equivalent), so backend session reporting — Sessions/Playing,
.../Progress, .../Stopped, and LiveStreams/Close — has to run from a service
script. HTTP uses urllib directly, independent of Kodi's HTTP stack.
"""
import json
import os
import time
import urllib.request
import urllib.error

import xbmc
import xbmcaddon
import xbmcvfs


ADDON_ID = 'pvr.kofin'
REPORT_INTERVAL = 10  # seconds between progress reports


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


def normalize_base_url(address):
    """Mirror InstanceSettings::GetJellyfinBaseUrl: default scheme http,
    default port 8096 (http) / 443 (https), strip any path. The raw setting
    may be a bare host/IP — urllib needs a full URL."""
    address = (address or '').strip()
    if not address:
        return ''
    while address.endswith('/'):
        address = address[:-1]
    if address.startswith('https://'):
        scheme, remainder = 'https', address[8:]
    elif address.startswith('http://'):
        scheme, remainder = 'http', address[7:]
    else:
        scheme, remainder = 'http', address
    slash = remainder.find('/')
    if slash != -1:
        remainder = remainder[:slash]
    if remainder.startswith('['):  # IPv6 bracket notation [::1]:port
        bracket_end = remainder.find(']')
        has_port = (bracket_end != -1 and bracket_end + 1 < len(remainder)
                    and remainder[bracket_end + 1] == ':')
    else:
        has_port = ':' in remainder
    if not has_port:
        remainder += ':443' if scheme == 'https' else ':8096'
    return f'{scheme}://{remainder}'


def post_json(base_url, endpoint, body, token, device_id):
    """POST JSON to Jellyfin. Fire-and-forget — errors are logged, not raised."""
    base = normalize_base_url(base_url)
    if not base:
        return
    url = base + endpoint
    data = json.dumps(body).encode('utf-8')
    headers = {
        'Content-Type': 'application/json',
        # Non-deprecated header form, matching the C++ client — Jellyfin v12
        # drops X-Emby-Authorization support.
        'Authorization': build_auth_header(token, device_id),
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
        self.is_recording = False
        self.last_position_ticks = 0

    def onAVStarted(self):
        """Stream is up — read session data written by C++ addon and report start."""
        # Player callbacks fire for ALL playback. Skip if this isn't PVR —
        # getPlayingFile() returns the resolved stream URL (not a pvr:// URL)
        # so we gate on the PVR playback condition instead.
        if not (xbmc.getCondVisibility('PVR.IsPlayingTV') or
                xbmc.getCondVisibility('PVR.IsPlayingRecording')):
            return

        addon = xbmcaddon.Addon(ADDON_ID)
        session_path = os.path.join(
            xbmcvfs.translatePath(addon.getAddonInfo('profile')),
            'session.json')
        try:
            with open(session_path, 'r') as f:
                session_data = json.load(f)
        except (OSError, json.JSONDecodeError):
            return
        # Ignore stale session files (e.g. left behind by a Kodi crash): with
        # a second PVR addon installed, the PVR.IsPlaying* gate alone would
        # misattribute foreign playback to this session. WrittenAt is set by
        # the C++ addon when it resolves the stream URL, moments before
        # playback starts; 0/absent means an older addon build — accept it.
        written_at = session_data.get('WrittenAt', 0)
        if written_at and time.time() - written_at > 300:
            xbmc.log('pvr.kofin reporter: ignoring stale session.json', xbmc.LOGINFO)
            return
        item_id = session_data.get('ItemId', '')
        if not item_id:
            return

        self.is_recording = xbmc.getCondVisibility('PVR.IsPlayingRecording')
        self.last_position_ticks = 0
        self.session = {
            'ItemId': item_id,
            'MediaSourceId': session_data.get('MediaSourceId', ''),
            'PlaySessionId': session_data.get('PlaySessionId', ''),
            'LiveStreamId': session_data.get('LiveStreamId', ''),
            'PlayMethod': session_data.get('PlayMethod', ''),
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
        self._capture_position()

    def onPlayBackResumed(self):
        self.paused = False
        self._capture_position()

    def onPlayBackSeek(self, seek_time, seek_offset):
        if self.is_recording:
            # seek_time is the new playhead in ms; ticks are 100ns
            self.last_position_ticks = int(seek_time * 10_000)

    def _stop(self):
        if not self.session:
            return
        self._capture_position()
        session = self.session
        is_recording = self.is_recording
        final_position_ticks = self.last_position_ticks
        self.session = None

        # Clear persisted session so the next non-kofin playback (e.g. a music
        # track from another addon) isn't misreported as a resumed kofin session.
        addon = xbmcaddon.Addon(ADDON_ID)
        session_path = os.path.join(
            xbmcvfs.translatePath(addon.getAddonInfo('profile')),
            'session.json')
        try:
            os.remove(session_path)
        except OSError:
            pass

        xbmc.log('pvr.kofin reporter: playback stopped', xbmc.LOGINFO)

        # Report playback stopped
        stopped_body = {
            'ItemId': session['ItemId'],
            'MediaSourceId': session['MediaSourceId'],
            'PlaySessionId': session['PlaySessionId'],
        }
        if is_recording:
            # Without an explicit position the server's stop handler infers
            # one — and infers wrong when seeks have shifted the playhead
            # off the wall-clock elapsed value seen in /Progress events.
            stopped_body['PositionTicks'] = final_position_ticks
        self._send_with(session, '/Sessions/Playing/Stopped', stopped_body)

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
        return {
            'QueueableMediaTypes': 'Video,Audio',
            'CanSeek': True,
            'ItemId': self.session['ItemId'],
            'MediaSourceId': self.session['MediaSourceId'],
            'PlayMethod': self.session['PlayMethod'],
            'PlaySessionId': self.session['PlaySessionId'],
            'PositionTicks': self._position_ticks(),
            'IsPaused': self.paused,
            'IsMuted': False,
            'VolumeLevel': 100,
        }

    def _position_ticks(self):
        # Recordings: ask the player for the real playhead. Wall-clock elapsed
        # drifts off the actual position after seeks/skips, and the server
        # uses /Progress and /Stopped position to decide watched state. Live
        # TV has no per-channel watched state — keep wall-clock elapsed there
        # so live-TV reporting is byte-for-byte unchanged.
        if self.is_recording:
            self._capture_position()
            return self.last_position_ticks
        elapsed = time.time() - self.start_time if self.start_time else 0
        return int(elapsed * 10_000_000)

    def _capture_position(self):
        if not self.is_recording:
            return
        try:
            self.last_position_ticks = int(self.getTime() * 10_000_000)
        except RuntimeError:
            # Player not active (during stop teardown) — keep cached value
            pass

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
