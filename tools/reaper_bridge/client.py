"""The Python side of the REAPER bridge protocol. No third-party imports.

A request is a file in <spool>/in named <id>.req holding JSON {"op": ..., ...}. The bridge
script in REAPER writes <spool>/out/<id>.res holding {"id", "ok", "result" | "error"}."""

import json
import os
import stat
import time
import uuid
from pathlib import Path

DEFAULT_SPOOL = Path.home() / ".furrow-bridge"


class BridgeError(RuntimeError):
    pass


def ensure_spool(spool=DEFAULT_SPOOL):
    """Creates the spool folders private to the current user and refuses a folder that is not."""
    spool = Path(spool)
    for sub in ("", "in", "out", "render"):
        folder = spool / sub if sub else spool
        folder.mkdir(mode=0o700, parents=True, exist_ok=True)
    (spool / "in" / ".keep").touch()
    info = spool.stat()
    if info.st_uid != os.getuid():
        raise BridgeError("%s is owned by another user" % spool)
    if info.st_mode & (stat.S_IRWXG | stat.S_IRWXO):
        raise BridgeError("%s must be private to you. Run: chmod 700 %s" % (spool, spool))
    return spool


class Client:
    def __init__(self, spool=DEFAULT_SPOOL):
        self.spool = ensure_spool(spool)

    def send(self, op, **params):
        """Writes a request and returns its id."""
        request_id = uuid.uuid4().hex[:16]
        body = dict(params, op=op)
        temp = self.spool / "in" / (request_id + ".tmp")
        temp.write_text(json.dumps(body))
        os.replace(temp, self.spool / "in" / (request_id + ".req"))
        return request_id

    def read(self, request_id):
        """The response for an id, or None when it has not arrived."""
        path = self.spool / "out" / (request_id + ".res")
        if not path.exists():
            return None
        response = json.loads(path.read_text())
        path.unlink()
        return response

    def call(self, op, timeout=15.0, pump=None, **params):
        """Sends a request and waits for its result. pump is called while waiting, for tests."""
        request_id = self.send(op, **params)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if pump:
                pump()
            response = self.read(request_id)
            if response is not None:
                if not response.get("ok"):
                    raise BridgeError(response.get("error", "the bridge reported an error"))
                return response.get("result")
            time.sleep(0.03)
        raise BridgeError("no answer from REAPER in %.0f s. Is the bridge script running?" % timeout)
