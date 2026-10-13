"""An MCP server that lets Claude Code read and drive a REAPER running the bridge script.

Run it with the Python that has the `mcp` package. It creates ~/.furrow-bridge with private
permissions. In REAPER, start tools/reaper_bridge/bridge.lua from the action list."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from analysis import analyze  # noqa: E402
from client import Client  # noqa: E402

from mcp.server.mcpserver import MCPServer  # noqa: E402

mcp = MCPServer("reaper-bridge")
_client = None


def client():
    global _client
    if _client is None:
        _client = Client()
    return _client


@mcp.tool()
def reaper_ping() -> dict:
    """Checks that the bridge script is running in REAPER and returns the REAPER version."""
    return client().call("ping", timeout=5)


@mcp.tool()
def reaper_state() -> dict:
    """The project's tracks, FX, sends, media items (with Furrow metadata), transport, cursor, tempo and undo labels."""
    return client().call("state")


@mcp.tool()
def reaper_eval(code: str) -> object:
    """Runs Lua code inside REAPER with the `reaper` API in scope and returns its return value. Runs with the user's permissions, so keep it to the scratch project."""
    return client().call("eval", code=code)


@mcp.tool()
def reaper_action(command: str) -> dict:
    """Runs a REAPER action by number (as a string of digits) or by named command such as _RS1234."""
    return client().call("action", command=int(command) if command.isdigit() else command)


@mcp.tool()
def reaper_transport(action: str = "seek", position: float | None = None) -> dict:
    """Controls playback. action is play, stop, pause or seek. position (seconds) moves the edit cursor first."""
    params = {"action": action}
    if position is not None:
        params["position"] = position
    return client().call("transport", **params)


@mcp.tool()
def reaper_render(stop: float, start: float = 0.0, name: str = "render") -> dict:
    """Renders the master mix from start to stop seconds to a file offline, then returns the audio analysis: peak level and the time and level of each note onset."""
    result = client().call("render", timeout=120, start=start, stop=stop, name=name)
    return analyze(result["path"])


@mcp.tool()
def reaper_analyze(path: str) -> dict:
    """Analyses an existing audio file for note onsets."""
    return analyze(path)


@mcp.tool()
def reaper_shutdown() -> dict:
    """Stops the bridge script in REAPER."""
    return client().call("shutdown", timeout=5)


if __name__ == "__main__":
    client()  # creates the private spool folder before REAPER's script looks for it
    mcp.run()
