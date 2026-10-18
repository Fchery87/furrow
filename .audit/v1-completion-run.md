# V1 completion run

## Todos

- [x] Read the Principles section of the poteto-mode skill.
- [x] Phase A: Frame. Verify the requirements, scope, definition of done, and release blockers before the long verification run.
- [x] Phase B: Design the workflow. Order checks by risk and write the phase plan before changes.
- [x] Phase C: Run the loop. Verify one unit before starting the next.
- [x] Phase D: Keep the audit trail. Append decisions and evidence to `.audit/v1-completion.tsv`.
- [x] Phase E: Verify and hand back. Recheck the release predicate on the actual project and report open gates.

## Definition of done

Every documented V1 requirement maps to current implementation and completed automated or host evidence. The full documented test command exits zero with a complete unittest summary. Required timing, CPU, item-gate, recovery, installation, and platform gates have evidence, or the report names the exact external prerequisite and reproduction steps. No unavailable host or operating-system check is represented as a pass.

## Scope and throughput checkpoint

The release scope has eight workstreams from the V1 request: (1) side-window snapshot identity, (2) capture, (3) instrument lanes, (4) Panic, (5) item gate, (6) packaging and installation, (7) full release validation, and (8) specification and test-guide alignment. The reported baseline is about one hour of prior implementation plus a long full suite. Current work is verification-first. Host and OS checks may require separate machines or hardware.

- Blocking first steps: verify repository state, test prerequisites, and documented commands before running the long suites.
- Independent workstreams: n/a for implementation fan-out. All work uses the same REAPER scratch project and shared working tree, so verification and edits remain serialized.
- Shared mutable state: protect the shared REAPER project and worktree. Use scratch projects and inspect existing diffs before edits.
- Smallest safe decomposition: one owner is safest because host behavior, code, and release evidence share the same REAPER instance and project metadata.

## Phases

1. Read project instructions, requirements, tests, and audit notes. Record the actual worktree and host baseline.
2. Run the documented full test suite and Lua checks to completion. Capture complete output and exit status.
3. Verify current REAPER and Linux host evidence. Read the saved startup-v2 offline-recovery results and prior host records. Use a disposable tab or project for mutable host checks and preserve dirty projects.
4. Compare timing, CPU, installation, packaging, dependency, and platform requirements with available evidence. Run supported checks. Mark external prerequisites as blocked with exact steps.
5. Fix only confirmed in-scope defects. Add focused regression coverage and rerun each affected check, then rerun applicable full suites.
6. Update the spec, test guide, root development-install documentation, and audit log where requirements and verified behavior support exact claims. The indexer install was authorized, but the approved prerequisite install could not run because sudo requires an interactive password.
7. Inspect the final diff, verify the original `Testing-furrow.RPP` remains untouched, audit the log against this run, and report readiness and blockers.

## 18 October continuation notes

- The reported 163-test full-suite pass and 54-test isolated SeqCore pass were reproduced before current-turn edits. The post-edit documented suite completed 165 tests in 886.157 seconds with `OK` and exit status 0. Its full log is `/tmp/furrow-v1-full-suite-final-2026-10-18.log`.
- Added a REAPER-track color stripe because the V1 product table and milestone notes explicitly include lane color. Candidate review selected a display-only stripe keyed by the existing stable lane-to-track resolution; no persistent color state was added.
- Added an incompatible-ReaImGui API smoke test and entry-action error handling. Its initial red run raised the expected unsupported-API error; the focused green run passed.
- `diag/gate.txt` contains only an item-gate mode-word probe result. The previous host handoff records direct-edit and rendered-isolation cases, but a single repeatable full matrix artifact is not present. Do not mark that matrix fully closed.
- Startup-v2 evidence on REAPER 7.82/Linux records 21 results and no failures, including offline/online with a held note and 4/4 receiver note-off plus CC123. It validates engine startup recovery, not the rack-window Panic action.
- The ReaPack indexer is not installed. Its user-scoped install and system prerequisites were authorized, but the prerequisite command could not complete because sudo requires an interactive password. Index generation against a committed, versioned source snapshot and clean-profile installation remain release gates. No REAPER global configuration was changed.
- The actual REAPER 7.82/Linux keymap labels this checkout `Custom: Furrow_Rack.lua`, while source declares `Furrow: Open rack`. The development README points to the loaded script entry. Verify the ReaPack action label in a clean profile.
- Current run verdict: V1 is not ready. Functional automation passes, but the full Linux host matrix artifact, remaining timing cases, 10-minute CPU target, coordinator recovery, third-party instrument, clean installation, ReaPack index, supported-version floor, and Windows/macOS validation remain open.

## 18 October release-readiness continuation

### Todos

