# Splinter Cell Chaos Theory: Community Online Co-op

**Work in progress — test build v1.0.0-rc5.** This project is a 32-bit Windows launcher and network relay for online co-op in *Splinter Cell: Chaos Theory*. It connects the game’s existing co-op networking between two players and adds experimental voice support.

## What it does

The game uses LAN-style discovery and gameplay traffic. The launcher relays that traffic between the host and guest using one of two transports:

- **Steam networking:** creates or joins a public lobby and carries co-op traffic over Steam Datagram Relay. Steam must be running, and a compatible `steam_api.dll` must be supplied beside the launcher. The DLL is not included.
- **Direct UDP:** connects a private room over UDP port **7790**. The host may need to allow this port through the Windows firewall and router. Steam is not required.

The launcher starts before the game. It detects the game process, applies the supported in-memory co-op/menu patches, and forwards game discovery, gameplay, control, and voice packets. It also has experimental voice capture/playback and windowed/borderless setup. These features have not all been verified on real two-player sessions.

## Quick start

1. Build the launcher or download `SCCT_OnlineCoop.exe` from the [latest prerelease](https://github.com/biyoudah/Splinter-Cell-Chaos-Theory-Community-Online-Coop/releases).
2. Put it next to the game’s `System` folder. Do not put it inside `System`.
3. For Steam, start Steam and place your compatible `steam_api.dll` beside the launcher. For direct UDP, skip this step.
4. Start the launcher. Choose **Host Public Game** or **Browse & Join** for Steam, or host/join a direct room.
5. In the game, choose **COOP → ONLINE → CREATE GAME** or **JOIN GAME**. Keep the launcher open for the session.

Direct rooms use UDP port 7790. Steam lobbies currently use AppID 480. Use matching game builds on both PCs while testing; fixed-address patches have not been checked against every game release.

## Build from source

Requirements: Visual Studio C++ x86 build tools and the Windows SDK. From this folder, run:

```bat
build.bat
```

The x86 executable is written to `build/`. The build uses `/W4`, `/GS`, Control Flow Guard, ASLR, and DEP. Steam runtime files and game files are not included.

## Current status

This is a prerelease for testing, not a stable 1.0 release.

- **Connectivity:** Steam sends no longer request a session restart on every packet. After a failed session callback, the host probes immediately and the client schedules a rate-limited recovery probe 2.5 seconds later; an incoming host heartbeat cancels the client's pending retry. The local two-process loopback fixture passed with simulated packet loss, delay, a 62-second idle period, and guest reconnect. This fixture does not run Steam or the game; Steam SDR and two-PC recovery remain unverified.
- **Voice and display:** voice devices, proximity audio, windowed/fullscreen transitions, alt-tab, and minimize/restore need live testing.
- **Game compatibility:** fixed-address patches and window settings may vary by game build. Ubisoft Connect, Steam, and standalone releases have not all been validated.
- **Mission loading:** a temporary not-responding screen was observed during a mission transition. The available dump did not establish its cause.

When reporting a test issue, include the launcher log and describe the game build, transport, host/guest role, mission, and approximate time. Remove account names, IDs, and other personal details before sharing logs publicly.

## Repository contents

- `SCCT_OnlineCoop.c` — launcher, relay, Steam integration, voice support, and game patches.
- `build.bat` — x86 Windows build script.
- `SCCT_OnlineCoop.rc` and `SCCT_OnlineCoop.manifest` — Windows resources.
- `relay_loopback_harness.py` and `test_scct_relay.py` — local relay test fixture.
- `README_ONLINE_COOP.txt` — short in-game setup notes.

The repository does not contain the game, game assets, Steam runtime DLL, logs, crash dumps, or signing certificates.
