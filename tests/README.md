# Tests

These tests run SeqCore in [ysfx](https://github.com/JoepVanlier/ysfx), an independent JSFX host. The real Lua modules drive it through named gmem, as they do in REAPER. A pass shows the EEL2 code and the Lua protocol agree. It does not show REAPER behaves the same, so REAPER runs still decide host behaviour.

Build the host library once. It needs git, a C++17 compiler and cmake.

```sh
tools/jsfx_host/build.sh
```

Then run the engine, rack, commit, capture, gate, and bridge suites with a Python environment that has `lupa` installed.

```sh
cd tests
python -m unittest test_seqcore test_rack test_commit test_capture test_gate test_bridge
```

- The Python environment needs `lupa`. The engine and gate suites also need the library built by `tools/jsfx_host/build.sh`.
- `test_seqcore.py` covers wire format 1, transactions, snapshots, playback timing, mute and solo, the full ring, pool exhaustion, undo, and state format 1.
- `test_rack.py` runs the coordinator, host, and window code against a stub REAPER, with SeqCore underneath. The stub records calls. It does not prove REAPER honours them.
- `test_commit.py` compares the MIDI renderer with SeqCore and checks commit snapshot encoding and fingerprints.
- `test_capture.py` checks group-reference and fingerprint decisions, MIDI event scanning and conversion preview rules. `test_rack.py` also exercises selected-item scanning, source restoration, conversion application, new-lane routing, and the visible Capture action against the REAPER stub. These harness checks do not replace manual REAPER verification.
- `test_gate.py` checks that the item gate and SeqCore never send note-ons at the same time in either processing order. It runs in ysfx, not REAPER.
- `test_bridge.py` checks the bridge protocol, REAPER API stubs, rendering, and analysis.

Run the standalone scheduling, engine-bit, and Lua-host probe checks from the repository root:

```sh
python3 probes/check_schedule.py
python3 probes/check_engine_bits.py
python3 probes/check_lua_scripts.py
```

The Lua-script probe also needs `lupa`. The full unittest command and these probe checks are separate; passing the stubs does not establish REAPER host behavior.


`FakeImGui` in `test_rack.py` accepts any arguments. It cannot catch an assertion that real ReaImGui raises for a bad argument or flag, such as the one `InputInt` raises for the Enter-only flag. Run each new widget in REAPER before trusting it.
