# CLAUDE.md

Personal, private LandSandBoat (FFXI server emulator) fork: `italonvcc/server_ffxi`.
`upstream` is LandSandBoat/server. Read `AGENTS.txt` / `docs/ai_agents/README.md` for the
upstream project's expectations; this file covers how this fork is run and worked on.

## Current project: bot players

The main work in this fork is **bot players**: server-controlled characters that the client
sees as real players, to make soloing viable. Everything lives in `modules/bots/`, and
**`modules/bots/README.md` is the design doc**: architecture, DB tables, command queue,
dashboard, known gotchas and next steps. Read it before touching bot code.

Keep bot work in `modules/bots/` (C++ module + Lua + dashboard). Only change core
`src/` code when a module hook can't do it, and say so when you do.

## Working agreements

- **Restart servers without asking.** When a change needs a new `xi_map.exe`, stop the
  running one, deploy, and start it again, even if the user's character is logged in. Just
  mention it in the reply.
- Commit, push and open PRs only when asked. PRs go to **`italonvcc/server_ffxi`, base
  branch `base`**. The repo is a fork, so always pass `--repo italonvcc/server_ffxi` to `gh`
  or it targets upstream LandSandBoat.
- Verify in the running server (logs, DB, dashboard API). In-client checks are the user's;
  say what you couldn't verify.

## Build (Windows, this machine)

Ninja Multi-Config in `build/`, configuration `RelWithDebInfo`. The VS dev environment
must use MSVC **14.44** (the default 14.52 fails to link with `LNK1104 msvcprt.lib`) and
needs `vswhere` on PATH:

```bat
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 -vcvars_ver=14.44
cmake --build build --config RelWithDebInfo --target xi_map
```

The last build step copies `xi_map.exe` to the repo root. It fails with "Permission denied"
while xi_map is running (the exe is locked); compile and link have still succeeded. Editing
`modules/init.txt` re-runs CMake configure; adding or removing a module `.cpp` does not,
so run `cmake build` first or the new file is silently left out.

## Running the servers

- All five servers start from `start_servers.bat` (repo root), each in its own console.
- Restart only the map server after a rebuild:
  1. stop `xi_map.exe`
  2. copy `build/src/map/RelWithDebInfo/xi_map.exe` and `.pdb` to the repo root
  3. start `xi_map.exe` with the repo root as working directory
  4. wait for `The map-server is ready to work` in `log/map-server.log` (can take 15-110 s)
- Crash stack traces go to the console (stdout), not the log file. To catch one, run
  xi_map from a shell with output redirected.
- Logs: `log/map-server.log` (also world/search/connect/profile).

## Database

MariaDB at `C:\Program Files\MariaDB 13.0\bin\mariadb.exe`, database `xidb`. Credentials
are in `settings/network.lua` (gitignored, never commit or echo the password).

Things to know:
- Triggers: `char_insert` creates most per-character rows; `char_delete` removes them.
  Delete characters through `chars` only.
- `accounts_sessions` = who is online (what `/sea` lists). `accid` is unique.
- The user's own characters are on account `kenji1997` (e.g. Kenji, Maya). Never modify
  or delete them; bots are only the characters listed in `bot_players`.

## Bot dashboard

```
python modules/bots/dashboard/dashboard.py
```

http://127.0.0.1:8088. JSON API: `GET /api/state`, `GET /api/bots/<charid>`,
`GET /api/zones`, `POST /api/commands` (queues a `bot_commands` row),
`GET /api/commands/<id>`. Use the API (curl) to drive and check bots from a session.
