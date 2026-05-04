[![License: GPL-2.0-or-later](https://img.shields.io/badge/License-GPL%20v2+-blue.svg)](LICENSE.md)

# Jellyfin PVR Client for Kodi

Kodi PVR client addon for [Jellyfin](https://jellyfin.org) Live TV. Provides native Kodi PVR integration for live TV channels, EPG, recordings, and timers from a Jellyfin server.

Play cathup/ archive content from EPG in same manner as IPTV simple client (When direct playing Jellyfin server acts only as proxy between client and provider).

Forked from [pvr.iptvsimple](https://github.com/kodi-pvr/pvr.iptvsimple), replacing M3U/XMLTV data sources with Jellyfin REST API calls.

**Requires:** Kodi 21 "Omega" or later, Jellyfin 10.9.x or later with Live TV configured.

## Features

- Play Live TV channels from Jellyfin
- Play Catchup/ Archive from EPG (direct play only)
- Play in-progress recordings from beginning
- Force re-muxing (for stream sharing)
- Optional transcoding
- Pause live tv (local or server buffer)
- One-shot and series recording timers
- inputstream.ffmpegdirect / inputstream.adaptive support
- Login with username/ password or quick connect code

## Installation

Install via the [Kontell Repository](https://github.com/kontell/repository.kontell).

## Configuration

- After install go to: Add-ons -> My add-ons -> PVR clients -> Kofin PVR for Jellyfin -> Configure
- Enter server details and logon
- Reenter settings and restart add-on
- Kodi PVR settings can be found in: Settings -> PVR & Live TV
- Kofin settings can be accessed from: Settings -> PVR & Live TV -> General -> Client specific settings

## Catchup
If supported by your provider cathup works with direct play only. Force remuxing must be disabled and streams must be within any bitrate limit set. To use it you must upload a reference playlist that provides the relevant catchup tags which are omitted by Jellyfin. Refer to IPTV Simple Client for detailed catchup documentation.

Note: Only tested with timeshift="days" element.

## Supported platforms

| Platform | Kodi 21 (Omega) | Kodi 22 (Piers) |
|----------|----------------|-----------------|
| Linux x86_64 | yes | yes |
| Linux armv7 (Pi 2+) | yes | yes |
| Linux aarch64 (Pi 3+) | yes | yes |
| Android ARM32 | yes | yes |
| Android ARM64 | yes | yes |
| Windows x86_64 | yes | yes |

## License

This project is licensed under the [GPL-2.0-or-later](LICENSE.md) license.
