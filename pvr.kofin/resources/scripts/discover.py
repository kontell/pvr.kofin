"""Find Jellyfin servers on the local network and fill in the server address.

Run from the Account settings button:

    RunScript(special://home/addons/pvr.kofin/resources/scripts/discover.py)

The button carries <close>true</close>, which is load-bearing rather than
house style: this script opens modal dialogs, and a modal raised while the
settings dialog is still up fights it. By the time we run, settings is gone --
so the script reopens it at the end, and the filled-in field is the
confirmation. A binary add-on cannot do that for itself; the settings ABI has
no action callback and no way to open its own dialog, which is why this lives
in Python rather than beside JellyfinAuth in C++.

Ported from plugin.video.kofin (lib/kofin/core/discovery.py and
lib/kofin/plugin/serverpicker.py), keeping the decisions those carry.

Jellyfin's client discovery is one plaintext datagram broadcast to
255.255.255.255:7359; every server whose AutoDiscovery is on -- the default --
replies **unicast** to our ephemeral port with a small JSON object naming
itself. Server-side that is receive, serialise three strings, send: no
database, no disk. A reply measured 3 ms on a wired workstation and 22 ms over
Wi-Fi on an Android tablet, so one not seen within a few tens of milliseconds
was *lost* rather than delayed. Waiting longer recovers nothing; another probe
does, and the fragile half is our own broadcast -- an access point sends it at
the lowest basic rate, unacknowledged. Hence three probes a second apart
closing at three seconds, rather than one probe and a longer listen.

Several classes of server never answer at any timeout, which is what the
"nothing found" wording has to account for: AutoDiscovery switched off, Docker
bridge networking (a published UDP port does not receive 255.255.255.255
traffic), another subnet, and -- easy to misread as a bug -- a second Jellyfin
on a host where something already holds 7359, since only one process per
machine can bind it.

Security: RunScript is callable by any add-on or skin, so this script is
reachable from outside the settings dialog. Unlike trigger.py, which guards an
allowlist because it can poke arbitrary setting names, the only thing writable
here is jellyfinServerAddress, and only with a value that a real Jellyfin on
this LAN answered from and that the user then picked out of a dialog.
"""

import json
import socket
import ssl
import time
from urllib.error import URLError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

import xbmc
import xbmcaddon
import xbmcgui

ADDON_ID = 'pvr.kofin'
SETTING = 'jellyfinServerAddress'

DISCOVERY_PORT = 7359
DISCOVERY_MESSAGE = b'who is JellyfinServer?'
BROADCAST_ADDRESS = '255.255.255.255'

# Three probes, one a second, then done. See the module docstring for why the
# budget buys repeats rather than a longer wait.
SCAN_SECONDS = 3.0
PROBE_INTERVAL_SECONDS = 1.0

# How long one recvfrom blocks. Not a protocol timeout -- it is the poll
# granularity, and it is short so Cancel answers promptly and the progress bar
# moves. The window above is what actually bounds the scan.
READ_SLICE_SECONDS = 0.2

# The real payload is ~120 bytes. 1024 is what every other client reads.
RECV_BYTES = 1024

# One attempt per address. The scan window is already the retry policy, and a
# server that answered the broadcast milliseconds ago is plainly alive -- what
# this probe tests is whether the address it *published* is usable from here,
# and every way that fails (an unroutable IP, a name with no answer, a TLS
# mismatch) fails while connecting rather than while reading.
PROBE_TIMEOUT_SECONDS = 4.0

ADDON = xbmcaddon.Addon(ADDON_ID)


def text(string_id):
    return ADDON.getLocalizedString(string_id)


def log(message, level=xbmc.LOGINFO):
    xbmc.log('pvr.kofin discover: %s' % message, level)


def ssl_context():
    """An unverified SSL context when the user turned sslVerify off, else None.

    Mirrors service.py: None means urlopen's default, a fully verifying one.
    """
    if ADDON.getSetting('sslVerify') != 'false':
        return None
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    return context


def parse(data, source_host):
    """One datagram -> (server_id, name, address, source_host), or None."""
    try:
        payload = json.loads(data.decode('utf-8'))
    except (UnicodeDecodeError, ValueError):
        # Something else is sitting on 7359, or the datagram was truncated.
        # Not fatal: the rest of the window belongs to whatever else answers.
        log('undecodable datagram from %s' % source_host, xbmc.LOGWARNING)
        return None
    if not isinstance(payload, dict):
        return None
    address = (payload.get('Address') or '').strip().rstrip('/')
    if not address:
        return None
    return {
        'id': str(payload.get('Id') or ''),
        'name': str(payload.get('Name') or '') or source_host,
        'address': address,
        'source_host': source_host,
    }


