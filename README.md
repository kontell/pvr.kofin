[![License: GPL-2.0-or-later](https://img.shields.io/badge/License-GPL%20v2+-blue.svg)](LICENSE.md)

# Kodi PVR for Jellyfin

Kodi PVR client addon for [Jellyfin](https://jellyfin.org) Live TV. Provides native Kodi PVR integration for live TV channels, EPG, recordings, and timers from a Jellyfin server.

Play cathup/ archive content from EPG in same manner as IPTV simple client (When direct playing Jellyfin server acts only as proxy between client and provider).

Forked from [pvr.iptvsimple](https://github.com/kodi-pvr/pvr.iptvsimple), replacing M3U/XMLTV data sources with Jellyfin REST API calls.

**Requires:** Kodi 21 "Omega" or later, Jellyfin 10.9.x or later with Live TV configured.

## Installation

Install via the [Kontell Repository](https://github.com/kontell/repository.kontell) for automatic updates.

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

## License

This project is licensed under the [GPL-2.0-or-later](LICENSE.md) license.
