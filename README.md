<!-- markdownlint-disable MD041 -->
# Enshrouded LiveMap (Discord Bridge & Standalone CI/CD Fork)

> **Attribution & Credits:**
> - **Original Creator:** [oggii](https://github.com/oggii) ([oggii/enshrouded-livemap](https://github.com/oggii/enshrouded-livemap))
> - **Community Maintainer:** [anthony-spruyt](https://github.com/anthony-spruyt) ([anthony-spruyt/enshrouded-livemap](https://github.com/anthony-spruyt/enshrouded-livemap))
> - **Fork Features:** Discord chat bridge integration & automated standalone CI/CD workflows, developed with AI-assisted binary reverse engineering (Antigravity).

## About this fork

This is a fork of [oggii/enshrouded-livemap](https://github.com/oggii/enshrouded-livemap).
It exists because upstream only accepts the game builds listed in its source and has no CI, so every Enshrouded server update stops the map until someone patches and rebuilds it.
`main` is upstream's latest release plus the community compatibility patches for newer server builds (written by goakiller900 on [upstream issue 1](https://github.com/oggii/enshrouded-livemap/issues/1); see `docs/COMPAT-*.md`).
Upstream's later, unreleased work is reverted on `main` until it is reviewed.
Releases are cut by release-please; GitHub Actions builds, tests and publishes them and checks them against the game executable.
Only feat, fix, perf and refactor commits release. A PR that changes what ships (`src/`, `cmake/`, build lines in `CMakeLists.txt`, `data/builds.json`, `package/`, `web/`, `tools/compat/`) uses one of them; CI, test, docs and sync changes use ci, test, docs or chore.
Compatibility patches are still written by hand.

The Enshrouded dedicated server builds this fork supports are listed in `data/builds.json`.

HD map users: take the HD asset from upstream's latest release. This fork does not build one.

### Game patches and the update robot

A new server build goes through these steps. For now, a person runs each step by hand.

1. `exe-capture.yaml` saves the new build's server exe to the exe store. It takes the same two inputs as `compat.yaml` and only accepts Steam's current public build, so run it before the next patch ships. The next build is judged against this exe.
2. `compat.yaml` judges the new build against the last supported one.
3. A PR that changes only `data/builds.json` runs the `data-only` check and the boot test. The check runs only when that file is the only change, and passes when it is valid against its schema. The boot test starts the real server with the built DLL and checks that the map comes up. It always boots the server Steam serves now, and fails when no row of `data/builds.json` matches that server.
4. release-please opens a release PR. Merging it cuts the release.

The robot that chains these steps on its own is still being built (see [spruyt-labs issue 4105](https://github.com/anthony-spruyt/spruyt-labs/issues/4105)).

To run `compat.yaml` by hand, open the Actions tab, pick "Compat" and press "Run workflow".
Or run `gh workflow run compat.yaml -f build_id=<id> -f manifest_gid=<gid>`.
It takes two inputs:

- `build_id`: Steam's public build id of the server (app 2278520).
- `manifest_gid`: the public manifest gid of depot 2278521 for that build.

Read both on [SteamDB](https://steamdb.info/app/2278520/depots/), or from Steam's info API:

```sh
curl -fsS https://api.steamcmd.net/v1/info/2278520 | jq '.data["2278520"].depots | {build_id: .branches.public.buildid, manifest_gid: .["2278521"].manifests.public.gid}'
```

The job refuses a build that is not Steam's current public one.
It uploads the `livemap-compat` artifact. The artifact holds `report.json`, and, when the result is `known` or `easy`, the `builds.json` to commit in a PR.
Copy that file to `data/builds.json` and open the PR titled `fix(builds): add server build <id>` for a `known` build or `fix(builds): support server build <id>` for an `easy` one. Only a release ships the new build, so the type must be `fix`.
The artifact never holds game files.

The `result` in `report.json` is one of:

- `known`: the exe is one that a row in `data/builds.json` already lists. Only the build id and its manifest are new, so `builds.json` adds the id to that row and sets the row's manifest to the new build's. If the build id is listed already, the file is unchanged and there is nothing to commit.
- `easy`: every pattern matches once, and the code around each site is the same as in the last supported build, apart from addresses. `builds.json` gets a new row.
- `hard`: anything else. There is no `builds.json`. `report.json` lists each site that differs and how, and a person writes the patch.

`tools/compat/README.md` has the exact checks.

Once the robot runs, each build it handles gets one comment on the pinned [tracking issue](https://github.com/anthony-spruyt/enshrouded-livemap/issues/28): the result, links to the runs and the release, and how long each step took.

The text below is upstream's.

# Enshrouded Live Map

A server-side mod for Enshrouded dedicated servers. The server serves a web map: players with
names and walking direction, in-game clock and real time, weather, all map markers from the
game's data, ore deposits players mined and chests they opened. Players install nothing; they
open a link in the browser.

## Install

The mod is `iphlpapi.dll` next to `enshrouded_server.exe`, plus the `livemap` folder (page,
map tiles, icons, marker data).

- **Windows server:** stop the server, copy the files, start it.
- **Linux / Docker (Wine):** also add `iphlpapi=n,b` to `WINEDLLOVERRIDES`
  (several mods: `WINEDLLOVERRIDES=powrprof=n,b;iphlpapi=n,b`).

On the first start the mod writes `livemap.ini` with a random key and logs the link to share.

## Share the map

You share one link. The mod writes it to `livemap.log`:

```text
map link: http://203.0.113.5:15637/?key=k3t9x...
```

and shows it in the game chat when a player joins, in a short form to type:
`Live map: 203.0.113.5:15637/k3t9x...`. The game chat cannot copy text, so share the full link
from the log once (Discord, messenger) and let people bookmark it.

The link has three parts, and the mod fills in all three itself:

| Part | Where it comes from | What you do |
| --- | --- | --- |
| Address | the server's public IP, asked from api.ipify.org once an hour | nothing; or set `PublicUrl` |
| Port | `Port=auto`: the same number as the game's `queryPort`, but TCP | open that port for **TCP** too |
| Key | `Key=` in `livemap.ini`, random on the first start | nothing; or set a word you like (see below) |

### Open the port for TCP

The game itself only uses UDP on its query port. The map uses TCP on the same number, so you
usually extend the rule you already have:

- **Home server:** in the router, set the port forwarding of the game port to "TCP and UDP"
  (often called "Both").
- **Docker:** next to `15637:15637/udp` add `15637:15637/tcp`.
- **Rented server with a Pterodactyl panel:** the allocated port is usually open for TCP and
  UDP already; nothing to do.
- **Other hosts:** ask the support to open TCP on the game port, or for one extra TCP port and
  set `Port=` to it.

Check from outside your network (for example on your phone without Wi-Fi): the link must
open the map. Without `?key=...` the server answers 403, which is fine.

### Your own key

`Key=` can be any word: 3 to 64 letters, digits, `-` or `_`, case does not matter.

```ini
Key=ourmap
```

Players can then type `203.0.113.5:15637/ourmap` (or `map.example.com/ourmap` with
`PublicUrl`); the map forwards that to the full link. A short word is easier to type but easier to
guess, and everyone with the key sees all player positions. Changing the key locks old links out.

### A hostname or HTTPS

If the server has a DNS name, or you put the map behind a reverse proxy or a tunnel (for
example a Cloudflare Tunnel to `http://localhost:<Port>`), set

```ini
PublicUrl=https://map.example.com
```

The chat and the log then show `https://map.example.com/?key=...`. `PublicUrl` must start with
`http://` or `https://`; one without a scheme is ignored, the log says so, and the public IP is
used instead. `DetectPublicAddress=0` stops the IP lookup entirely.

### Good to know

- Everyone with the link sees all player positions and Flame Altars. Share it with your players only; change
  `Key=` to revoke it.
- Without a proxy the map is plain HTTP.
- Links in the game chat cannot be clicked: type them, or copy them from `livemap.log`.

## Settings (`livemap.ini`)

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | `1` | `0` turns the mod off. |
| `Port` | `auto` | `auto` = the game's `queryPort` number over TCP; or a fixed number. |
| `PublicUrl` | empty | Address for the link, starting with `http://` or `https://`, e.g. `https://map.example.com`. Empty = public IP. |
| `DetectPublicAddress` | `1` | `1` = ask api.ipify.org (then icanhazip.com) for the public IP, once an hour. |
| `AnnounceInChat` | `1` | `1` = the chat shows the map link when a player joins. |
| `Bind` | `0.0.0.0` | Network interface; `127.0.0.1` = only this machine (behind a proxy). |
| `Key` | random | Access key in the link: 3 to 64 letters, digits, `-` or `_`; case does not matter. |
| `UpdateMs` | `250` | How often the page gets new positions (100 to 5000). |
| `Finds` | `1` | Ore deposits and opened chests on the map. |
| `Bases` | `1` | The players' Flame Altars on the map. |
| `Discovered` | `1` | Which places each player discovered and where they have been (filter and fog of war). |
| `Log`, `LogDiagnostics` | `1` | Log file and periodic diagnostics. |

### Discord Chat Bridge (`[discord]` section)

| Key | Default | Meaning |
| --- | --- | --- |
| `WebhookUrl` | empty | Discord Webhook URL. When set, player chat in-game is forwarded to your Discord channel. |
| `EnableBridge` | `1` | `1` enables the `/api/chat` REST endpoint and Discord webhook forwarder. |
| `BotName` | `Enshrouded` | Username displayed by the Discord webhook. |

#### REST Chat API

- **Inbound (`POST /api/chat?key=<Key>`)**:
  Broadcasts a message into the game server chat:
  ```json
  {
    "sender": "DiscordUser",
    "message": "Ready to take down the Fell Monstrosity?"
  }
  ```
  Appears in-game as: `[DiscordUser] Ready to take down the Fell Monstrosity?`.
- **Outbound (`GET /api/chat?key=<Key>`)**:
  Returns the recent in-game chat messages as a JSON array.

## The log

`livemap.log` is next to the server exe and is written while `Log` is `1`. Each line starts with a
UTC timestamp and ends in CRLF.

When the process hits a fault in the mod's own code, in the memory of one of its hooks, or in game
code that one of its hook callbacks called, the mod writes one line first, at most once per run,
and does not stop the fault: the process ends the normal way.

```text
2000-01-02 03:04:05.678 [livemap] FAULT code=0xC0000005 at=livemap+0x000014A8 site=movement
```

| Field | Meaning |
| --- | --- |
| `code` | The exception code: `0x` and 8 upper-case hex digits. |
| `at` | `livemap+0x...` is the offset from the start of `iphlpapi.dll`, 8 upper-case hex digits. `trampoline+0x...` is the offset into the memory holding one hook's stub and moved game instructions; the same hex form. `game+0x...` is the offset from the start of the game exe, for a fault in game code while one of the mod's hook callbacks was running; the same hex form. |
| `site` | The name of the hook whose callback or trampoline was running, or `none` when the fault is in the mod but in no hook (the names are in `src/world.cpp` and `src/announce.cpp`); a `game+0x...` line always names the hook. |

The line tells a crash in the mod, or in game code a hook callback called, from a crash in the game
on its own, which writes no line. A bad read that the mod handles itself is not a fault and writes
no line.

## The map

The map is drawn from the game's own map data (the textures the in-game map is made of): height,
relief shading, contour lines and the shroud zones, at 1 metre per pixel when zoomed in. The
icons are the game's own map markers. `tools/render_map.py` and `tools/extract_icons.py` make
them from the game files; see `docs/RESEARCH.md`, section 13. The page wears the game's own
map-screen look: its gold frame, separators and player arrow (`livemap/ui/`, from the client's UI
textures) and Alegreya (`livemap/fonts/`, SIL Open Font License), the closest free match to the
game's Harmonique Display.

You can add your own map picture as a second style in `livemap/maps.json`:

```json
{ "name": "My map", "file": "my-map.jpg", "bounds": [[0, 0], [10240, 10240]] }
```

`bounds` is the part of the world the picture covers, in metres (`[[south, west], [north, east]]`).
If it does not sit right, open the map with `&calibrate` added to the link and follow the steps.

Looted chests show as looted for 30 minutes, then as full again (what players see on dedicated
servers; the game data has no fixed time). Chests inside a Flame Altar's build zone stay looted.
Change the time with `"chestRefillMinutes": 60` at the top level of `maps.json`; `0` = never.
The marker panel has switches for the rain and snow overlay and for deadly shroud: shroud zones
above the world's Flame level, drawn pink as on the game's map (zone levels from the game's map
data, `tools/extract_shroud.py`; the Flame level is the first keyword of the server's A2S_INFO).

Click a player in the list to follow them (click again or drag the map to stop). Add
`&follow=<name>` to the link to follow that player whenever they are online.

## Works with Shared XP

Shared XP 1.1.3 or newer. Both mods add a line to the chat after "Player X joined." and can run together; whichever starts
second adds its hook on top of the first one.

## Build

With Visual Studio 2026 (MSVC 14.50) or newer with the C++ build tools (x64 Native Tools prompt), CMake, Ninja and Python 3,
these commands build `build\iphlpapi.dll`, run the unit tests and build `build\livemap_selftest.exe`:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure --no-tests=error
```

The selftest checks a server exe on disk: `livemap_selftest.exe "C:\path\to\enshrouded_server.exe"`.

The `tools_*` unit tests run the Python tools' tests with pytest. Install
pytest and jsonschema once with `python -m pip install --require-hashes --no-deps -r tools\requirements-test.txt`.

The supported server builds, their hook signatures and the RVAs the selftest expects live in
`data/builds.json` (schema: `data/builds.schema.json`). CMake turns that file into the tables the DLL compiles in, so
supporting another build whose code the existing signatures still match is a data change.

CI runs `livemap_selftest` and `livemap_compat check` on the server exe of every build that lists an `exe_sha256`, and
refuses a change to a build without one. The hook checks are in [`tools/compat/README.md`](tools/compat/README.md).

A pull request that is a plain edit of `data/builds.json` and changes nothing else runs the `data-only` check, which
passes when the file validates against the schema. Code pull requests skip it and merge as usual. The base commit's
`tools/data_only.py` classifies the pull request.

A pull request that changes `data/`, `src/`, `cmake/`, `CMakeLists.txt` or the boot test's own files (`tools/boot_test.py`,
`tools/data_only.py`, `ci-repo.yaml`) also runs the `boot-test` job. It downloads the server Steam serves now: the depot
that `data/builds.json` records, without a manifest, so Steam hands out its current public build. It hashes
`enshrouded_server.exe` and picks the row of `data/builds.json` whose `steam.exe_sha256` matches; the row gives the
sigset, and so the mandatory hooks, that the job checks. When no row matches, the job fails with a message that Steam's
current server is in no row, so the map does not support it yet. It starts the server in the
`mornedhels/enshrouded-server` image, first without the DLL and then with the built one. The server must be started by
its supervisor exactly once, so a crash that the supervisor retries fails the job. The run without the DLL passes when
the game answers its query port and stays up for a minute. The run with the DLL passes when the server reaches RUNNING,
`livemap.log` lists every mandatory hook, the map serves fresh data, and the server runs for 10 minutes without a crash
or exit. The game files stay on the runner and are never uploaded or cached; `tools/boot_test.py` holds the checks.
