# nova-cef (`omp-cef`)

![Version](https://img.shields.io/github/v/release/ndyy2/nova-cef?include_prereleases&label=version)
![License](https://img.shields.io/github/license/ndyy2/nova-cef)
[![Wiki](https://img.shields.io/badge/docs-wiki-blue)](https://github.com/aurora-mp/omp-cef/wiki)
![CEF](https://img.shields.io/badge/CEF-158.0.3-blue)
![SA--MP](https://img.shields.io/badge/SA--MP-0.3.7%20%7C%200.3.DL-orange)
![open.mp](https://img.shields.io/badge/open.mp-supported-brightgreen)

Client/server CEF plugin for **open.mp** and **SA-MP**, maintained by **[@ndyy2](https://github.com/ndyy2)** (forked from [aurora-mp/omp-cef](https://github.com/aurora-mp/omp-cef)).

`nova-cef` embeds Chromium Embedded Framework into GTA San Andreas Multiplayer clients and allows servers to create browser-based UI in several rendering modes: Overlay2D, World2D and WorldObject3D.

## Features

- Off-screen CEF rendering inside GTA SA
- Overlay2D browser rendering for HUDs and menus
- Browser layers to control which 2D interfaces appear on top
- World2D browser rendering at world positions
- WorldObject3D browser rendering through object texture replacement
- Packaged server resources served through `http://cef/...`
- Pawn natives and callbacks
- JavaScript <-> Pawn/C# event bridge
- Focus, cursor, input and audio controls
- Game screen capture exposed to CEF interfaces as RGBA frames
- Custom ESC menu support
- Custom TAB menu (scoreboard) support
- Secure client/server handshake and MTU-safe KCP/UDP transport
- Automatic `cef_port` discovery via server query rules with backward compatibility for v1.3.0+ clients

## Documentation

The full documentation is available in the GitHub Wiki:

https://github.com/aurora-mp/omp-cef/wiki

## Supported clients

- SA-MP 0.3.7-R1
- SA-MP 0.3.7-R3-1
- SA-MP 0.3.7-R5-1
- SA-MP 0.3.DL-R1

## Current CEF version

```txt
158.0.3+g8c29be2+chromium-158.0.8127.18
```

## Repository layout

```txt
src/
  client/          # client plugin (CEF offscreen + DX9 + SA:MP hooks)
  server/
    common/        # network core, sessions, resources, protocol
    omp/           # open.mp component (bridge + lifecycle + natives)
    samp/          # SA-MP plugin (bridge + lifecycle + natives)
   shared          # shared protocol/types (packets, serialization, common utilities)
deps/              # deps (minhook, sdk, etc.)
vcpkg.json         # vcpkg manifest (if using manifest mode)
CMakeLists.txt
CMakePresets.json  # CMake presets (configure/build presets for VS/CMake)
```

## Configuration (open.mp)

In `config.json`, the CEF server options can be configured under `cef`:

```json
"cef": {
  "bind": "",
  "debug": false,
  "init_timeout": 15,
  "master_resource_key": "ThisIsA16ByteKey",
  "min_client_version": "1.3.0",
  "port": 0,
  "resources_loader_mode": "cef",
  "resources_loader_dialog_id": 32700,
  "strict_ip_check": true
}
```

- `cef.port: 0` (default): automatically uses `network.port + 2` (upstream behavior).
- `cef.port: 1..65535`: binds the CEF UDP server to the specified port and publishes the open.mp query rule `cef_port=<port>` (`enable_query: true`). Clients built from this repo automatically detect `cef_port` on join (with fallback to `gamePort + 2` on servers without the rule). Legacy/upstream clients (`v1.3.0`–`v1.4.2`) still connect to `gamePort + 2` unless forwarded via NAT/proxy.
- `cef.bind: ""` (default): falls back to `network.bind` (or `0.0.0.0` if unset). Useful on multi-homed hosts so UDP replies use the same IP as the game server.
- `cef.min_client_version: "1.3.0"` (default): minimum compatible CEF client version (accepts v1.3.0 through v1.4.x; newer features like `CEF_SetBrowserLayer` are gracefully skipped for `< 1.4.0` clients).
- `cef.strict_ip_check: true` (default): requires CEF UDP packets to originate from the player's game IP. Set to `false` if players connect through a reverse proxy, Docker NAT, or CGNAT where game and CEF UDP traffic exit from different IPs.
- `cef.init_timeout: 15` (default, clamped to `5..60` seconds; `cef_init_timeout` in SA-MP `server.cfg`): maximum time the server waits for the CEF handshake after `OnPlayerClientInit` before firing `CEF_INIT_TIMEOUT`.
- **Reliable KCP manifest delivery (`v1.4.3+`)**: clients `v1.4.3+` receive the resource manifest reliably over KCP (`MTU = 1200`) inside `ServerConfig`, keeping raw UDP `JoinResponse` packets small to prevent IP fragmentation drops on mobile/PPPoE/VPN/CGNAT connections. Legacy clients (`v1.3.0`–`v1.4.2`) remain fully supported.

## Building

Build requirements and detailed setup instructions are documented in the wiki:

https://github.com/aurora-mp/omp-cef/wiki

Short version:

- Visual Studio 2022
- CMake
- vcpkg
- Windows SDK
- x86 target for GTA SA / SA-MP client-side

## Samples

- Overlay2D HUD
- World2D browser
- WorldObject3D browser
- JavaScript <-> Pawn events
- Browser lifecycle
- Hidden browser navigation
- Stress testing

## Credits

- **[@ndyy2](https://github.com/ndyy2)** — `nova-cef` fork maintainer (custom `cef_port` auto-detection, KCP manifest delivery & MTU-safe transport, handshake retry & timeout reliability fixes, v1.3.0+ compatibility)
- **[aurora-mp/omp-cef](https://github.com/aurora-mp/omp-cef)** — Original `omp-cef` project and upstream contributors

## Contributing

Pull requests are welcome <3.

## License

See the repository license.
