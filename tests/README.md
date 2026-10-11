# Tests

These tests run SeqCore in [ysfx](https://github.com/JoepVanlier/ysfx), an independent JSFX host. The real Lua modules drive it through named gmem, as they do in REAPER. A pass shows the EEL2 code and the Lua protocol agree. It does not show REAPER behaves the same, so REAPER runs still decide host behaviour.

Build the host library once. It needs git, a C++17 compiler and cmake.

```sh
tools/jsfx_host/build.sh
```

Then run the suites with a Python that has `lupa` installed.

```sh
cd tests
python -m unittest test_seqcore test_rack
```

- `test_seqcore.py` covers wire format 1, transactions, snapshots, playback timing, mute and solo, the full ring, pool exhaustion, undo and state format 1.
- `test_rack.py` runs the coordinator, host and window code against a stub REAPER, with SeqCore underneath. The stub records calls. It does not prove REAPER honours them.
