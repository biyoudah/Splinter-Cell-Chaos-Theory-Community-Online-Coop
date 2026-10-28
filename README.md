# Splinter Cell Chaos Theory: Community Online Co-op

**Work in progress.** This community project is an x86 Windows launcher and network relay for *Splinter Cell: Chaos Theory* co-op. It supports Steam networking and direct UDP rooms.

## Build

Install Visual Studio C++ x86 build tools and the Windows SDK, then run:

```bat
build.bat
```

The executable is written to `build/`. For Steam features, provide a compatible `steam_api.dll`; it is not included.

## Run

1. Place the launcher beside the game's `System` folder.
2. Start it before co-op.
3. Host a Steam public lobby or direct UDP room, or join a friend's session.
4. Use **COOP → ONLINE → CREATE GAME** or **JOIN GAME** in the game.
5. Keep the launcher open while playing.

Direct rooms use UDP port 7790. Steam public lobbies currently use AppID 480.

## Current status

- The source builds with the included x86 script and `/W4`.
- Native in-game cutscene synchronization is **not implemented**. The launcher does not yet have a verified game-thread cutscene trigger/state hook.
- Steam/direct reconnection, voice devices, window modes, and alternate game builds need real-session testing.
- The local UDP relay fixture failed here because loopback heartbeat replies were not received. It needs investigation and a successful run on a normal Windows desktop before release.

## Repository

This repository contains the launcher source, Windows resources, build script, and a small relay test fixture. It does not include game files, Steam runtime files, logs, crash dumps, or signing certificates.
