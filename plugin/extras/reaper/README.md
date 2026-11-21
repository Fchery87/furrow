# REAPER track templates for Furrow

Copy these `.RTrackTemplate` files into REAPER's `TrackTemplates` folder (`Options -> Show REAPER resource path in explorer/finder -> TrackTemplates`), then insert them via `Track -> Insert track from template`.

- `Furrow-16Out-MultiBus.RTrackTemplate`: Creates a 32-channel Furrow parent track wired to 16 individual stereo mixer channels (`Main` and `Out 2` through `Out 16`). Assign each lane's **Output** selector in Furrow's Inspector to route drums onto separate REAPER mixer tracks.
- `Furrow-MIDI-Out-Routing.RTrackTemplate`: Creates a Furrow parent track sending MIDI Channels 1 through 4 (audio muted) to four child instrument tracks. Load your target synths or samplers onto the child tracks and set Furrow lanes to **MIDI out** on matching channels.
