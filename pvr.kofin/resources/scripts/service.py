#  Copyright (C) 2026 Kofin
#
#  SPDX-License-Identifier: GPL-2.0-or-later
#  See LICENSE.md for more information.

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
import calendar
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
# The provider name tuned content claims under (the public provider
# contract): live channels and catchup programmes alike, because both are
# *tuned, not fetched* — a follower reaches them by opening this add-on's
# PVR channel, not by fetching a URL. Recordings stay provider "jellyfin":
# they are ordinary Jellyfin items a follower can play by id.
SYNC_PROVIDER = 'pvr.kofin'
# A programme whose end is at least this far gone is catchup, not live —
# covers EPG clock skew around a live programme's final minute.
CATCHUP_GRACE_SECS = 60
# The tempo file the C++ side stamps on every stream it routes through
# inputstream.tempo (AppendTempoProperties): the add-on arms its pipeline,
# and writes the state line the engine reads its clock from, only for a
# stream that names one. The claim names the same file.
TEMPO_FILE = 'special://temp/inputstream_tempo.pvr.kofin'
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
# One record per live stream the C++ addon opened that no playback has adopted
# yet. The addon only writes them; this script deletes each once it has
# adopted or closed that stream.
PENDING_DIR = (os.path.join(xbmcvfs.translatePath(_profile), 'pending')
               if _profile else '')
# How long a pending stream may sit with the player idle before it is closed.
# isPlaying() is already true while Kodi is still opening a stream, so an idle
# player this long after the tune means the open failed or was abandoned.
PENDING_IDLE_SECS = 30
# A close the server did not accept leaves the record for a retry, first after
# a minute and then at doubling intervals up to an hour. Jellyfin answers 204
# for a stream it no longer holds, so once the server is back any retry
# clears the record. A day on, it is dropped: the server is gone for good, or
# the credentials no longer reach it.
PENDING_RETRY_SECS = 60
PENDING_RETRY_MAX_SECS = 3600
PENDING_GIVE_UP_SECS = 24 * 3600
# The stream property naming the PlaySessionId of the tune a playback came
# from. The addon sets it on every stream it hands Kodi, and Kodi copies stream
# properties onto the item it plays, so it tells a kofin playback from another
# PVR addon's and from a kofin tune that never played.
SESSION_TAG = 'pvr.kofin.playsessionid'


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
    """POST JSON to Jellyfin. Errors are logged, not raised; returns whether
    the server accepted the request."""
    base = normalize_base_url(base_url)
    if not base:
        return False
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
        return True
    except (urllib.error.URLError, OSError) as e:
        xbmc.log(f'pvr.kofin reporter: POST {endpoint} failed: {e}', xbmc.LOGDEBUG)
        return False


def remove_file(path):
    try:
        os.remove(path)
    except OSError:
        pass


def pending_sessions():
    """The pending stream records, as (path, session) pairs."""
    if not PENDING_DIR:
        return []
    try:
        names = os.listdir(PENDING_DIR)
    except OSError:
        return []
    found = []
    for name in names:
        # The addon writes each record under a .tmp name and renames it into
        # place, so a .json file is always complete.
        if not name.endswith('.json'):
            continue
        path = os.path.join(PENDING_DIR, name)
        try:
            with open(path, 'r') as f:
                found.append((path, json.load(f)))
        except (OSError, json.JSONDecodeError):
            continue
    return found