- [x] Read the Principles section of the poteto-mode skill.
- [x] Phase A: Frame. Verify documented requirements, the current worktree, available tools, and release blockers.
- [x] Phase B: Design the workflow. Order independent checks by risk and record the run plan before making changes.
- [x] Phase C: Run the loop. Reproduce supported checks and verify any focused correction before moving on.
- [x] Phase D: Keep the audit trail. Append decisions and evidence to `.audit/v1-completion.tsv`.
- [x] Phase E: Verify and hand back. Recheck the release predicate against the repository and available REAPER host.
- [x] Re-run the documented full unittest suite and standalone probe checks.
- [x] Validate which REAPER host checks are available without touching the active dirty project. Mark GUI-dependent behavior and the complete milestone matrix blocked when the desktop control failed to start.
- [x] Prepare only packaging work that does not require a committed versioned snapshot; leave index generation and release actions gated as required.
- [x] Update audit and user documentation with current evidence, blockers, and reproduction steps.
- [x] Inspect the final worktree and confirm `Testing-furrow.RPP` remains unchanged.

### Framing and workflow

Definition of done: each documented V1 release gate has fresh evidence or an exact prerequisite and reproduction procedure. V1 stays not ready while required installation, full host, timing, performance, or platform gates remain open. This run covers packaging, repeatable Linux host evidence, timing and CPU requirements, and platform validation. It does not include a commit, tag, index publication, or a system package install. The worktree contains uncommitted project changes and Ruby is absent. Preserve existing dirty projects and use disposable projects for mutable host checks.

Work order:

1. Confirm instructions, dirty worktree, release requirements, available host, and packaging prerequisites.
2. Run the full documented tests and probes with complete output and exit status.
3. Check whether a separate REAPER scratch tab can safely support additional host checks. Do not act on the active dirty project.
4. Verify each timing, CPU, platform, dependency, and packaging requirement against the documentation. Run only checks with the stated acceptance method and available setup.
5. Fix only a reproduced V1 defect. Add a focused regression check and rerun its suite.
6. Update the audit, spec, and installation docs with evidence from this run.
7. Inspect the final diff and original project status, then report readiness and exact remaining prerequisites.

### Current run findings

- The worktree is dirty with 14 modified tracked files and 5 untracked files. `HEAD` is `e429398`; no version tags exist. The original `Testing-furrow.RPP` is not modified.
- No `index.xml`, `.reapack-index.conf`, `reapack-index`, Ruby, or RubyGems executable is present. The user approved a user-scoped indexer install and then approved installing system prerequisites. The prerequisite command `sudo apt-get install -y ruby ruby-dev cmake` was attempted and failed because sudo requires an interactive password. No system package changed.
- The current REAPER host is 7.82/Linux. The active project was dirty and contained prior test tracks. A new project tab was opened and closed without saving; no existing project was saved or altered by the check.
- The spec requires a 27-case base timing grid from 3 rates, 3 block sizes, and 3 tempos, plus tempo-map, time-signature, start, loop, seek, stop/resume, offline-render and four-bus cases. Timing passes only within one sample of an independently derived project-sample expectation.
- The CPU check requires a 10-minute 32-lane workload at 48 kHz and 256 samples, with separate SeqCore measurement and a stated 1% target. The spec also asks for peak block duration but does not define its acceptance threshold or measurement method.
- This machine is Linux x86_64 on an Intel Core i5-2415M. Windows and Apple silicon macOS are unavailable here.
- The approved prerequisite install was attempted with `sudo apt-get install -y ruby ruby-dev cmake` and failed because `sudo` needs a terminal password. No system package was installed. The ReaPack indexer therefore remains unavailable.
- ReaPack's official indexer docs confirm that `@version` is mandatory and that package files need headers. The project currently has no package version or `@provides` declarations. Packaging order is: choose the V1 version and add package metadata/layout; validate installed paths and file headers; obtain approval for and create a committed versioned snapshot; then generate and validate the index. The current git `HEAD` is `e429398`, the working tree remains dirty, and no release tag exists. No index or package artifact can represent this uncommitted tree.
- Fresh automated verification completed. The documented unittest command ran 165 tests in 1,784.981 seconds with `OK` and exit 0. The schedule, engine-bit and Lua-host probes ran 14, 1 and 31 tests, all with `OK`. Logs and evidence are listed in `.audit/v1-completion.tsv`.
- Rechecking the saved 11 October timing files confirmed the constant and offline-render cases within one sample, and the first live ramp artifact still fails by two samples. These captures came from earlier engine commits, so they do not establish current-source behavior. The receiver dump sorts events and `diff_log.py` ignores lane and order fields, so event order and lane ownership remain unproven.
- The required 10-minute CPU run is blocked by the current 44.1 kHz/1,024-sample audio setup and absent isolated measurement procedure. Windows, macOS Apple silicon, third-party instruments, clean-profile install, Panic UI after offline recovery, edited-source UI choices, visual lane color and the full item-gate matrix remain unverified.
- This continuation aligns the offline lifecycle table with the current saved-held-note release path and explicitly records that the spec has no peak block-time threshold in `Furrow-Spec-and-Architecture.md`. `probes/README.md` now states the timing recorder's order/lane limitation. V1 remains not ready.
- The fresh automated suite and short probe outputs are persisted under `.audit/results/2026-10-18/` with the tested HEAD and worktree identity. REAPER GUI interactions are recorded in the TSV, but no standalone transcript was available; GUI-dependent observations remain run notes rather than independently replayable logs.

