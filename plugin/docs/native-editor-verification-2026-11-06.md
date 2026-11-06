# Direct native editor verification, 2026-11-06

A WebView-enabled Debug Standalone process was launched on the existing Xorg display and targeted by X11 window ID `0x540000f`. The editor window rendered at 728x454.

Evidence images:

- `verification-2026-11-06/native-editor-initial.png`: initial rack and step grid.
- `verification-2026-11-06/native-editor-clear-steps.png`: `Clear steps` focused and Kick hits removed while other lanes remained.
- `verification-2026-11-06/native-editor-toggle-step.png`: a Kick step restored through the grid control.
- `verification-2026-11-06/native-editor-add-lane.png`: `Add lane` created `Lane 5` with its own row.
- `verification-2026-11-06/native-editor-save-dialog.png`: `Save kit` opened the native GTK `Choose file` dialog.

This directly verifies rendered WebView content, supported step interaction, lane creation and native save-dialog routing. It does not verify drag/drop, audio output, MIDI routing in a host, persistence after closing, or the WebKit critical diagnostics emitted by pluginval subprocesses. The GDB lifecycle log is retained in `verification-2026-11-06/standalone-gdb-lifecycle.log`; the bounded debugger run did not produce a WebKit critical stack trace.
