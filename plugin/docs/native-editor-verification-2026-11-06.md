# Direct native editor verification, 2026-11-06

A WebView-enabled Debug Standalone process was launched on the existing Xorg display and targeted by X11 window ID `0x540000f`. The editor window rendered at 728x454.

Evidence images:

- `verification-2026-11-06/native-editor-initial.png`: initial rack and step grid.
- `verification-2026-11-06/native-editor-clear-steps.png`: `Clear steps` focused and Kick hits removed while other lanes remained.
- `verification-2026-11-06/native-editor-toggle-step.png`: a Kick step restored through the grid control.
- `verification-2026-11-06/native-editor-add-lane.png`: `Add lane` created `Lane 5` with its own row.
- `verification-2026-11-06/native-editor-save-dialog.png`: `Save kit` opened the native GTK `Choose file` dialog.

This directly verifies rendered WebView content, supported step interaction, lane creation and native save-dialog routing. It does not verify drag/drop, audio output, MIDI routing in a host, persistence after closing, or the WebKit critical diagnostics emitted by pluginval subprocesses. The GDB lifecycle log is retained in `verification-2026-11-06/standalone-gdb-lifecycle.log`; the bounded debugger run did not produce a WebKit critical stack trace.

A second batch-mode GDB run wrapped pluginval strictness 10 with `G_DEBUG=fatal-criticals`. It followed forks into `WebKitNetworkProcess`, detaching from the JUCE WebKit child and validator. GDB exited before the detached validator finished writing the log. The eventual log ends in `SUCCESS`, but no critical-emitting subprocess stack was captured. The earlier inference that this narrowed the issue to a harmless or non-fatal lifecycle diagnostic was unsupported. The raw run is `verification-2026-11-06/pluginval-webkit-gdb.log`.

## Native file workflow continuation

At 04:15 UTC, process inspection revealed that the original Save kit chooser was still open behind the editor. Earlier unscoped Escape attempts had not cancelled it, and `choosingFile` correctly prevented a second chooser. Explicitly focusing that chooser and cancelling it allowed Choose sample to open a WAV-filtered dialog.

Selecting the scratch `furrow-drop-test/fixture.wav` through that dialog caused the inspector to display its full path with no visible sample error. This verifies native selection and backend state delivery, not audible output. See `verification-2026-11-06/native-sample-selected.png`.

Save kit then wrote a new 1,641-byte scratch file `native-kit-0416.furrow` after explicitly clicking the save dialog's OK button. The lane name was changed to `Native reload probe`. Loading the saved file through the explicitly focused Load kit chooser restored `Kick` and retained the WAV path. Before/after images are `native-kit-name-mutated.png` and `native-kit-restored.png` in the same evidence directory. This establishes a native dialog kit round-trip, not host-project persistence after process restart.

Drag/drop attempts remain inconclusive. Initial attempts used unsupported pointer options or obscured targets. A later side-by-side attempt did not display a sample path on Lane 5. The retained `native-editor-drag-drop.png` is an unsuccessful-attempt screenshot, not acceptance evidence. `native-editor-inspector-scroll.png` and `native-editor-sample-controls.png` document navigation only.

Export MIDI was also exercised through the native dialog. The resulting scratch file `native-pattern-0422.mid` is 352 bytes and begins with the Standard MIDI header `MThd`, one track, and 960 ticks per quarter note. The subsequent Preview MIDI chooser interaction was inconclusive because the chooser closed before the exact file was selected. Backend MIDI preview and replacement remain covered by C++ tests.
