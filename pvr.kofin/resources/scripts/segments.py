#  Copyright (C) 2026 Kofin
#
#  SPDX-License-Identifier: GPL-2.0-or-later
#  See LICENSE.md for more information.

"""Segment skipping for recordings.

A port of plugin.video.kofin's segment engine (``service/segments.py`` and
``service/skip.py``) for the media segment types Jellyfin knows (Intro,
Outro, Recap, Preview, Commercial), each with its own mode, and extended
for a recording still being written: the Comseg server plugin publishes
commercial breaks while the file grows, and a break's end can move until
the programme is back.

Each segment has one owner. **Auto** commercial breaks in a finished
recording are Kodi's: the C++ add-on hands them over as an EDL when
playback opens, Kodi skips them itself, and this engine leaves them alone.
Every other **Auto** segment is this engine's: it seeks past it, and in a
recording still being written it re-fetches the segments while the
recording plays and seeks again when one's end has grown past where it
landed. **Ask** is this engine's for every type on every recording: a skip
button for each segment, offered again when its end grows.
Positions are the player's own clock, which for a timeshifted stream counts
from the recording's start, the same timeline the server's segment ticks
use.

The tick is 0.25 s on float positions: it samples the position, acts on the
break that contains it, and never touches the network. A fetcher thread
refreshes the segment list and the tick only reads it, so a slow request
misses ticks instead of freezing them. A settle window after our own seek
keeps a lagging ``getTime()`` from re-triggering the segment just skipped;
the overlay's lifetime is tick-driven, and its button callbacks run on
Kodi's GUI thread.
"""

import threading

import xbmc
import xbmcaddon
import xbmcgui

ADDON_ID = 'pvr.kofin'

TICK_SECONDS = 0.25
# ~3 s of ticks: how long a lagging getTime() may keep reporting the pre-seek
# position after our own skip seek before the seek is re-issued.
SEEK_SETTLE_TICKS = 12
SEEK_RETRIES = 6
# A skip prompt stays up at least this long, so it can be read and pressed.
MIN_PROMPT_SECONDS = 8.0
# Segments too short to act on: a prompt that flashes is worse than none,
# and a sub-second seek is indistinguishable from a stutter.
MIN_ASK_SECONDS = 3.0
MIN_AUTO_SKIP_SECONDS = 1.0
# Two segments of a type are the same one when their starts are this close;
# the server replaces a segment's row when its end moves, so starts are the
# identity.
START_TOLERANCE = 1.5
# Never seek this close to the end of a stream still being written: the
# target may not have been remuxed yet, and Kodi treats a seek at the end of
# the stream as the end of playback.
LIVE_EDGE_MARGIN = 3.0
# A segment already handled is handled again only once at least this much more
# of it can be skipped: the live edge of a growing stream creeps along, and
# chasing it half a second at a time would be churn, not skipping.
REENTER_MIN_SECONDS = 5.0
# A seek landing within this of its target has settled; a seek held back at
# the live edge lands wherever the stream currently ends, so it gets the
# margin itself as its tolerance.
SETTLE_TOLERANCE = 0.5

MODE_OFF = 0
MODE_AUTO = 1
MODE_ASK = 2

# Jellyfin's MediaSegmentType names, each with the setting that holds its
# mode (plugin.video.kofin's ids) and that setting's default.
MODE_SETTINGS = {
    'Intro': ('skipIntroductionMode', MODE_OFF),
    'Outro': ('skipCreditsMode', MODE_OFF),
    'Recap': ('skipRecapMode', MODE_OFF),
    'Preview': ('skipPreviewMode', MODE_OFF),
    'Commercial': ('skipCommercialMode', MODE_ASK),
}

XML_FILENAME = 'pvrkofin-skip.xml'
SKIP_BUTTON = 3012
CLOSE_BUTTON = 3013
ACTION_SELECT_ITEM = 7
ACTION_PARENT_DIR = 9
ACTION_PREVIOUS_MENU = 10
ACTION_NAV_BACK = 92