## Timing verifier continuation — 18 October 2026, 18:03 UTC

This continuation repaired the timing evidence path before attempting fresh host captures.

- `jsfx/furrow_probe_receiver.jsfx` is now gmem layout version 4 and records each receiver's local event sequence, receiver slot, lane mapping, observed destination MIDI bus, destination channel, note payload and event kind. Both Lua dump paths retain receiver order rather than sorting it away.
- `probes/schedule.py` calculates the expected event time from the written tempo map independently of SeqCore and records receiver/lane routing identity and expected destination bus. `probes/diff_log.py` now matches full observable event identity, applies only 0- or 1-sample tolerance, checks extras and missing events/releases, reports event/routing/timing details, and enforces the documented note-off/controller/note-on precedence when events share an observed offset.
- The spec's tie rule names event-class precedence but no ordering within a class or total ordering across receiver tracks. The implementation does not invent either. Source MIDI bus cannot be observed at the receiver after the REAPER send remaps it; the verifier uses receiver, lane mapping, destination bus and destination channel as its observable routing evidence.
- Deliberately wrong fixtures cover lane and receiver delivery, destination bus, simultaneous event order, missing note release, extra event and a two-sample timing error. The historical sorted v3 captures are explicitly refused as unverifiable, including the artifact with a two-sample-late final ramp note-off. No cause is inferred and the tolerance remains one sample.
- Fresh automated checks: `python3 probes/check_schedule.py` — 21 tests, pass; `python3 probes/check_engine_bits.py` — 1 test, pass; `/tmp/furrow-v1-venv/bin/python probes/check_lua_scripts.py` — 31 tests, pass; `git diff --check` — pass. `python3 -m py_compile probes/diff_log.py probes/schedule.py probes/check_schedule.py probes/check_lua_scripts.py` also exits 0. Logs are in `.audit/results/2026-10-18/timing-verifier/`.
- The tested source is uncommitted `HEAD=e4293986bafdfb4f340ab79184a7bb3b477dfa63`; scoped tracked diff SHA-256 at verification is `685806fd5fb675e8b2e3596e9f8f8c7ed23944748ddbc4d310516670138963dd`. `Testing-furrow.RPP` remains unchanged. No full timing matrix has been captured against the current source yet.
- The REAPER bridge is live on 7.82/Linux, but its current project is dirty. Action 40859 (New project tab) was rejected by automatic review because it could persist the dirty project. The user-scoped REAPER `furrow_probe_receiver.jsfx` and `furrow_seqcore.jsfx` Effects entries are symlinks to the workspace copies, so the edits already reach the current source path without a profile update. A request for explicit disposable-tab authorization is pending; no alternate API was used to bypass the action rejection.
- GUI automation diagnosis: `/dev/fuse` is absent, so the Orca AppImage cannot mount. `xdotool` and `wmctrl` are installed but cannot open the display (`Cannot open display`). No GUI package, system setting or REAPER profile was changed. The REAPER bridge can inspect live state, but does not by itself provide visual interaction.
- A read-only `GetAudioDeviceInfo` query confirmed REAPER is currently at 44,100 Hz and 1,024 samples per block. The required CPU workload at 48 kHz/256 samples was not run, and no audio settings were changed.

This supersedes the earlier note that the dump sorted captured rows and the comparator ignored lane/order. It does not supersede the historical timing results: those remain stale, and host timing acceptance is still open.


## Safe host setup gate — 18 October 2026, 18:17 UTC