def scan(progress):
    """Broadcast for SCAN_SECONDS. Returns the servers, or None if cancelled."""
    found = []
    seen = set()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        started = time.monotonic()
        probes = 0
        while True:
            now = time.monotonic()
            elapsed = now - started
            if elapsed >= SCAN_SECONDS:
                break
            if progress.iscanceled():
                return None
            if now >= started + probes * PROBE_INTERVAL_SECONDS:
                try:
                    sock.sendto(DISCOVERY_MESSAGE,
                                (BROADCAST_ADDRESS, DISCOVERY_PORT))
                except OSError as error:
                    # No route to the broadcast address: an interface that is
                    # down. The window continues -- a later probe may land, and
                    # "nothing found" is the same answer to the user either way.
                    log('probe failed: %s' % error, xbmc.LOGWARNING)
                probes += 1
            progress.update(int(elapsed * 100 / SCAN_SECONDS), text(30847))
            sock.settimeout(min(READ_SLICE_SECONDS, SCAN_SECONDS - elapsed))
            try:
                data, addr = sock.recvfrom(RECV_BYTES)
            except socket.timeout:
                continue
            except OSError as error:
                log('read failed: %s' % error, xbmc.LOGWARNING)
                break
            entry = parse(data, addr[0])
            if entry is None:
                continue
            # Keyed on the address as well as the id: two servers restored from
            # one data directory share an Id, and an id-only dedupe silently
            # hides the second, which reads as discovery being broken.
            key = (entry['id'], entry['address'])
            if key in seen:
                continue
            seen.add(key)
            found.append(entry)
    finally:
        sock.close()
    log('%d server(s) answered in %.1fs' % (len(found), SCAN_SECONDS))
    return found


def public_info(address):
    """/System/Info/Public, or None if this box cannot reach that address."""
    request = Request(address + '/System/Info/Public',
                      headers={'Accept': 'application/json'})
    try:
        context = ssl_context()
        if context is None:
            handle = urlopen(request, timeout=PROBE_TIMEOUT_SECONDS)
        else:
            handle = urlopen(request, timeout=PROBE_TIMEOUT_SECONDS,
                             context=context)
        with handle:
            body = json.loads(handle.read(65536).decode('utf-8'))
        return body if isinstance(body, dict) else None
    except (URLError, OSError, ValueError) as error:
        log('%s did not answer: %s' % (address, error))
        return None


def fallback_address(entry):
    """The published address re-hosted on the IP the datagram came from.

    A server answers with the URL it publishes, which is not always one this
    box can use: a reverse-proxied install answers a client on its own wire
    with its external hostname. The datagram's source is the one endpoint in
    the exchange known to be reachable, so it is what the second attempt uses.
    """
    parts = urlsplit(entry['address'])
    netloc = entry['source_host']
    if parts.port:
        netloc = '%s:%d' % (netloc, parts.port)
    rebuilt = '%s://%s' % (parts.scheme or 'http', netloc)
    if parts.path:
        rebuilt += parts.path.rstrip('/')
    return rebuilt


def verify(entry):
    """Try the published address, then the source address. Annotates entry."""
    candidates = [entry['address']]
    fallback = fallback_address(entry)
    if fallback != entry['address']:
        candidates.append(fallback)
    for address in candidates:
        info = public_info(address)
        if info is not None:
            entry['reachable'] = True
            entry['address'] = address
            entry['version'] = str(info.get('Version') or '')
            entry['name'] = str(info.get('ServerName') or '') or entry['name']
            return
    entry['reachable'] = False
    entry['version'] = ''


def row(entry):
    """(label, label2) for one picker row."""
    detail = entry['address']
    if entry['version']:
        detail = '%s · %s' % (detail, entry['version'])
    if not entry['reachable']:
        detail = '%s · %s' % (detail, text(30849))
    return entry['name'], detail


def main():
    progress = xbmcgui.DialogProgress()
    progress.create(ADDON.getAddonInfo('name'), text(30847))
    try:
        found = scan(progress)
        if found:
            for index, entry in enumerate(found):
                progress.update(int((index + 1) * 100 / len(found)), text(30848))
                verify(entry)
    finally:
        progress.close()

    if found is None:
        log('cancelled')
        return
    if not found:
        # Deliberately not "try again": nothing on this protocol answers
        # slowly, so a repeat only helps against a lost frame, and the usual
        # causes no retry reaches. See the module docstring.
        xbmcgui.Dialog().notification(ADDON.getAddonInfo('name'), text(30850),
                                      xbmcgui.NOTIFICATION_WARNING, 6000)
        return

    # Reachable first: one that answered nothing is offered rather than hidden,
    # because the user may know the network better than the probe does, but it
    # should not be the obvious pick.
    found.sort(key=lambda entry: (not entry['reachable'], entry['name'].lower()))

    items = []
    for entry in found:
        label, detail = row(entry)
        items.append(xbmcgui.ListItem(label, detail))
    choice = xbmcgui.Dialog().select(text(30846), items, useDetails=True)
    if choice < 0:
        return

    picked = found[choice]
    # Both address normalisers in this add-on strip the path
    # (InstanceSettings::GetJellyfinBaseUrl and service.py::normalize_base_url),
    # so a Jellyfin published under one cannot be addressed. Storing it anyway
    # would leave a field that reads correctly and does not work, so say so
    # instead of writing it.
    if urlsplit(picked['address']).path:
        xbmcgui.Dialog().ok(ADDON.getAddonInfo('name'),
                            text(30851) % picked['address'])
        return

    ADDON.setSetting(SETTING, picked['address'])
    log('server address set to %s' % picked['address'])
    if not picked['reachable']:
        xbmcgui.Dialog().notification(ADDON.getAddonInfo('name'),
                                      text(30852) % picked['name'],
                                      xbmcgui.NOTIFICATION_WARNING, 6000)
    # No success toast: the reopened field is the confirmation, and Login sits
    # directly below it.
    xbmc.executebuiltin('Addon.OpenSettings(%s)' % ADDON_ID)


main()

# Kodi warns "left several classes in memory that we couldn't clean up" when an
# xbmcaddon.Addon outlives the script, so drop the module-level reference here
# rather than constructing one per call: a handful of lookups is not worth
# ~3 ms of Addon construction each, and the warning is noise in every log.
del ADDON