STRING_CLOSE = 30879
# The skip button and the toast after a skip, per segment type.
SKIP_LABELS = {'Intro': 30889, 'Outro': 30890, 'Recap': 30891, 'Preview': 30892,
               'Commercial': 30878}
SKIPPED_TOASTS = {'Intro': 30893, 'Outro': 30894, 'Recap': 30895, 'Preview': 30896,
                  'Commercial': 30880}


# -- pure helpers --------------------------------------------------------------


def parse_segments(doc, types):
    """Sorted ``[(start, end, type)]`` in seconds from a /MediaSegments body,
    for the segment types in ``types``."""
    out = []
    for item in (doc or {}).get('Items') or []:
        segment_type = item.get('Type')
        if segment_type not in types:
            continue
        try:
            start = float(item.get('StartTicks') or 0) / 10_000_000
            end = float(item.get('EndTicks') or 0) / 10_000_000
        except (TypeError, ValueError):
            continue
        if end > start:
            out.append((start, end, segment_type))
    out.sort()
    return out


def safe_seek_end(end, total, now, live):
    """Where a skip lands: the segment's end, held back from the end of a
    growing stream, or None when the seek would go nowhere. A stream whose
    length is not known yet is not seekable: that unknown is what the live
    margin exists to refuse."""
    if total <= 0:
        return None
    target = min(max(0.0, float(end)), total - (LIVE_EDGE_MARGIN if live else 1.0))
    if now is not None and target <= now + 0.5:
        return None
    return target


def prompt_hide_at(start, end, percent, now, minimum=MIN_PROMPT_SECONDS):
    """When a prompt hides: a share of the segment, never sooner than the
    reading floor measured from ``now`` (a late-entered segment still gets
    its full dwell), never past the segment's end."""
    deadline = start + max(0.0, end - start) * (percent / 100.0)
    floor = max(start, now) + minimum
    return min(end, max(deadline, floor))


