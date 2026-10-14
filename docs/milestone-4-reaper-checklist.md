# Milestone 4 REAPER checklist

Milestone 4 closes only when every case below passes in REAPER. ysfx and the stub REAPER do not replace these runs. Record each result as PASS or FAIL with what you saw. A FAIL stops the milestone, and the case is fixed before the next one starts.

## Setup

1. Open a new project. Save it as `m4-proof.rpp` in a folder you can find again.
2. Add one audio lane with a sample, and a second lane with another sample.
3. Open `lua/Furrow_Rack.lua`. Make the pattern audible in Live mode with the transport playing.
4. Start the bridge (`tools/reaper_bridge/bridge.lua`) if you want the state and render checks. The bridge reads state, but this checklist does not depend on it.

## A. Live and Items never sound together

For each case, play the transport for at least 4 bars after the change. Then listen for both Live hits and committed hits at once.

| # | Action | Expected |
|---|---|---|
| A1 | Commit at the cursor, then press Live | Only the live pattern sounds. The committed items are silent. |
| A2 | Press Items | Only the committed items sound. Live is silent. |
| A3 | With the rack window **closed**, unmute one committed item in REAPER's item properties | No live hits sound on that lane. The committed hit does, and only in Items mode. |
| A4 | With the rack window **closed**, mute one committed item, then unmute it | The muted item stays silent until unmuted. No live hits sound at any point. |
| A5 | With the window closed, switch the rack to Live through the Furrow action, then listen | The committed items stay silent while Live plays. |

## B. Move, delete, duplicate and detach

| # | Action | Expected |
|---|---|---|
| B1 | Move a committed item to another track (detach it) | The item keeps playing on its new track. Furrow stops owning it, and the rack does not change. |
| B2 | Duplicate a committed item on its own track | The copy plays with the original in Items mode. Its metadata still names the same group. |
| B3 | Delete one committed item | The rest of the group still plays. The rack does not complain. |
| B4 | Delete a whole items track | The rack recovers. Commit again and the items return. |

## C. Mode handoff and failed commit

| # | Action | Expected |
|---|---|---|
| C1 | Switch Live to Items to Live five times quickly | No stuck notes. Only one of Live or Items sounds at a time. |
| C2 | Start a commit, then close the window before it finishes | The commit finishes or rolls back. No half-written items remain. |
| C3 | Commit with a locked or missing lane track | The commit refuses, names the problem, and leaves no items. The rack returns to its earlier mode. |

## D. Undo and reload

| # | Action | Expected |
|---|---|---|
| D1 | Commit, then press Ctrl+Z | The items disappear, and the rack returns to Live. |
| D2 | Ctrl+Shift+Z | The items return, and the rack is in Items. |
| D3 | Save the project, close REAPER, reopen it, open the window | The rack, its mode, the items and their group all come back, and the sound matches before the save. |

## E. Restore (needs PR #43 merged)

| # | Action | Expected |
|---|---|---|
| E1 | Select an unedited committed item, then use the restore button | A new pattern appears with the same steps and length as the source. |
| E2 | Move a note in an item, then restore it | The restore refuses with a message, because the item is edited. |
| E3 | Delete a lane from the rack, then restore a commit that used it | The restore names the missing lane and changes nothing. |

## F. Tick order (open question from the commit PR)

| # | Action | Expected |
|---|---|---|
| F1 | Commit a pattern where two notes share a tick, then reopen the project | The items read back as unedited, so restore accepts them. |

## Reporting

For each case, write `A1 PASS` or `A1 FAIL` and one line about what you heard or saw. For a FAIL, say which step failed and what happened instead. Keep the project file and any render you made, so the result can be checked again.
