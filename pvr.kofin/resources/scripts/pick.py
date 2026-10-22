#  Copyright (C) 2026 Kofin
#
#  SPDX-License-Identifier: GPL-2.0-or-later
#  See LICENSE.md for more information.

"""Multi-select pickers for the direct play codec and HDR settings.

Kodi's binary settings transfer (CAddonDll::TransferSettings) has cases for
boolean, integer, number and string and none for a list, so a list[string]
setting logs "Unknown setting type" at error level on every transfer. These
three are plain strings instead, behind action buttons that run this picker.
The value is the same comma-joined string the list setting stored, so nothing
migrates.

Kodi handles the picker running while the settings dialog is open: setSetting
posts the value to the open dialog instead of the file, OK there saves it and
Cancel discards it.
"""

import xbmcaddon
import xbmcgui

ADDON_ID = 'pvr.kofin'

# setting id -> (heading string id, [(value, label string id), ...]).
# The table is the allowlist: RunScript is callable by any add-on or skin, so
# only these settings may be written through here.
PICKERS = {
    'directPlayVideoCodecs': (30781, [
        ('h264', 30626), ('h264_10bit', 30802), ('hevc', 30627),
        ('hevc_rext', 30803), ('av1', 30628), ('mpeg2video', 30796),
        ('vp9', 30797), ('vc1', 30798),
    ]),
    'directPlayAudioCodecs': (30783, [
        ('aac', 30631), ('mp2', 30799), ('mp3', 30633), ('ac3', 30632),
        ('eac3', 30800), ('opus', 30715), ('flac', 30801), ('dts', 30805),
    ]),
    'allowedHdrTypes': (30810, [
        ('HDR10', 30812), ('HLG', 30813), ('HDR10Plus', 30814), ('DOVI', 30815),
        ('DOVIWithHDR10', 30816), ('DOVIWithHLG', 30817), ('DOVIWithSDR', 30818),
        ('DOVIWithEL', 30819), ('DOVIWithHDR10Plus', 30820),
        ('DOVIWithELHDR10Plus', 30821),
    ]),
}


def run(setting_id):
    """Entry point, called by trigger.py as RunScript(pvr.kofin,pick,<id>)."""
    if setting_id not in PICKERS:
        return
    addon = xbmcaddon.Addon(ADDON_ID)
    heading_id, options = PICKERS[setting_id]
    values = [value for value, _ in options]
    labels = [addon.getLocalizedString(label_id) for _, label_id in options]
    current = addon.getSetting(setting_id).split(',')
    preselect = [i for i, value in enumerate(values) if value in current]
    picked = xbmcgui.Dialog().multiselect(addon.getLocalizedString(heading_id),
                                          labels, preselect=preselect)
    if picked is None:
        return
    addon.setSetting(setting_id, ','.join(values[i] for i in sorted(picked)))
