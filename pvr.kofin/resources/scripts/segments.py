#  Copyright (C) 2026 Kofin
#
#  SPDX-License-Identifier: GPL-2.0-or-later
#  See LICENSE.md for more information.

"""Commercial skipping for recordings.

A port of plugin.video.kofin's segment engine (``service/segments.py`` and
``service/skip.py``) cut down to one segment type, and extended for a
recording still being written: the Comseg server plugin publishes breaks
while the file grows, so while such a recording plays the engine re-fetches
them and acts on the ones that arrived after Kodi opened the stream.

Two modes. **Auto**: the C++ add-on hands Kodi the breaks known when
playback opened, as an EDL Kodi skips by itself, so this engine acts only on
breaks that appear later. **Ask**: no EDL; this engine offers a skip button
for every break. Positions are the player's own clock, which for a
timeshifted stream counts from the recording's start, the same timeline the
server's segment ticks use.

The tick is 0.25 s on float positions and detects boundary *crossings*, so
a coarse or late poll cannot step over a break; a settle window after our
own seek keeps a lagging ``getTime()`` from re-triggering it; the overlay's
lifetime is tick-driven, and its button callbacks run on Kodi's GUI thread.
"""

import threading
import time

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
# Two breaks are the same break when their starts are this close; the server
# replaces a break's row when its end moves, so starts are the identity.
START_TOLERANCE = 1.5
# Never seek this close to the end of a stream still being written: the
# target may not have been remuxed yet, and Kodi treats a seek at the end of
# the stream as the end of playback.
LIVE_EDGE_MARGIN = 3.0

MODE_OFF = 0
MODE_AUTO = 1
MODE_ASK = 2

XML_FILENAME = 'pvrkofin-skip.xml'
SKIP_BUTTON = 3012
CLOSE_BUTTON = 3013
ACTION_SELECT_ITEM = 7
ACTION_PARENT_DIR = 9
ACTION_PREVIOUS_MENU = 10
ACTION_NAV_BACK = 92

STRING_SKIP_AD = 30875
STRING_CLOSE = 30876
STRING_SKIPPED = 30877


# -- pure helpers --------------------------------------------------------------


def parse_commercials(doc):
    """Sorted ``[(start, end)]`` in seconds from a /MediaSegments body."""
    out = []
    for item in (doc or {}).get('Items') or []:
        if item.get('Type') != 'Commercial':
            continue
        try:
            start = float(item.get('StartTicks') or 0) / 10_000_000
            end = float(item.get('EndTicks') or 0) / 10_000_000
        except (TypeError, ValueError):
            continue
        if end > start:
            out.append((start, end))
    out.sort()
    return out


def safe_seek_end(end, total, now, live):
    """Where a skip lands: the break's end, held back from the end of a
    growing stream, or None when the seek would go nowhere."""
    target = max(0.0, float(end))
    if total > 0:
        cap = total - (LIVE_EDGE_MARGIN if live else 1.0)
        if cap <= 0:
            cap = total
        target = min(target, cap)
    if now is not None and target <= now + 0.5:
        return None
    return target


def prompt_hide_at(start, end, percent, now, minimum=MIN_PROMPT_SECONDS):
    """When a prompt hides: a share of the break, never sooner than the
    reading floor measured from ``now`` (a late-entered break still gets its
    full dwell), never past the break's end."""
    deadline = start + max(0.0, end - start) * (percent / 100.0)
    floor = max(start, now) + minimum
    return min(end, max(deadline, floor))


