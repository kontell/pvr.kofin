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
import ssl
import time
import urllib.request
import urllib.error
import urllib.parse

import xbmc
import xbmcaddon
import xbmcvfs


ADDON_ID = 'pvr.kofin'
REPORT_INTERVAL = 10  # seconds between progress reports
# The provider name catchup content claims under (the public provider
# contract); recordings and live channels stay provider "jellyfin".
SYNC_PROVIDER = 'pvr.kofin'
# A programme whose end is at least this far gone is catchup, not live —
# covers EPG clock skew around a live programme's final minute.
CATCHUP_GRACE_SECS = 60
# inputstream.tempo's shared tempo file — the one it polls when a stream
# names none — and the state line it answers with. This add-on's C++ side
# stamps no tempo_file, so every tempo-routed play of ours reports there.
TEMPO_FILE = 'special://temp/inputstream_tempo'
TEMPO_QUEUE_SECS_DEFAULT = 8.0  # Kodi 21 hard-codes its demux queue


def get_addon():
    """Kodi unregisters the addon for a moment while it is installed over the
    running copy, and xbmcaddon.Addon() raises RuntimeError('Unknown addon id')
    for the whole of that window. Uncaught, it kills the reporter for the rest
    of the Kodi session, so callers degrade instead of dying."""
    try:
        return xbmcaddon.Addon(ADDON_ID)
    except RuntimeError:
        return None


def get_setting(key):
    """Read live — the token and server address change when the user logs in
    while the reporter is already running."""
    addon = get_addon()
    return addon.getSetting(key) if addon else ''


# Neither of these changes while the script runs, so read them once at startup
# rather than exposing the playback paths to the reinstall window above.
_addon = get_addon()
ADDON_VERSION = _addon.getAddonInfo('version') if _addon else ''
_profile = _addon.getAddonInfo('profile') if _addon else ''
SESSION_PATH = (os.path.join(xbmcvfs.translatePath(_profile), 'session.json')
                if _profile else '')


def get_device_name():
    name = xbmc.getInfoLabel('System.FriendlyName') or ''
    name = name.strip()
    if not name or name.lower() == 'kodi':
        ip = (xbmc.getInfoLabel('Network.IPAddress') or '').strip()
        name = f'Kodi ({ip})' if ip else 'Kodi'
    return name


