# Scratchivision

Scratch a sample on your MPC with a turntable. Scratchivision decodes Serato-style control
vinyl from a track's audio input and plays the chosen sample at the record's speed and
direction. Hold the platter and the sample stops. Pull it back and the sample plays backwards.

## Install

1. You need **MPC OS 3.10.1 or later**. On the MPC, turn on **Preferences → Enhancements (Beta)
   → Enable Enhancements** and **External Plugin Support**.
2. Copy the folder `Slow Ops - VST - Scratchivision` into a folder named `VST2 Plugins` at the
   root of a drive (USB, SD card or the internal drive).
3. Restart the MPC.

## Quick start

1. Connect the turntable, through a phono preamp, to the MPC's audio input.
2. Make an audio track that monitors that input, and add **Scratchivision** as an insert on it
   (External Plugins).
3. Put on Serato control vinyl, side A, at 33. The DECK page shows `Locked` and `+1.00x`, and
   the sample plays.
4. Scratch. Tap **CUE** to go back to the start of the sample.

No turntable? Tap **TEST** on the SCOPE page. An internal timecode signal moves the sample back
and forth.

## Add your own samples

1. Put **WAV, AIFF or MP3** files in a folder named **`Scratchivision`** at the root of any
   drive: internal, SD card or USB.
   - The plugin creates this folder on its own drive the first time it loads.
   - Samples on the internal drive are listed first, then other drives.
   - You can copy files there from a computer, or save or export a sample into it from the
     MPC's browser.
2. On the DECK page, tap **RESCAN**, or reload the plugin. The new files appear after the
   bundled samples, sorted by name.
3. Step through the samples with **◀ ▶**, or turn the Sample Q-Link.

**Limits:**
- Up to 64 samples in total.
- Files longer than 20 s are cut to 20 s, and the info line shows "cut to 20 s".
- Any sample rate. Mono or stereo (multichannel files use the first two channels). WAV and
  AIFF in 8 to 32-bit or float, and MP3.
- A file that can't be read shows "Can't read".

The project remembers the sample by its file name, so adding files doesn't change which sample
a saved project opens with.

## Cue points

Five hot cues per sample, on the row under the waveform:
- **Tap an empty cue** (outlined) to store the current position. It fills with its colour, and a
  numbered marker appears on the waveform.
- **Tap a set cue** to jump there. The record keeps controlling the sample from the cue. In Abs
  mode the cue becomes the new starting point, as CUE does.
- **Delete a cue:** tap **CLEAR**, then the cue. CLEAR turns itself off after one cue.

Cues belong to the audio file. They are saved in a small `<file>.cues` file next to it in the
`Scratchivision` folder, so they come back in every project that uses the file. Cues for the
bundled samples go in `Scratchivision` on the plugin's drive.

## Pages

- **DECK**: the sample's waveform scrolls past the centre playhead. It is drawn as bars: chrome
  ahead of the playhead, pink behind. Below it:
  - five cue pads and CLEAR, with numbered markers on the waveform,
  - the sample name with ◀ ▶ and RESCAN,
  - CUE, 33/45, Rel/Abs, Loop and Thru,
  - the lock state,
  - **Zoom − / +**: 0.25 to 8 s across the window, or **All** for the whole sample, standing
    still, with the playhead moving across it. The Zoom Q-Link does the same.
- **SCOPE**: a dot trace of the input (a clean signal makes a round ring), a scrolling speed
  plot, the input level, and the pitch, direction, lock and record position. Also SWAP and TEST.

## Good to know

- Forward play reads `Rev`? Your L and R are swapped: tap **SWAP**.
- `Relative` instead of `Locked`: scratching works, but the needle position isn't decoded. Abs
  mode then behaves like Rel.
- Only the selected sample is kept in memory (up to 20 s, about 7 MB in stereo). Each instance
  also holds a 4 MB position table.

Serato is a trademark of Serato Audio Research. Scratchivision is not affiliated with Serato or
inMusic. MPC is a trademark of inMusic Brands.