class PlaybackReporter(xbmc.Player):
    def __init__(self):
        super().__init__()
        self.session = None
        self.paused = False
        self.start_time = None
        self.is_recording = False
        self.last_position_ticks = 0
        # Pending records whose close failed: path -> (retry at, last delay).
        self.pending_retry = {}

    def onAVStarted(self):
        """Stream is up — adopt it if it is kofin's, and report the start."""
        session_data = self._playing_session()

        # A new playback can replace ours without any stop event (seamless
        # channel zap) — finalize the outgoing session first, or its live
        # stream is never closed on the server. Same PlaySessionId means a
        # duplicate start event for the current stream: keep it. A stream
        # whose playback never got this far is left to the pending records.
        if self.session:
            new_psid = session_data.get('PlaySessionId', '') if session_data else None
            if new_psid != self.session['PlaySessionId']:
                xbmc.log('pvr.kofin reporter: playback replaced without stop event',
                         xbmc.LOGDEBUG)
                self._finalize(self.session, self.is_recording,
                               self.last_position_ticks)
                self.session = None

        if session_data and not self.session:
            self._adopt(session_data)

    def adopt_playing(self):
        """Adopt a kofin stream that was already playing when this player was
        created, such as one Kodi resumed at startup: its onAVStarted went to
        no one."""
        if self.session or not self.isPlaying():
            return
        session_data = self._playing_session()
        if session_data:
            xbmc.log('pvr.kofin reporter: adopting playback already under way',
                     xbmc.LOGDEBUG)
            self._adopt(session_data)

    def _playing_session(self):
        """The session.json record of the item now playing, or None when that
        item is not a kofin stream. Player callbacks fire for all playback, and
        session.json outlives its own playback when a tune never starts, so the
        file counts only when the playing item carries its PlaySessionId."""
        try:
            tag = self.getPlayingItem().getProperty(SESSION_TAG)
        except RuntimeError:
            return None
        if not tag:
            return None
        try:
            with open(SESSION_PATH, 'r') as f:
                session_data = json.load(f)
        except (OSError, json.JSONDecodeError):
            return None
        if session_data.get('PlaySessionId', '') != tag or not session_data.get('ItemId', ''):
            return None
        return session_data

    def _adopt(self, session_data):
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
        xbmc.log(f'pvr.kofin reporter: playback started ({content})', xbmc.LOGDEBUG)

        self._send('/Sessions/Playing', self._build_body())
        self._send_sync_claim()
        self._settle_pending(session_data)

    def _settle_pending(self, adopted):
        """Take the adopted stream off the pending list, and close the streams
        it replaced. The player plays one stream at a time, so one opened before
        the stream now playing was zapped away from before it started. Those
        opened after it are left alone: the next tune may already be under way.
        """
        adopted_psid = adopted.get('PlaySessionId', '')
        adopted_at = adopted.get('WrittenAtMs', 0)
        for path, pending in pending_sessions():
            if pending.get('PlaySessionId', '') == adopted_psid:
                remove_file(path)
            elif pending.get('WrittenAtMs', 0) < adopted_at:
                self._close_pending(path, pending)

    def sweep_pending(self):
        """Close the pending streams whose playback never started. Only while
        the player is idle: while something plays, it may be a kofin stream
        this script did not adopt (one whose session.json could not be read,
        say), and closing its stream would cut it off."""
        if self.isPlaying():
            return
        now = time.time()
        for path, pending in pending_sessions():
            if now * 1000 - pending.get('WrittenAtMs', 0) <= PENDING_IDLE_SECS * 1000:
                continue
            if now < self.pending_retry.get(path, (0, 0))[0]:
                continue
            self._close_pending(path, pending)

    def close_all_pending(self):
        """At shutdown nothing pending can still start. A close that fails
        keeps its record for the next run."""
        for path, pending in pending_sessions():
            self._close_pending(path, pending)

    def _close_pending(self, path, pending):
        """Close one pending stream. The record is the only trace that the
        stream still needs closing, since session.json has moved on, so it is
        deleted only once the server has accepted the close."""
        xbmc.log('pvr.kofin reporter: closing a stream that never started playing',
                 xbmc.LOGDEBUG)
        # The server the stream was opened on, which the settings may no longer
        # name. Records from before it was stored fall back to the setting.
        closed = self._close_live_stream({
            'LiveStreamId': pending.get('LiveStreamId', ''),
            'BaseUrl': pending.get('BaseUrl') or get_setting('jellyfinServerAddress'),
            'Token': get_setting('jellyfinAccessToken'),
            'DeviceId': get_setting('deviceId'),
        })
        if closed:
            remove_file(path)
            self.pending_retry.pop(path, None)
            return
        if time.time() * 1000 - pending.get('WrittenAtMs', 0) > PENDING_GIVE_UP_SECS * 1000:
            xbmc.log('pvr.kofin reporter: giving up on closing a stream opened a day ago',
                     xbmc.LOGDEBUG)
            remove_file(path)
            self.pending_retry.pop(path, None)
            return
        previous = self.pending_retry.get(path)
        delay = min(previous[1] * 2, PENDING_RETRY_MAX_SECS) if previous else PENDING_RETRY_SECS
        self.pending_retry[path] = (time.time() + delay, delay)

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

        xbmc.log('pvr.kofin reporter: playback stopped', xbmc.LOGDEBUG)
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
        self._close_live_stream(session)

    def _close_live_stream(self, session):
        """Close the live stream if one was opened. liveStreamId MUST ride as
        a query parameter: Jellyfin's LiveStreams/Close binds it from the
        query string only and answers a JSON/form body with HTTP 400 — the
        stream then stays open and the tuner's consumer count never drops.
        Returns whether there was nothing to close or the server accepted it."""
        live_stream_id = session.get('LiveStreamId', '')
        if not live_stream_id:
            return True
        return self._send_with(
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
        docs/syncplay-provider-contract.md): a recording is an ordinary
        Jellyfin item, so its claim names provider "jellyfin" and a group
        follower plays the same id through kofin's ordinary route.

        A **live channel** claims under this add-on's own name instead, with
        the channel id as the key and no runtime — a zero-runtime claim is
        the contract's spelling of "live". Naming jellyfin there was wrong:
        the engine would build `plugin://plugin.video.kofin/?mode=play&id=`
        for the follower, which resolves the Jellyfin live stream as a plain
        plugin item — no PVR channel, so no EPG, no live OSD, no timeshift
        bar, while the member who pressed play watches `pvr://`. The
        pipelines have to be the same on every member, so live is delegated
        like catchup and the follower tunes this add-on's channel.

        A catchup play (a live-TV playback whose programme has already
        ended) claims under the same name with the programme identity as the key
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
            # Live and catchup are both delegated (see the docstring); the
            # key is what tells them apart, and no runtime means live.
            data['provider'] = SYNC_PROVIDER
            programme = self._playing_programme()
            if programme and programme['end'] < time.time() - CATCHUP_GRACE_SECS:
                data['key'] = '%s@%d' % (self.session['ItemId'], programme['start'])
                data['name'] = programme['title']
                data['runtime_ticks'] = int(
                    (programme['end'] - programme['start']) * 10_000_000)
            elif programme:
                data['name'] = programme['title']
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
        add-on's file, written at the pipeline's anchor — so a line older
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
        and catchup playback of a channel. **The times are UTC**, whatever
        the box's timezone: reading them as local time shifted every
        programme by the UTC offset, so under an hour of DST the last hour
        of a live programme read as already ended and a live play at the
        edge claimed catchup (observed on the rig, 2026-09-02: a 13:30–20:00
        local programme parsed as ending 18:00 UTC and claimed catchup from
        19:03 onwards).
        """
        try:
            result = json.loads(xbmc.executeJSONRPC(json.dumps({
                'jsonrpc': '2.0', 'id': 1, 'method': 'Player.GetItem',
                'params': {'playerid': 1,
                           'properties': ['title', 'starttime', 'endtime']},
            }))).get('result', {}).get('item', {})
            start = calendar.timegm(time.strptime(
                result['starttime'], '%Y-%m-%d %H:%M:%S'))
            end = calendar.timegm(time.strptime(
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
            return False
        return post_json(base_url, endpoint, body, token, device_id)


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
    xbmc.log('pvr.kofin reporter: sync provider registered', xbmc.LOGDEBUG)


def _resolve_local_channel(channel_guid):
    """The local Kodi PVR channel for a Jellyfin channel id, or None.

    The GUID is nowhere in Kodi's JSON-RPC view of PVR, so it resolves
    through the server to a channel name and then by name locally.
    """
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
                 % channel_guid, xbmc.LOGDEBUG)
        return None, None

    channels = rpc('PVR.GetChannels',
                   {'channelgroupid': 'alltv'}).get('channels', [])
    channel = next((c for c in channels if c.get('label') == name), None)
    if not channel:
        xbmc.log('pvr.kofin reporter: no local channel named %r' % name,
                 xbmc.LOGDEBUG)
        return None, name
    return channel, name


def execute_sync_start(key):
    """Tune this Kodi to what a SyncSession.Start names.

    Two kinds of key, and the difference is the whole point of this
    executor: `<channel jellyfin id>` alone is the **live** channel, and
    `<channel jellyfin id>@<programme start, epoch seconds>` is a catchup
    programme. Both open through this add-on's own PVR channel, so every
    member of the group runs the same pipeline as the one who pressed play
    — the same `pvr://` item, the same EPG, the same live OSD.

    Programme times in PVR.GetBroadcasts are UTC strings, so the epoch
    converts back through gmtime (the claim side reads them the same way).

    Failing quietly is right — the engine's load watchdog gives playback
    back to the member if nothing starts.
    """
    channel_guid, sep, start_raw = key.partition('@')

    if not sep:
        # Live: tune the channel itself, at its own live edge.
        channel, name = _resolve_local_channel(channel_guid)
        if not channel:
            return
        xbmc.log('pvr.kofin reporter: sync start -> live channel %s (%r)'
                 % (channel['channelid'], name), xbmc.LOGDEBUG)
        rpc('Player.Open', {'item': {'channelid': channel['channelid']}})
        return

    try:
        start_utc = time.strftime('%Y-%m-%d %H:%M:%S',
                                  time.gmtime(int(start_raw)))
    except ValueError:
        xbmc.log('pvr.kofin reporter: bad sync start key %r' % key,
                 xbmc.LOGDEBUG)
        return

    channel, name = _resolve_local_channel(channel_guid)
    if not channel:
        return

    broadcasts = rpc('PVR.GetBroadcasts',
                     {'channelid': channel['channelid'],
                      'properties': ['starttime']}).get('broadcasts', [])
    broadcast = next((b for b in broadcasts
                      if b.get('starttime') == start_utc), None)
    if not broadcast:
        xbmc.log('pvr.kofin reporter: no broadcast at %s on %r'
                 % (start_utc, name), xbmc.LOGDEBUG)
        return

    xbmc.log('pvr.kofin reporter: sync start -> broadcast %s (%r at %s)'
             % (broadcast['broadcastid'], name, start_utc), xbmc.LOGDEBUG)
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
    xbmc.log('pvr.kofin reporter: started', xbmc.LOGDEBUG)
    register_sync_provider()
    player.adopt_playing()

    while not monitor.abortRequested():
        if monitor.waitForAbort(REPORT_INTERVAL):
            break
        player.report_progress()
        player.sweep_pending()

    # Clean up on exit — report stopped if still playing
    if player.session:
        player._stop()
    player.close_all_pending()

    xbmc.log('pvr.kofin reporter: stopped', xbmc.LOGDEBUG)
