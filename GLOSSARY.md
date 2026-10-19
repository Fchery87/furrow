# Furrow

Furrow is a step sequencer inside REAPER. This glossary defines the words the project uses for its own concepts, so that the spec, the code and the window mean the same thing.

## Structure

**Rack**:
A sequencer instance in a project. It owns lanes and patterns, and it can be live or committed.
_Avoid_: Sequencer, instance, project

**Lane**:
One sound in a rack, with its own steps. Each lane has a stable identity that outlives reordering and renaming.
_Avoid_: Channel, track, row

**Pattern**:
A complete set of lane steps that a rack can play or edit. A rack holds many patterns, and one is shown at a time.
_Avoid_: Sequence, scene, clip

**Step**:
One position in a lane. A step is on or off and carries its own values, such as velocity and gate.
_Avoid_: Cell, beat, note

## Playback

**Live**:
The rack plays its shown pattern in real time.
_Avoid_: Realtime, pattern mode

**Items**:
The rack plays its committed MIDI items instead of playing live. Live and Items never sound together.
_Avoid_: Arrange mode, bounce mode

**Commit**:
Render the shown pattern to MIDI items at the edit cursor and switch the rack to Items.
_Avoid_: Bounce, freeze, render

**Commit group**:
The set of items that one commit wrote. Each item in the group points back to the same source.
_Avoid_: Batch, set

**Source snapshot**:
The complete musical source of a commit group, stored once for the group. It is what restore rebuilds.
_Avoid_: Backup, copy, dump

## Capture

**Capture**:
Bring a MIDI item back into a pattern. Capture is the umbrella action. It either restores or converts, depending on the item.
_Avoid_: Import, reverse-bounce

**Unedited item**:
A committed item whose position, length, take offset and events are the same as when it was committed.
_Avoid_: Pristine, clean

**Edited item**:
A committed item whose position, length, take offset or events changed after it was committed.
_Avoid_: Modified, dirty

**Restore**:
Rebuild an unedited item's source snapshot as a new pattern. Restore changes only steps and lengths. It never changes lane settings.
_Avoid_: Revert, undo, reload

**Convert**:
Turn MIDI that Furrow did not write, or an edited item, into lanes and steps, after the user has seen a preview. Convert may refuse what it cannot represent.
_Avoid_: Quantize, import

## Probability

**Probability key**:
The stable number that seeds the probability rolls of a pattern. A restored pattern keeps the key of its source, so its rolls match the original.
_Avoid_: Seed, random id