def build_auth_header(token, device_id):
    device = get_device_name().replace('"', '')
    header = (
        f'MediaBrowser Client="Kofin PVR", Device="{device}"'
        f', DeviceId="{device_id}"'
        f', Version="{ADDON_VERSION}"'
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


def ssl_context():
    """An unverified SSL context when the user turned sslVerify off, else None.

    Read live rather than cached: the setting can change while the reporter is
    running, same as the token and server address. None means "urlopen's
    default", which is a fully verifying context.
    """
    if get_setting('sslVerify') != 'false':
        return None
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    return context


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
        with urllib.request.urlopen(req, timeout=5, context=ssl_context()) as resp:
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
        # Player callbacks fire for ALL playback. Only treat this as a kofin
        # stream if PVR playback is active and a fresh session.json exists —
        # getPlayingFile() returns the resolved stream URL (not a pvr:// URL)
        # so we gate on the PVR playback condition instead.
        session_data = None
        # EPG-tag playback answers neither IsPlayingTV nor IsPlayingRecording
        # on every path: the tempo catchup route reports PVR.IsPlayingEpgTag
        # (measured on Piers), the ffmpegdirect route reported IsPlayingTV.
        if (xbmc.getCondVisibility('PVR.IsPlayingTV') or
                xbmc.getCondVisibility('PVR.IsPlayingRecording') or
                xbmc.getCondVisibility('PVR.IsPlayingEpgTag')):
            try:
                with open(SESSION_PATH, 'r') as f:
                    session_data = json.load(f)
            except (OSError, json.JSONDecodeError):
                session_data = None
        if session_data:
            # Ignore stale session files (e.g. left behind by a Kodi crash):
            # with a second PVR addon installed, the PVR.IsPlaying* gate alone
            # would misattribute foreign playback to this session. WrittenAt is
            # set by the C++ addon when it resolves the stream URL, moments
            # before playback starts; 0/absent means an older addon build —
            # accept it.
            written_at = session_data.get('WrittenAt', 0)
            if written_at and time.time() - written_at > 300:
                xbmc.log('pvr.kofin reporter: ignoring stale session.json',
                         xbmc.LOGINFO)
                session_data = None
            elif not session_data.get('ItemId', ''):
                session_data = None

        # A new playback can replace ours without any stop event (seamless
        # channel zap) — finalize the outgoing session first, or its live
        # stream is never closed on the server. Same PlaySessionId means a
        # duplicate start event for the current stream: keep it. (Residual
        # gap: a stream whose playback never reaches onAVStarted can still
        # leak — its LiveStreamId only ever lived in session.json, which the
        # C++ addon has since overwritten.)
        if self.session:
            new_psid = session_data.get('PlaySessionId', '') if session_data else None
            if new_psid != self.session['PlaySessionId']:
                xbmc.log('pvr.kofin reporter: playback replaced without stop event',
                         xbmc.LOGINFO)
                self._finalize(self.session, self.is_recording,
                               self.last_position_ticks)
                self.session = None

        if not session_data:
            return

        self.is_recording = xbmc.getCondVisibility('PVR.IsPlayingRecording')
        self.last_position_ticks = 0
        self.session = {
            'ItemId': session_data.get('ItemId', ''),
            'MediaSourceId': session_data.get('MediaSourceId', ''),
            'PlaySessionId': session_data.get('PlaySessionId', ''),
            'LiveStreamId': session_data.get('LiveStreamId', ''),
            'PlayMethod': session_data.get('PlayMethod', ''),
            'WrittenAt': session_data.get('WrittenAt', 0),
            'BaseUrl': get_setting('jellyfinServerAddress'),
            'Token': get_setting('jellyfinAccessToken'),
            'DeviceId': get_setting('deviceId'),
        }
        self.paused = False
        self.start_time = time.time()

        content = 'live TV' if xbmc.getCondVisibility('PVR.IsPlayingTV') else \
                  'recording' if xbmc.getCondVisibility('PVR.IsPlayingRecording') else \
                  'video'
        xbmc.log(f'pvr.kofin reporter: playback started ({content})', xbmc.LOGINFO)

        self._send('/Sessions/Playing', self._build_body())
        self._send_sync_claim()

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

        # Clear the persisted session so the next non-kofin playback (e.g. a
        # music track from another addon) isn't misreported as a resumed kofin
        # session — but only while the file still belongs to the session that
        # just ended: on a zap the C++ addon has already written the NEXT
        # channel's session.json by the time this stop event runs, and deleting
        # that would leave the new stream untracked (and never closed).
        try:
            with open(SESSION_PATH, 'r') as f:
                on_disk = json.load(f)
            if on_disk.get('PlaySessionId', '') == session['PlaySessionId']:
                os.remove(SESSION_PATH)
        except (OSError, json.JSONDecodeError):
            pass

        xbmc.log('pvr.kofin reporter: playback stopped', xbmc.LOGINFO)
        self._finalize(session, is_recording, final_position_ticks)

    def _finalize(self, session, is_recording, position_ticks):
        """Report Stopped and close the live stream for a finished session."""
        stopped_body = {
            'ItemId': session['ItemId'],
            'MediaSourceId': session['MediaSourceId'],
            'PlaySessionId': session['PlaySessionId'],
        }
        if is_recording:
            # Without an explicit position the server's stop handler infers
            # one — and infers wrong when seeks have shifted the playhead
            # off the wall-clock elapsed value seen in /Progress events.
            stopped_body['PositionTicks'] = position_ticks
        self._send_with(session, '/Sessions/Playing/Stopped', stopped_body)

        # Close the live stream if one was opened. liveStreamId MUST ride as
        # a query parameter: Jellyfin's LiveStreams/Close binds it from the
        # query string only and answers a JSON/form body with HTTP 400 — the
        # stream then stays open and the tuner's consumer count never drops.
        live_stream_id = session.get('LiveStreamId', '')
        if live_stream_id:
            self._send_with(
                session,
                '/LiveStreams/Close?liveStreamId='
                + urllib.parse.quote(live_stream_id, safe=''),
                {})

    def report_progress(self):
        """Called from main loop. Sends progress if session is active."""
        if not self.session:
            return
        self._send('/Sessions/Playing/Progress', self._build_body())

    def _send_sync_claim(self):
        """Tell a kofin-hosted SyncPlay engine what is on screen.

        The public provider contract (plugin.video.kofin,
        docs/syncplay-provider-contract.md): recordings and live channels are
        Jellyfin items, so those claims name provider "jellyfin" and a group
        follower plays the same id through kofin's ordinary route. A live
        channel sends no runtime — a zero-runtime claim is the contract's
        spelling of "live". A catchup play (a live-TV playback whose
        programme has already ended) is nobody's Jellyfin item: it claims
        under this add-on's own name with the programme identity as the key
        (channel GUID @ programme start, epoch seconds). A live or catchup
        play that runs through inputstream.tempo (the Inputstream tab's
        choice) adds the tempo route — the add-on polls its shared tempo
        file when the stream names none — so the engine's fine sync can
        pulse this member: on the source clock the add-on reports for a
        live channel, on the programme for catchup. Sent from here rather
        than C++ because
        executeJSONRPC lands on this Kodi's own bus, which no localhost
        socket can promise on a host running two Kodis. Fire-and-forget:
        with no kofin engine listening the notification costs nothing, and
        the engine drops the claim itself when playback stops.
        """
        data = {
            'v': 1,
            'provider': 'jellyfin',
            'key': self.session['ItemId'],
            'play_method': self.session['PlayMethod'] or 'DirectPlay',
            'play_session': self.session['PlaySessionId'],
        }
        if self.is_recording:
            try:
                data['name'] = self.getVideoInfoTag().getTitle()
                data['runtime_ticks'] = int(self.getTotalTime() * 10_000_000)
            except RuntimeError:
                pass  # player already tearing down; the claim still identifies
        else:
            programme = self._playing_programme()
            if programme and programme['end'] < time.time() - CATCHUP_GRACE_SECS:
                data['provider'] = SYNC_PROVIDER
                data['key'] = '%s@%d' % (self.session['ItemId'], programme['start'])
                data['name'] = programme['title']
                data['runtime_ticks'] = int(
                    (programme['end'] - programme['start']) * 10_000_000)
            route = self._tempo_route()
            if route:
                data['tempo'] = route
        xbmc.executeJSONRPC(json.dumps({
            'jsonrpc': '2.0', 'id': 1, 'method': 'JSONRPC.NotifyAll',
            'params': {'sender': ADDON_ID,
                       'message': 'SyncProvider.Claim',
                       'data': data}}))
        xbmc.log('pvr.kofin reporter: sync claim sent (%s)' % data['provider'],
                 xbmc.LOGDEBUG)

    def _tempo_route(self):
        """The fine-sync route to claim, or None.

        Only when this playback really runs through inputstream.tempo: the
        C++ side chooses the inputstream (the Inputstream tab), and what
        tells the two apart from here is the add-on's state line for the
        shared file, written at the pipeline's anchor — so a line older
        than this stream's session cut (WrittenAt, stamped by the C++ side
        as it resolves the URL) belongs to some earlier play. A route the
        pulses could never reach would arm the engine and fail its first
        pulse on every item. The queue depth is Kodi's own (Kodi 22's
        setting; fixed at 8 s on Kodi 21), which a kofin service shortens
        for the session — the engine measures a queue depth after every
        pulse, so it has to be the real one.
        """
        state_path = xbmcvfs.translatePath(TEMPO_FILE) + '.state'
        try:
            written = os.path.getmtime(state_path)
        except OSError:
            return None
        since = self.session.get('WrittenAt') or (self.start_time - 15)
        if written < since - 1:
            return None
        queue = rpc('Settings.GetSettingValue',
                    {'setting': 'videoplayer.queuetimesize'}).get('value')
        try:
            queue_secs = int(queue) / 10.0 if queue else TEMPO_QUEUE_SECS_DEFAULT
        except (TypeError, ValueError):
            queue_secs = TEMPO_QUEUE_SECS_DEFAULT
        return {
            'file': xbmcvfs.translatePath(TEMPO_FILE),
            'queue_secs': queue_secs,
            'manifest_type': 'hls',
        }

    def _playing_programme(self):
        """{'title', 'start', 'end'} of the programme on screen, or None.

        Player.GetItem carries the EPG tag's title and times for both live
        and catchup playback of a channel; the times arrive as local-time
        strings.
        """
        try:
            result = json.loads(xbmc.executeJSONRPC(json.dumps({
                'jsonrpc': '2.0', 'id': 1, 'method': 'Player.GetItem',
                'params': {'playerid': 1,
                           'properties': ['title', 'starttime', 'endtime']},
            }))).get('result', {}).get('item', {})
            start = time.mktime(time.strptime(
                result['starttime'], '%Y-%m-%d %H:%M:%S'))
            end = time.mktime(time.strptime(
                result['endtime'], '%Y-%m-%d %H:%M:%S'))
        except (KeyError, ValueError, OSError):
            return None
        return {'title': result.get('title', ''), 'start': int(start),
                'end': int(end)}

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


def rpc(method, params):
    """One local JSON-RPC call; {} on any failure."""
    try:
        reply = json.loads(xbmc.executeJSONRPC(json.dumps(
            {'jsonrpc': '2.0', 'id': 1, 'method': method, 'params': params})))
        return reply.get('result') or {}
    except (ValueError, TypeError):
        return {}


def register_sync_provider():
    """Register with a kofin-hosted SyncPlay engine as a delegated-start
    provider: catchup content has no URL a template could carry (an EPG tag
    is tuned, not fetched), so the engine broadcasts SyncSession.Start and
    this service executes it (the provider contract's delegated start)."""
    xbmc.executeJSONRPC(json.dumps({
        'jsonrpc': '2.0', 'id': 1, 'method': 'JSONRPC.NotifyAll',
        'params': {'sender': ADDON_ID,
                   'message': 'SyncProvider.Register',
                   'data': {'v': 1, 'provider': SYNC_PROVIDER,
                            'play': {'delegated': True}}}}))
    xbmc.log('pvr.kofin reporter: sync provider registered', xbmc.LOGINFO)


def execute_sync_start(key):
    """Tune this Kodi to the programme a SyncSession.Start names.

    The key is `<channel jellyfin id>@<programme start, epoch seconds>`.
    The channel resolves through the server (the GUID is nowhere in Kodi's
    JSON-RPC view of PVR), then the local EPG names the broadcast: times in
    PVR.GetBroadcasts are local-time strings, so the epoch converts through
    localtime. Failing quietly is right — the engine's load watchdog gives
    playback back to the member if nothing starts.
    """
    try:
        channel_guid, _, start_raw = key.partition('@')
        start_local = time.strftime('%Y-%m-%d %H:%M:%S',
                                    time.localtime(int(start_raw)))
    except ValueError:
        xbmc.log('pvr.kofin reporter: bad sync start key %r' % key,
                 xbmc.LOGWARNING)
        return

    base = normalize_base_url(get_setting('jellyfinServerAddress'))
    token = get_setting('jellyfinAccessToken')
    device_id = get_setting('deviceId')
    user_id = get_setting('jellyfinUserId')
    name = None
    try:
        req = urllib.request.Request(
            base + '/LiveTv/Channels?userId=' + user_id,
            headers={'Authorization': build_auth_header(token, device_id)})
        with urllib.request.urlopen(req, timeout=10,
                                    context=ssl_context()) as resp:
            doc = json.load(resp)
        name = next((c.get('Name') for c in doc.get('Items', [])
                     if c.get('Id') == channel_guid), None)
    except (urllib.error.URLError, OSError, ValueError):
        pass
    if not name:
        xbmc.log('pvr.kofin reporter: sync start channel %s not found'
                 % channel_guid, xbmc.LOGWARNING)
        return

    channels = rpc('PVR.GetChannels',
                   {'channelgroupid': 'alltv'}).get('channels', [])
    channel = next((c for c in channels if c.get('label') == name), None)
    if not channel:
        xbmc.log('pvr.kofin reporter: no local channel named %r' % name,
                 xbmc.LOGWARNING)
        return

    broadcasts = rpc('PVR.GetBroadcasts',
                     {'channelid': channel['channelid'],
                      'properties': ['starttime']}).get('broadcasts', [])
    broadcast = next((b for b in broadcasts
                      if b.get('starttime') == start_local), None)
    if not broadcast:
        xbmc.log('pvr.kofin reporter: no broadcast at %s on %r'
                 % (start_local, name), xbmc.LOGWARNING)
        return

    xbmc.log('pvr.kofin reporter: sync start -> broadcast %s (%r at %s)'
             % (broadcast['broadcastid'], name, start_local), xbmc.LOGINFO)
    rpc('Player.Open', {'item': {'broadcastid': broadcast['broadcastid']}})


class SyncMonitor(xbmc.Monitor):
    """The provider contract's inbound side: re-register on the engine's
    announce, and execute delegated starts addressed to this provider."""

    def onNotification(self, sender, method, data):
        if sender == ADDON_ID:
            return  # our own outbound messages echo back

        if method == 'Other.SyncSession.State':
            register_sync_provider()
            return

        if method == 'Other.SyncSession.Start':
            try:
                payload = json.loads(data)
                if isinstance(payload, list):
                    payload = payload[0]
            except (ValueError, IndexError):
                return
            if payload.get('provider') != SYNC_PROVIDER:
                return
            execute_sync_start(str(payload.get('key') or ''))


if __name__ == '__main__':
    monitor = SyncMonitor()
    player = PlaybackReporter()
    xbmc.log('pvr.kofin reporter: started', xbmc.LOGINFO)
    register_sync_provider()

    while not monitor.abortRequested():
        if monitor.waitForAbort(REPORT_INTERVAL):
            break
        player.report_progress()

    # Clean up on exit — report stopped if still playing
    if player.session:
        player._stop()

    xbmc.log('pvr.kofin reporter: stopped', xbmc.LOGINFO)