def format_span(seconds):
    """``M:SS`` for a span, as the skip button renders it."""
    whole = max(0, int(round(seconds)))
    return '%d:%02d' % (whole // 60, whole % 60)


# -- the overlay ---------------------------------------------------------------


class SkipOverlay(xbmcgui.WindowXMLDialog):
    """Skip <type> / Close over the video. Non-modal: ``show()`` returns at once
    and the engine's tick owns the lifetime; the button callback runs on
    Kodi's GUI thread."""

    def __init__(self, *args, **kwargs):
        self._on_skip = kwargs.pop('on_skip', None)
        skip_label = kwargs.pop('skip_label', '')
        skip_duration = kwargs.pop('skip_duration', '')
        close_label = kwargs.pop('close_label', '')
        self.closed = False
        super().__init__(*args)
        self.setProperty('pvrkofin.skip.label', skip_label)
        # The skin appends this in brackets, so an empty value renders nothing.
        self.setProperty('pvrkofin.skip.duration', skip_duration)
        self.setProperty('pvrkofin.close.label', close_label)

    def close(self):
        self.closed = True
        try:
            super().close()
        except Exception:  # window already torn down with the player
            pass

    def onAction(self, action):
        action_id = action.getId()
        if action_id in (ACTION_PARENT_DIR, ACTION_PREVIOUS_MENU, ACTION_NAV_BACK):
            self.close()
        elif action_id == ACTION_SELECT_ITEM:
            # Over fullscreen video the focus sits on the grouplist, so a
            # button press arrives as an action rather than onClick.
            self.onClick(self.getFocusId())

    def onClick(self, control_id):
        if self.closed:
            return  # idempotent: a select routed via onAction must not re-run
        if control_id == SKIP_BUTTON and self._on_skip is not None:
            try:
                self._on_skip()
            except Exception as error:
                xbmc.log('pvr.kofin segments: skip failed: %s' % error, xbmc.LOGWARNING)
        if control_id in (SKIP_BUTTON, CLOSE_BUTTON):
            self.close()


def open_overlay(skip_label, skip_duration, close_label, on_skip):
    addon_path = xbmcaddon.Addon(ADDON_ID).getAddonInfo('path')
    overlay = SkipOverlay(XML_FILENAME, addon_path, 'default', '1080i',
                          skip_label=skip_label, skip_duration=skip_duration,
                          close_label=close_label, on_skip=on_skip)
    overlay.show()
    return overlay


# -- the fetcher ---------------------------------------------------------------


class SegmentFetcher(threading.Thread):
    """Fetches the segment list off the tick thread: once for a finished
    recording (one retry), every ``interval`` seconds for one still being
    written. A daemon, so a request in flight when the engine stops is
    abandoned rather than waited for; it never touches the player."""

    def __init__(self, fetch, interval, repeat, halt):
        super().__init__(name='pvr.kofin-segments-fetch', daemon=True)
        self._fetch = fetch
        self._interval = interval
        self._repeat = repeat
        self._halt = halt
        self._lock = threading.Lock()
        self._latest = None

    def take(self):
        """The newest segment list since the last call, or None."""
        with self._lock:
            latest, self._latest = self._latest, None
        return latest

    def run(self):
        attempts = 0
        while not self._halt.is_set():
            attempts += 1
            try:
                fetched = self._fetch()
            except Exception as error:
                xbmc.log('pvr.kofin segments: fetch failed (try %d): %s' % (attempts, error),
                         xbmc.LOGWARNING)
                if not self._repeat and attempts >= 2:
                    return
                if self._halt.wait(1.0 if attempts < 2 else self._interval):
                    return
                continue
            with self._lock:
                self._latest = fetched
            if not self._repeat or self._halt.wait(self._interval):
                return


# -- the engine ----------------------------------------------------------------


class SegmentSkipper(threading.Thread):
    """Drives the player's segment tick while a recording plays.

    ``player`` is the ``xbmc.Player`` that owns the playback (``getTime``,
    ``getTotalTime``, ``seekTime``); ``modes`` maps each segment type this
    engine owns to MODE_AUTO or MODE_ASK; ``fetch`` returns the current
    ``[(start, end, type)]`` from the server or raises; ``localized`` and
    ``notify`` are the add-on's string lookup and toast. ``in_progress``
    says the recording is still being written: the segments are re-fetched
    every ``poll_seconds`` and seeks stay off the live edge.
    """

    def __init__(self, player, modes, hide_percent, poll_seconds, in_progress,
                 fetch, localized, notify):
        super().__init__(name='pvr.kofin-segments')
        self._player = player
        self._modes = dict(modes)
        self._hide_percent = max(10.0, min(100.0, float(hide_percent or 50)))
        self._in_progress = in_progress
        self._localized = localized
        self._notify = notify
        self._halt = threading.Event()
        self._fetcher = SegmentFetcher(fetch, max(5.0, float(poll_seconds or 10)),
                                     in_progress, self._halt)
        self._segments = []          # [(start, end, type)], sorted
        # (type, start) -> where the segment was handled to: the position our
        # skip seeked to, or the end a prompt offered. A segment whose end
        # has grown past that is handled again.
        self._acted = {}
        self._settle_target = None
        self._settle_tolerance = SETTLE_TOLERANCE
        self._settle_ticks = 0
        self._settle_retries = 0
        self._pending_notify = None  # the segment type whose skip to announce
        self._pending_jump = False
        self._overlay = None
        self._overlay_type = None
        self._overlay_start = None
        self._overlay_end = None
        self._overlay_hide_at = 0.0
        self._overlay_window = None

    # -- lifecycle ---------------------------------------------------------

    def stop(self):
        """Ends the tick; it never blocks on the network, so the join is a
        tick long. The fetcher is left to its own timeout."""
        self._halt.set()
        if self.is_alive() and threading.current_thread() is not self:
            self.join(timeout=5)
        self._close_overlay()

    def note_seek(self, target):
        """Player-seek hook: user seeks re-arm the engine; the echo of our
        own skip seek is ignored."""
        settle = self._settle_target
        if settle is not None and abs(target - settle) < 2.0 + self._settle_tolerance:
            return
        self._settle_target = None
        self._pending_jump = True

    def run(self):
        xbmc.log('pvr.kofin segments: engine started (modes %s, in progress %s)'
                 % (self._modes, self._in_progress), xbmc.LOGDEBUG)
        monitor = xbmc.Monitor()
        self._fetcher.start()
        while not self._halt.is_set() and not monitor.abortRequested():
            try:
                self._tick()
            except Exception as error:
                xbmc.log('pvr.kofin segments: tick failed: %s' % error, xbmc.LOGWARNING)
            # Kodi-aware wait: while the overlay is open the engine must yield
            # to Kodi between ticks so the window's callbacks are serviced.
            if monitor.waitForAbort(TICK_SECONDS):
                break
        self._halt.set()  # on abort too, so the fetcher stops waiting
        self._close_overlay()
        xbmc.log('pvr.kofin segments: engine stopped', xbmc.LOGDEBUG)

    # -- the tick -----------------------------------------------------------

    def _tick(self):
        try:
            now = float(self._player.getTime())
        except RuntimeError:  # nothing playing yet / race with stop
            return

        self._drive_overlay(now)
        fetched = self._fetcher.take()
        if fetched is not None and fetched != self._segments:
            xbmc.log('pvr.kofin segments: now %d segments' % len(fetched), xbmc.LOGDEBUG)
            self._segments = fetched

        if self._settle_target is not None:
            # Post-seek settle: a lagging getTime() must not re-trigger the
            # segment we just skipped.
            self._settle_ticks -= 1
            if now >= self._settle_target - self._settle_tolerance:
                if self._pending_notify is not None:
                    self._notify(self._localized(SKIPPED_TOASTS[self._pending_notify]))
                    self._pending_notify = None
                self._settle_target = None
                self._resync(now)
            elif self._settle_ticks <= 0:
                if self._settle_retries > 0:
                    self._settle_retries -= 1
                    self._settle_ticks = SEEK_SETTLE_TICKS
                    self._player.seekTime(self._settle_target)
                else:
                    self._pending_notify = None
                    self._settle_target = None
                    self._resync(now)
            return

        if self._pending_jump:
            self._pending_jump = False
            self._resync(now)

        self._check(now)

    def _resync(self, now):
        """After a position jump only segments still containing ``now`` stay
        handled, so a seek back re-offers; a stale overlay closes."""
        ends = {(kind, start): end for start, end, kind in self._segments}
        self._acted = {key: handled for key, handled in self._acted.items()
                       if key[1] <= now <= ends.get(key, handled)}
        window = self._overlay_window
        if self._overlay is not None and window is not None and not (window[0] <= now <= window[1]):
            self._close_overlay()

    def _check(self, now):
        for start, end, kind in self._segments:
            key = (kind, start)
            if end < now:
                self._acted.pop(key, None)  # left it: re-arm for a seek back
                continue
            if start > now:
                break  # still ahead (sorted)
            handled = self._acted.get(key)
            if handled is not None and end <= handled + 0.5:
                continue  # handled, and the end has not grown since
            mode = self._modes.get(kind, MODE_OFF)
            if mode == MODE_AUTO:
                self._auto_skip(key, end, now)
            elif mode == MODE_ASK:
                self._ask(key, end, now)

    def _auto_skip(self, key, end, now):
        kind, start = key
        if end - start < MIN_AUTO_SKIP_SECONDS:
            self._acted[key] = end
            return
        handled = self._acted.get(key)
        target = safe_seek_end(end, self._total(), now, self._in_progress)
        if target is None or target < now + (MIN_AUTO_SKIP_SECONDS if handled is None
                                             else REENTER_MIN_SECONDS):
            return  # nothing reachable to skip yet; the next tick looks again
        if handled is not None and target <= handled + 0.5:
            return  # the reachable end has not moved past where we landed
        self._acted[key] = target
        self._seek(target, end, kind)
        xbmc.log('pvr.kofin segments: auto-skip %s %.1f-%.1f -> %.1f' % (kind, start, end, target),
                 xbmc.LOGINFO)

    def _ask(self, key, end, now):
        kind, start = key
        if end - start < MIN_ASK_SECONDS:
            self._acted[key] = end  # too short to read a prompt for
            return
        handled = self._acted.get(key)
        target = safe_seek_end(end, self._total(), now, self._in_progress)
        if target is None or target < now + (MIN_ASK_SECONDS if handled is None
                                             else REENTER_MIN_SECONDS):
            return  # nothing worth a prompt is reachable yet
        if handled is not None and target <= handled + 0.5:
            return  # the reachable end has not moved past where the skip landed
        self._acted[key] = end  # the end offered; a longer one re-prompts
        if (self._overlay is not None and self._overlay_start == start
                and self._overlay_type == kind):
            self._overlay_end = end  # prompt already up: its button reads the new end
            return
        self._open_overlay(kind, start, end, now)

    def _seek(self, target, end, kind):
        # A seek held back from the live edge lands wherever the stream
        # currently ends, not on the target to the half second.
        self._settle_tolerance = SETTLE_TOLERANCE if target >= end - 0.5 else LIVE_EDGE_MARGIN
        self._settle_target = target
        self._settle_ticks = SEEK_SETTLE_TICKS
        self._settle_retries = SEEK_RETRIES
        self._pending_notify = kind
        self._player.seekTime(target)

    def _total(self):
        try:
            return float(self._player.getTotalTime())
        except RuntimeError:
            return 0.0

    def _current_end(self, kind, start):
        for s, e, k in self._segments:
            if k == kind and abs(s - start) <= START_TOLERANCE:
                return e
        return None

    # -- the overlay --------------------------------------------------------

    def _open_overlay(self, kind, start, end, now):
        self._close_overlay()
        self._overlay_type = kind
        self._overlay_start = start
        self._overlay_end = end
        self._overlay_hide_at = prompt_hide_at(start, end, self._hide_percent, now)
        self._overlay_window = (start, self._overlay_hide_at)
        try:
            self._overlay = open_overlay(
                self._localized(SKIP_LABELS[kind]),
                format_span(end - max(start, now)),
                self._localized(STRING_CLOSE),
                self._overlay_skip)
        except Exception as error:
            xbmc.log('pvr.kofin segments: overlay failed: %s' % error, xbmc.LOGWARNING)
            self._overlay = None

    def _drive_overlay(self, now):
        overlay = self._overlay
        if overlay is None:
            return
        if getattr(overlay, 'closed', False):
            self._overlay = None  # a button or back closed it on the GUI thread
            return
        if 0 < self._overlay_hide_at <= now:
            self._close_overlay()

    def _close_overlay(self):
        overlay = self._overlay
        self._overlay = None
        self._overlay_window = None
        self._overlay_hide_at = 0.0
        if overlay is not None:
            try:
                overlay.close()
            except Exception:
                pass

    def _overlay_skip(self):
        """Skip button, on Kodi's GUI thread: seeks to the segment's current
        end, which may have grown since the prompt opened."""
        start = self._overlay_start
        kind = self._overlay_type
        if start is None or kind is None:
            return
        end = self._current_end(kind, start)
        if end is None:
            end = self._overlay_end
        try:
            now = float(self._player.getTime())
        except RuntimeError:
            now = None
        target = safe_seek_end(end, self._total(), now, self._in_progress)
        if target is None:
            return
        self._acted[(kind, start)] = target
        self._seek(target, end, kind)
        xbmc.log('pvr.kofin segments: user skipped %s to %.1f' % (kind, target), xbmc.LOGINFO)