def format_span(seconds):
    """``M:SS`` for a span, as the skip button renders it."""
    whole = max(0, int(round(seconds)))
    return '%d:%02d' % (whole // 60, whole % 60)


def same_break(a, b):
    return abs(float(a[0]) - float(b[0])) <= START_TOLERANCE


# -- the overlay ---------------------------------------------------------------


class SkipOverlay(xbmcgui.WindowXMLDialog):
    """Skip Ad / Close over the video. Non-modal: ``show()`` returns at once
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


# -- the engine ----------------------------------------------------------------


class CommercialSkipper(threading.Thread):
    """Drives the player's break tick while a recording plays.

    ``player`` is the ``xbmc.Player`` that owns the playback (``getTime``,
    ``getTotalTime``, ``seekTime``); ``fetch`` returns the current
    ``[(start, end)]`` from the server or raises; ``localized`` and
    ``notify`` are the add-on's string lookup and toast.
    """

    def __init__(self, player, mode, hide_percent, poll_seconds, in_progress,
                 fetch, localized, notify):
        super().__init__(name='pvr.kofin-segments')
        self._player = player
        self._mode = mode
        self._hide_percent = max(10.0, min(100.0, float(hide_percent or 50)))
        self._poll_seconds = max(5.0, float(poll_seconds or 10))
        self._in_progress = in_progress
        self._fetch = fetch
        self._localized = localized
        self._notify = notify
        self._halt = threading.Event()
        self._segments = []          # [(start, end)], sorted
        self._native = []            # breaks Kodi's EDL already handles (Auto)
        self._loaded = False
        self._prev_pos = None
        self._acted = {}             # start -> end acted on (dedup, end growth)
        self._settle_target = None
        self._settle_ticks = 0
        self._settle_retries = 0
        self._pending_notify = False
        self._pending_jump = False
        self._overlay = None
        self._overlay_hide_at = 0.0
        self._overlay_window = None
        self._skip_target = None
        self._next_fetch = 0.0

    # -- lifecycle ---------------------------------------------------------

    def stop(self):
        self._halt.set()
        if self.is_alive() and threading.current_thread() is not self:
            self.join(timeout=5)
        self._close_overlay()

    def note_seek(self, target):
        """Player-seek hook: user seeks re-arm the engine; the echo of our
        own skip seek is ignored."""
        settle = self._settle_target
        if settle is not None and abs(target - settle) < 2.0:
            return
        self._settle_target = None
        self._pending_jump = True

    def run(self):
        xbmc.log('pvr.kofin segments: engine started (mode %d, in progress %s)'
                 % (self._mode, self._in_progress), xbmc.LOGDEBUG)
        monitor = xbmc.Monitor()
        self._load(monitor)
        while not self._halt.is_set() and not monitor.abortRequested():
            try:
                self._tick()
            except Exception as error:
                xbmc.log('pvr.kofin segments: tick failed: %s' % error, xbmc.LOGWARNING)
            # Kodi-aware wait: while the overlay is open the engine must yield
            # to Kodi between ticks so the window's callbacks are serviced.
            if monitor.waitForAbort(TICK_SECONDS):
                break
        self._close_overlay()
        xbmc.log('pvr.kofin segments: engine stopped', xbmc.LOGDEBUG)

    def _load(self, monitor):
        for attempt in (1, 2):
            if self._halt.is_set():
                return
            try:
                self._segments = self._fetch()
                break
            except Exception as error:
                xbmc.log('pvr.kofin segments: fetch failed (try %d): %s' % (attempt, error),
                         xbmc.LOGWARNING)
                if monitor.waitForAbort(1.0):
                    return
        # In Auto mode the breaks known at open went to Kodi as an EDL, which
        # skips them itself; acting on them here too would double-seek.
        if self._mode == MODE_AUTO:
            self._native = list(self._segments)
        self._loaded = True
        self._next_fetch = time.time() + self._poll_seconds
        if self._segments:
            xbmc.log('pvr.kofin segments: %d breaks known at start' % len(self._segments),
                     xbmc.LOGDEBUG)

    def _refetch(self):
        if not self._in_progress or time.time() < self._next_fetch:
            return
        self._next_fetch = time.time() + self._poll_seconds
        try:
            fetched = self._fetch()
        except Exception as error:
            xbmc.log('pvr.kofin segments: refetch failed: %s' % error, xbmc.LOGDEBUG)
            return
        if fetched != self._segments:
            xbmc.log('pvr.kofin segments: now %d breaks' % len(fetched), xbmc.LOGDEBUG)
            self._segments = fetched

    # -- the tick -----------------------------------------------------------

    def _tick(self):
        try:
            now = float(self._player.getTime())
        except RuntimeError:  # nothing playing yet / race with stop
            return

        self._drive_overlay(now)
        if not self._loaded:
            self._prev_pos = now
            return
        self._refetch()

        if self._settle_target is not None:
            # Post-seek settle: a lagging getTime() must not re-trigger the
            # break we just skipped.
            self._settle_ticks -= 1
            if now >= self._settle_target - 0.5:
                if self._pending_notify:
                    self._notify(self._localized(STRING_SKIPPED))
                    self._pending_notify = False
                self._settle_target = None
                self._resync(now)
                self._prev_pos = now
            elif self._settle_ticks <= 0:
                if self._settle_retries > 0:
                    self._settle_retries -= 1
                    self._settle_ticks = SEEK_SETTLE_TICKS
                    self._player.seekTime(self._settle_target)
                else:
                    self._pending_notify = False
                    self._settle_target = None
                    self._resync(now)
                    self._prev_pos = now
            return

        if self._pending_jump:
            self._pending_jump = False
            self._resync(now)
            self._prev_pos = None  # no crossing credit across a user seek

        self._check(now)
        self._prev_pos = now

    def _resync(self, now):
        """After a position jump only breaks still containing ``now`` stay
        consumed, so a seek back re-offers; a stale overlay closes."""
        self._acted = {s: v for s, v in self._acted.items() if s <= now <= v[0]}
        window = self._overlay_window
        if self._overlay is not None and window is not None and not (window[0] <= now <= window[1]):
            self._close_overlay()

    def _check(self, now):
        for start, end in self._segments:
            if end < now:
                self._acted.pop(start, None)  # left it: re-arm for a seek back
                continue
            if start > now:
                break  # still ahead (sorted)
            acted = self._acted.get(start)  # (end acted on, target reached)
            if self._mode == MODE_AUTO:
                native = next((n for n in self._native if same_break((start, end), n)), None)
                if native is not None and (end <= native[1] + 0.5 or now < native[1] - 0.5):
                    continue  # Kodi's own EDL skip covers this
                # Either a break Kodi never saw, or one that grew past the end
                # Kodi knew (a provisional end on a recording still being
                # written) and Kodi's skip landed inside the rest of it.
                reach = self._reach()
                if acted is None or end > acted[0] + 0.5:
                    if end - start >= MIN_AUTO_SKIP_SECONDS:
                        self._auto_skip(start, end, now, reach)
                    else:
                        self._acted[start] = (end, end)
                elif acted[1] < end - 0.5 and reach >= end:
                    # An earlier skip stopped at the live edge; the rest of the
                    # break has been remuxed since: one more seek, to the end.
                    self._auto_skip(start, end, now, reach)
                continue
            if acted is None:
                self._acted[start] = (end, end)
                self._ask(start, end, now)

    def _reach(self):
        """How far a seek may go: the end of the stream, held back from the
        live edge of one still being written."""
        total = self._total()
        if total <= 0:
            return float('inf')
        return total - (LIVE_EDGE_MARGIN if self._in_progress else 1.0)

    def _ask(self, start, end, now):
        if now >= end - 0.25 or end - start < MIN_ASK_SECONDS:
            return  # nothing left to skip, or too short to read a prompt for
        self._open_overlay(start, end, now)

    def _auto_skip(self, start, end, now, reach):
        target = min(end, reach)
        if target <= now + 0.5:
            return  # nothing to skip to yet; left unrecorded so the next tick retries
        self._acted[start] = (end, target)
        self._begin_settle(target, notify=True)
        self._player.seekTime(target)
        xbmc.log('pvr.kofin segments: auto-skip %.1f-%.1f -> %.1f' % (start, end, target),
                 xbmc.LOGINFO)

    def _begin_settle(self, target, notify):
        self._settle_target = target
        self._settle_ticks = SEEK_SETTLE_TICKS
        self._settle_retries = SEEK_RETRIES
        self._pending_notify = notify

    def _total(self):
        try:
            return float(self._player.getTotalTime())
        except RuntimeError:
            return 0.0

    # -- the overlay --------------------------------------------------------

    def _open_overlay(self, start, end, now):
        self._close_overlay()
        self._skip_target = end
        self._overlay_hide_at = prompt_hide_at(start, end, self._hide_percent, now)
        self._overlay_window = (start, self._overlay_hide_at)
        try:
            self._overlay = open_overlay(
                self._localized(STRING_SKIP_AD),
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
        """Skip button, on Kodi's GUI thread."""
        target = self._skip_target
        if target is None:
            return
        try:
            now = float(self._player.getTime())
        except RuntimeError:
            now = None
        seek_to = safe_seek_end(target, self._total(), now, self._in_progress)
        if seek_to is None:
            return
        self._begin_settle(seek_to, notify=True)
        self._player.seekTime(seek_to)
        xbmc.log('pvr.kofin segments: user skipped to %.1f' % seek_to, xbmc.LOGINFO)
