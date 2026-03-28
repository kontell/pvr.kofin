[![License: GPL-2.0-or-later](https://img.shields.io/badge/License-GPL%20v2+-blue.svg)](LICENSE.md)

# Kofin PVR for Jellyfin

Kodi PVR client addon for [Jellyfin](https://jellyfin.org) Live TV. Provides native Kodi PVR integration for live TV channels, EPG, recordings, and timers from a Jellyfin server.

Forked from [pvr.iptvsimple](https://github.com/kodi-pvr/pvr.iptvsimple), replacing M3U/XMLTV data sources with Jellyfin REST API calls while keeping the proven stream handling pipeline.

**Requires:** Kodi 21 "Omega" or later, Jellyfin 10.9.x or later with Live TV configured.

## Features

- Live TV channels from Jellyfin with automatic channel numbering
- Electronic Program Guide (EPG) from Jellyfin
- One-shot and series recording timers
- Recording playback and management (view, delete)
- Channel groups
- HLS stream playback with MPEG-TS segments
- Automatic tuner lifecycle management (allocate on play, release on stop)
- inputstream.ffmpegdirect / inputstream.adaptive support

## Build instructions

### Prerequisites

- Kodi development headers (`kodi-addons-dev`)
- JsonCpp (`libjsoncpp-dev`)
- pugixml (`libpugixml-dev`)
- zlib (`zlib1g-dev`)
- LZMA (`liblzma-dev`)

### Linux

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

The built addon will be at `build/pvr.kofin.so`.

### Install

Copy the built `pvr.kofin` directory and `pvr.kofin.so` to your Kodi addons directory:

```bash
cp -r pvr.kofin ~/.kodi/addons/
cp build/pvr.kofin.so ~/.kodi/addons/pvr.kofin/
```

## Configuring the addon

### Jellyfin Server

| Setting | Description | Default |
|---------|-------------|---------|
| **Server hostname** | Jellyfin server hostname or IP address | *(empty)* |
| **Port** | Jellyfin server port | `8096` |
| **Use HTTPS** | Connect using HTTPS | `false` |

### Authentication

Two authentication methods are supported:

| Setting | Description | Default |
|---------|-------------|---------|
| **Authentication method** | Username & Password or Quick Connect | `Username & Password` |
| **Username** | Jellyfin username (visible when method is Username & Password) | *(empty)* |
| **Password** | Jellyfin password (visible when method is Username & Password) | *(empty)* |

**Username & Password**: Enter your Jellyfin credentials. The addon authenticates on startup and stores the access token for subsequent sessions.

**Quick Connect**: On startup, the addon displays a code in a progress dialog. Enter this code in your Jellyfin dashboard (Quick Connect section) to authorize the addon. The access token is stored for subsequent sessions.

### Options

| Setting | Description | Default |
|---------|-------------|---------|
| **Update interval** | Minutes between channel/EPG refresh | `60` |
| **Connection check timeout** | Seconds to wait for server ping | `10` |
| **Connection check interval** | Seconds between connection checks | `5` |

## Timer types

| Type | Description |
|------|-------------|
| **Record once (EPG)** | One-shot recording from an EPG programme. Supports pre/post padding. |
| **Record once (series child)** | Auto-created child of a series rule. Read-only, can be deleted. |
| **Record series** | Series recording rule. Supports any-channel, new-only, weekday filtering. |

## License

This project is licensed under the [GPL-2.0-or-later](LICENSE.md) license.
