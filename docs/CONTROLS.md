# Scratchivision controls

Parameter indices and ids are permanent. New parameters are only ever appended.

| # | Name | Values | Default | Page | Q-Link (screen) | Automatable |
|---|---|---|---|---|---|---|
| 0 | Gain | -24 .. +12 dB | 0 dB | (knob only) | 13 | yes |
| 1 | Sample | 1–64 (shows the file name) | 1 | DECK | 9 | yes |
| 2 | Speed | 33 / 45 | 33 | DECK | 5 | yes |
| 3 | Mode | Rel / Abs | Rel | DECK | 1 | yes |
| 4 | Loop | Off / On | Off | DECK | 14 | yes |
| 5 | Thru | Off / On | Off | DECK | 10 | yes |
| 6 | Cue | trigger | — | DECK | — | yes |
| 7 | Swap | Off / On | Off | SCOPE | 6 | yes |
| 8 | Test | Off / On | Off | SCOPE | — | yes |
| 9 | Pitch | -3.00x .. +3.00x | — | SCOPE | — | no (display) |
| 10 | Lock | No Signal / Relative / Locked | — | DECK, SCOPE | — | no |
| 11 | Level | 0 .. 1 (shown in dB) | — | SCOPE plot | — | no |
| 12 | Position | record time, m:ss.s | — | SCOPE | — | no |
| 13 | Direction | Stop / Fwd / Rev | — | SCOPE | — | no |
| 14 | Wave X | unused since 0.2 | — | — | — | no |
| 15 | Speed Plot | 0 .. 1 (0.5 = stopped) | — | SCOPE plot | — | no |
| 16–79 | Dot 1 X, Dot 1 Y, … Dot 32 Y | 0 .. 1 | — | SCOPE ring | — | no |
| 80 | Prev | trigger | — | DECK ◀ | — | yes |
| 81 | Next | trigger | — | DECK ▶ | — | yes |
| 82 | Rescan | trigger | — | DECK | — | yes |
| 83 | Zoom | 0.25 / 0.5 / 1 / 2 / 4 / 8 s / All | 1 s | DECK (− value +) | 2 | yes |
| 84 | Sample Info | e.g. `3/12  2.4 s`, `Loading...`, `cut to 20 s`, `Can't read`, `No WAVs` | — | DECK | — | no |
| 85–212 | Bar 1 … Bar 128 | 0 .. 1 | — | DECK waveform | — | no |
| 213–217 | Cue 1 … Cue 5 | trigger | — | DECK cue row | — | yes |
| 218 | Clear | Off / On (turns itself off after one cue) | Off | DECK cue row | — | yes |
| 219–223 | Cue 1 State … Cue 5 State | Unset / Set | — | DECK (pad image) | — | no |
| 224–228 | Cue 1 X … Cue 5 X | 0 .. 1 (0.5 = playhead) | — | DECK markers | — | no |
| 229–233 | Cue 1 Shown … Cue 5 Shown | Off / On | — | DECK markers | — | no |
| 234 | Zoom Out | trigger (−) | — | DECK | — | yes |
| 235 | Zoom In | trigger (+) | — | DECK | — | yes |
| 236 | Play X | 0 .. 1 | — | DECK moving playhead (Zoom All) | — | no |

Program-mode Q-Links 1–8: Gain, Sample, Speed, Mode, Loop, Thru, Swap, Zoom.

Sample changed in 0.2: it was a 2-way choice and is now 1–64. A project saved with 0.1.x may open on a different sample. From 0.2 the file name is saved with the project.

## What the controls do

- **Sample, ◀ ▶**: pick the sample and cue it.
  - Bundled samples come first, then the WAVs in `Scratchivision/` at the drive root.
- **Cue 1–5**:
  - An empty cue stores the play position.
  - A set cue jumps there, and the record keeps steering from it. In Abs mode it re-anchors.
  - Saved per audio file in `<file>.cues`.
- **CLEAR**: the next cue tapped is deleted.
- **RESCAN**: lists the folders again. New WAVs appear and the current sample stays selected.
- **Zoom − / +**:
  - Seconds across the waveform window: 0.25 to 8 s, centred on the playhead.
  - **All** shows the whole sample, standing still, with the playhead moving across it.
  - Bars behind the playhead are pink and bars ahead are chrome.
  - Sample changed in 0.4 from 4 steps to 7, so a project saved earlier may open at another zoom.
- **CUE**: back to the start of the sample. In Abs mode, the needle's current position on the
  record becomes the start.
- **Speed 33 / 45**: the record's nominal speed, so +1.00x means "playing normally".
- **Mode**:
  - **Rel** (relative): the sample follows the platter's motion. Needle drops are ignored.
  - **Abs** (absolute): the sample follows the needle position on the record, counted from the last CUE.
- **Loop**:
  - **Off**: the sample stops at its ends and is silent there. The first pull back after running
    off the end plays it backwards straight away.
  - **On**: the sample wraps around at its ends.
- **Thru**: the timecode goes to the output instead of the sample, for checking the input.
- **Swap**: exchanges L and R before decoding. Use it when forward play reads `Rev`.
- **Test**: an internal timecode signal replaces the input (a 2 s back-and-forth sweep).