- Fresh read-only bridge ping confirms REAPER `7.82/linux-x86_64`. `reaper_state` reports the active project is dirty and has nine tracks, with `undo_next` = `Create detach test item` and `redo_next` = `Detach test item`. It contains Furrow and prior test tracks. The project was not saved, closed, switched, or modified. No REAPER action, state-changing eval, transport command, or audio-setting change was attempted.
- The earlier automatic-review denial for action 40859 remains in force. This continuation did not retry that action through the bridge or another method. GUI automation is still unavailable per the prior `/dev/fuse` and display-access evidence.
- Current hashes for the timing receiver, Lua dump/render scripts, comparator, independent schedule, regression tests, and expected schedules match `.audit/results/2026-10-18/timing-verifier/source-identity.txt`. `HEAD` remains `e4293986bafdfb4f340ab79184a7bb3b477dfa63`. The whole tracked diff excluding `Testing-furrow.RPP` hashes to `e7e16dddc4f379d6753f0af816f9d1cc92cb68b88b262e239a2b1252f892894e`; this scope differs from the historical scoped-diff hash `685806fd...`, so those values are not compared as equivalent identities.
- No fresh timing capture or timing test was run. Before host testing, the user must establish a disposable blank project while leaving the dirty project open and untouched. Minimum safe setup: in REAPER choose **File → New project tab**, create an empty project, save that new project as `/tmp/furrow-v1-timing/constant.RPP`; do not save, discard, or close the existing project. If REAPER prompts to save the existing project, cancel and stop. Confirm the blank project is active, the original dirty tab remains open, and no save/discard prompt was accepted. After confirmation, obtain explicit authorization before running scripts that modify the scratch project and before changing sample-rate/block-size settings; record originals and restore them at the end.
- V1 remains not ready. Fresh timing evidence is blocked on the user-established scratch project and explicit scope for host/project/audio mutations.


## User-established REAPER scratch project — 18 October 2026, 18:23 UTC

- Read-only bridge reports REAPER 7.82/Linux and an active clean project with zero tracks. `EnumProjects` shows `/home/nochaserz/Documents/REAPER Media/Testing-furrow.RPP` remains open in another tab; the active scratch project has no saved path yet. Other existing tabs are present. Audio settings remain 44,100 Hz/1,024 samples.
- No project state, transport, save, or audio configuration was changed. The prior denied New Project Tab action was not retried.
- The user confirmed they started a new project and the bridge is running. Before creating the probe layout, saving the scratch project under `/tmp/furrow-v1-timing/`, or changing sample rate/block size for the timing matrix, obtain explicit authorization for those operations.

## Fresh timing captures — 18 October 2026, 19:06 UTC

- The user authorized saving and modifying the active blank scratch project, capturing REAPER output and temporarily changing the sample rate and block size for the documented matrix. The test project is `/tmp/furrow-v1-timing/cases/stop-restart-44100-1024.RPP`. No other project was saved, closed or switched. `Testing-furrow.RPP` has no repository worktree change and remains open in another tab.
- REAPER 7.82/Linux was checked at 44.1 kHz and 1,024 samples before and after the matrix. The 27-case grid used 44.1, 48 and 96 kHz, block sizes 64, 256 and 1,024, and 60, 120 and 200 BPM. The original 44.1 kHz and 1,024 sample settings were restored. The active scratch transport is stopped and the project is clean.
- A fresh comparison through `probes.diff_log.compare` rechecked all 27 grid captures. All 27 passed with 38 events each and at most one sample error. Full per-case captures and expected schedules are in `.audit/results/2026-10-18/fresh-timing/constant-*`; the rerun summary is `verifier-rerun-2026-10-18.txt`.
- Fresh ramp, on-beat tempo step, signature change, nonzero start, loop wrap, seek, stop/restart and offline-render cases passed. The old two-sample ramp discrepancy did not recur. The off-grid step remains a strict comparator failure for two events, each 689 samples early. The documented exception allows up to one 1,024-sample block after an off-grid marker in the same block. See `off-grid-exception-analysis.txt`. The one-sample verifier tolerance is unchanged.
- Pause-and-resume was investigated with project-scoped transport APIs after the original helper showed that global `GetPlayPosition2()` and project-specific `GetPlayPositionEx(project)` differed by more than 54 seconds with multiple tabs open. The original two captures that used the global API are excluded. Three corrected captures show stable receiver counts during a 0.4-second pause, then duplicate lane 1 and lane 2 events after resume. Raw counts, transport traces and comparator output are in the three `pause-resume-project-position-*` directories. The source resets its scheduling cursor whenever transport is not playing. This is a repeatable pause-resume anomaly, but the spec does not state whether resume must suppress events that the host may have scheduled before pause. The documented stop/restart scenario passes. No runtime change was made.
- No production code changed during this continuation. Tested source identity remains `HEAD=e4293986bafdfb4f340ab79184a7bb3b477dfa63`; the tracked diff hash excluding `Testing-furrow.RPP` is `e7e16dddc4f379d6753f0af816f9d1cc92cb68b88b262e239a2b1252f892894e`. These are uncommitted-source results, not release-snapshot evidence.
- These short captures do not satisfy the ten-minute timing success criterion. All-four internal MIDI-bus verification, the ten-minute 32-lane CPU workload, coordinator recovery, Windows, macOS Apple silicon, packaging and clean-install checks remain open. Peak block duration remains a reported measurement because the spec gives no acceptance threshold.
