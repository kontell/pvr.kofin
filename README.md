[![License: GPL-2.0-or-later](https://img.shields.io/badge/License-GPL%20v2+-blue.svg)](LICENSE.md)
[![Build and run tests](https://github.com/kodi-pvr/pvr.kofin/actions/workflows/build.yml/badge.svg?branch=Piers)](https://github.com/kodi-pvr/pvr.kofin/actions/workflows/build.yml)
[![Build Status](https://dev.azure.com/teamkodi/kodi-pvr/_apis/build/status/kodi-pvr.pvr.kofin?branchName=Piers)](https://dev.azure.com/teamkodi/kodi-pvr/_build/latest?definitionId=86&branchName=Piers)
[![Build Status](https://jenkins.kodi.tv/view/Addons/job/kodi-pvr/job/pvr.kofin/job/Piers/badge/icon)](https://jenkins.kodi.tv/blue/organizations/jenkins/kodi-pvr%2Fpvr.kofin/branches/)

# Kofin PVR for Jellyfin

Kodi PVR client addon for [Jellyfin](https://jellyfin.org) Live TV. Provides Kodi PVR integration for live TV channels, EPG, recordings, and timers from a Jellyfin server.

Play catchup/ archive content from EPG in same manner as IPTV simple client.

Forked from [pvr.iptvsimple](https://github.com/kodi-pvr/pvr.iptvsimple), replacing M3U/XMLTV data sources with Jellyfin REST API calls.

**Requires:** Kodi 22 "Piers", Jellyfin 10.9 or later with Live TV configured.

## Features

- Play Jellyfin Live TV channels from Kodi EPG
- Play Catchup/ Archive from EPG (direct play only)
- Play in-progress recordings from beginning
- Force re-muxing (for stream sharing)
- Optional transcoding
- Pause live tv (local or server buffer)
- One-shot and series recording timers
- Skip Jellyfin media segments in recordings, with Off, Auto or Ask for each type: intros, credits, recaps, previews and commercial breaks, including breaks found while a recording is still being written

## Installation

Install from the official Kodi add-on repository.

## Configuration

- After install go to: Add-ons -> My add-ons -> PVR clients -> Kofin PVR for Jellyfin -> Configure
- Enter server details and logon
- Reenter settings and restart add-on
- Kodi PVR settings can be found in: Settings -> PVR & Live TV

## Catchup
If supported by your IPTV provider catchup works by using inputstream.ffmpegdirect to play directly from the provider (transcoding settings are irrelevant for catchup playback). The matching reference-playlist entry must include a streaming URL; catchup tags alone are not enough. Refer to IPTV Simple Client for detailed catchup documentation.

## Reference Playlist

An optional M3U reference playlist can be configured to apply per-channel properties that Jellyfin doesn't provide. Channels are matched by name (case-insensitive) between the playlist and Jellyfin. The same `#KODIPROP:`, `#EXTVLCOPT:`, and `#EXTVLCOPT--` directives supported by IPTV Simple Client are honored, along with standard M3U tags like `group-title=` for channel grouping.

When a matching entry includes a streaming URL, live playback uses that URL directly unless Force remuxing or Force transcoding is on. Jellyfin 12 no longer DirectPlays HLS/DASH tuner sources, so the playlist URL is the way to play those channels without remuxing.

In addition to standard KodiProps, the following Kofin-specific properties can be set per channel:

| Property | Values | Description |
|----------|--------|-------------|
| `kofin-force-direct-play` | `true` / `false` | Direct-play this channel even when Force remuxing or Force transcoding is on. Uses the playlist URL when present, otherwise asks the server for DirectPlay |
| `kofin-force-remux` | `true` / `false` | Force remuxing for this channel (overrides global setting) |
| `kofin-force-transcode` | `true` / `false` | Force transcoding for this channel (overrides global setting) |
| `kofin-bitrate-limit` | kbps (e.g. `4000`) | Set a bitrate limit for this channel (0 or omitted = unlimited) |
| `kofin-disable-pvr` | `true` / `false` | Disable recording for this channel — hides the record option in the EPG |
