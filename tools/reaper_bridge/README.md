# REAPER bridge

Lets Claude Code read and drive a REAPER on this machine. Two parts talk through a private folder, `~/.furrow-bridge`.

- `bridge.lua` runs inside REAPER. It polls `~/.furrow-bridge/in` for request files, runs each one and writes `~/.furrow-bridge/out/<id>.res`.
- `server.py` is an MCP server. It turns Claude Code tool calls into request files and reads the answers.

## What it can do

| Tool | Does |
| --- | --- |
| `reaper_ping` | Checks the bridge is running and returns the REAPER version |
| `reaper_state` | Tracks, FX, sends, media items with Furrow metadata, transport, cursor, tempo, undo labels |
| `reaper_eval` | Runs Lua in REAPER with the `reaper` API in scope |
| `reaper_action` | Runs an action by number or named command |
| `reaper_transport` | Play, stop, pause, or move the edit cursor |
| `reaper_render` | Renders a time range of the master mix offline, then lists each note onset and the peak level |
| `reaper_analyze` | Does the same analysis for an existing audio file |
| `reaper_shutdown` | Stops the bridge |

## Security

Anything that can write a request file can run Lua in REAPER with your permissions. That means it can read and change your projects and files. So:

- `~/.furrow-bridge` is created mode 700 and the client refuses to use it if it is not private to you.
- Start the bridge on purpose, in a scratch project. No Furrow script starts it.
- It will not start with unsaved changes, unless `~/.furrow-bridge/allow_dirty` exists.
- A render never writes outside `~/.furrow-bridge/render`.

## Set up

1. A Python with the MCP package, for example `python3 -m venv ~/.furrow-bridge-venv && ~/.furrow-bridge-venv/bin/pip install mcp`. The server uses `mcp` 2.x.
2. Register the server with Claude Code: `claude mcp add reaper-bridge -- ~/.furrow-bridge-venv/bin/python /path/to/furrow/tools/reaper_bridge/server.py`. Starting the server creates the folder.
3. In REAPER, add `tools/reaper_bridge/bridge.lua` to the action list and run it. The console says it is running. Run it again to stop it.
4. Ask Claude Code to call `reaper_ping`.

Render needs `ffmpeg` on the path for the audio analysis.

## What it does not prove

Offline render is not the real-time audio thread. It runs faster than playback and block timing differs. It can show which notes sound and when. It cannot show that the real-time ordering of two tracks is safe. Listening in real time remains the check for that.
