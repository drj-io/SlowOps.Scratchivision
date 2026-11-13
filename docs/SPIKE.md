# Device checks for the display tricks (MPC XL)

The MPC lets a plugin draw only through JSON skins. Scratchivision's waveform and scope rely on
three widgets the SDK marks as "less tested": XYPad, Plotter, and parameter-driven redraws at
about 30 Hz. None of this can be checked off the device, so this is the first thing to try on
the XL.

## Setup
1. Run `./scripts/build.sh`, then copy `plugin/build/Slow Ops - VST - Scratchivision/` into
   `VST2 Plugins/` on an MPC drive (or use `python3 third_party/mpc-plugin-builder-sdk/tools/mpc_remote.py <ip> update plugin --drive N --yes`). Restart the MPC.
2. Load Scratchivision as an insert on any audio track. Open its screen and tap **TEST** on the
   SCOPE page. An internal timecode signal sweeps back and forth every 2 s, so there is sound and
   everything moves without a turntable.

## Checks

| # | Look at | Pass | If it fails |
|---|---|---|---|
| 1 | DECK: the waveform | It slides left and right; the play position stays under the pink playhead | — |
| 2 | DECK: waveform edges | The waveform is cut off at the edges of the bordered window | No clipping: the waveform spills over the page. Next try: a `Slider` (`TrackAndThumb`) with the waveform as the thumb image |
| 3 | DECK: waveform alignment | At CUE, the start marker (thin pink line) sits under the playhead | Off: the puck mapping is wrong. It assumes the XYPad keeps the puck inside the pad (pads are made wider than the image in `scripts/gen_skins.py`; `waveX()` = 1 - position) |
| 4 | DECK: sample buttons | Switching samples swaps the waveform | Both waveforms show: `IndexedEnabling` is not hiding the pad |
| 5 | DECK: smoothness | Movement looks continuous, not steps of 1 s or more | Steps: the MPC redraws parameters slowly. Fall back to a position bar |
| 6 | SCOPE: dots | 32 dots trace a ring that wobbles in size | Dots stuck at the centre, or the UI stutters: too many parameter updates. Reduce `kNumDots` |
| 7 | SCOPE: plotters | SPEED draws a scrolling sine-like trace; LEVEL a flat line | Empty: Plotter does not take history from parameters. Drop to the text readouts |
| 8 | Readouts | Pitch, Lock, Direction and Position change while TEST runs | — |
| 9 | CPU | The MPC's CPU meter stays low; no audio dropouts while the SCOPE page is open | High: reduce the display rate (`displayEvery_`) |
| 10 | Look | Logo, gradient borders and buttons are sharp, not blurred or squashed | Scaled: an image size does not match its bounds |

Then with a turntable and Serato control vinyl (side A, 33):

| # | Check | Pass |
|---|---|---|
| 11 | Wiring | Turntable → phono preamp (or a phono input) → the track's audio input; the track monitors its input |
| 12 | Direction | Playing forward reads `Fwd` and a positive pitch. If it reads `Rev`, tap SWAP |
| 13 | Lock | `Locked` within a fraction of a second at normal speed. If it only reads `Relative`, the LFSR constants in `core/include/slowops/scratch/Lfsr.hpp` do not match the vinyl. Pitch and scratching still work |
| 14 | Feel | Baby scratches, forward/back and stops follow the hand with no audible lag |
| 15 | Recording | Record 10 s of the raw timecode (Thru on) to a WAV and keep it: it becomes a regression test |

## 0.2 checks (user WAVs, live waveform)

| # | Look at | Pass | If it fails |
|---|---|---|---|
| 16 | DECK bars in TEST | 128 bars scroll smoothly; chrome ahead of the playhead, pink behind | Jerky or the UI stutters: use 64 bars (`kNumBars`, `NUM_BARS`), or update less often |
| 17 | CPU with bars + scope | No dropouts; the MPC stays responsive | As 16 |
| 18 | Sample name | ◀ ▶ change the name under the waveform | Name stuck: the MPC caches value text. Call `requestDisplayUpdate()` when the sample changes |
| 19 | User folder | `Scratchivision/` appears at the root of the internal drive after the plugin loads | Not created: the drive is read-only to plugins. Document making it by hand |
| 20 | RESCAN | A WAV saved into that folder from the MPC browser appears after RESCAN and plays | — |

## 0.3 checks (hot cues)

| # | Look at | Pass | If it fails |
|---|---|---|---|
| 21 | Cue pads | Tapping an empty pad fills it; tapping again jumps; CLEAR + pad empties it | Pad image stuck: `IndexedEnabling` on `Cue n State` not redrawing |
| 22 | Markers | Numbered markers sit on the waveform, move with it, and vanish outside the window | Off by a fixed amount: the marker pad maths (`gen_skins.py`) |
| 23 | Cue file | `<file>.cues` appears next to the audio file in `Scratchivision/`; cues come back after reloading | Not written: the drive is read-only to plugins |

## 0.4 checks (zoom)

| # | Look at | Pass | If it fails |
|---|---|---|---|
| 24 | Zoom − / + | The value steps 0.25 s … 8 s, All; the bars rescale | — |
| 25 | Zoom All | The whole sample shows, standing still; the fixed playhead hides and a moving one sweeps across; bars turn pink behind it | Two playheads, or none: `RangedEnabling` / `IndexedEnabling` on Zoom |

## Results

- 2026-11-13, 0.2.0: no user samples loaded. The files were MP3/AIFF on the USB drive, while the
  plugin was on the internal drive and read only WAV. 0.2.1 lists every drive and reads AIFF and MP3.
- 2026-11-13, MPC XL, OS 3.10.1.12: scratching works with Phase (MWM). The waveform did not move
  when the pad was the window's width (the puck is kept inside the pad), so the pads were widened (0.1.1).

Record the date, firmware version, and which checks passed or failed here.
