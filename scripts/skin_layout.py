"""Shared layout and palette for the Scratchivision skin.

Imported by make_skin_art.py (draws the PNGs) and gen_skins.py (writes TUI.json), so every
rectangle is defined once. Rectangles are (x, y, w, h) in tab coordinates: a tab's content
area is 1280 x 628 and starts 110 px down the MPC's 1280 x 800 screen.
"""
import os
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WAV_DIR = os.path.join(ROOT, 'wav')
TAB_W, TAB_H = 1280, 628

# ---- palette (from images/logo.png: chrome letters, black outline, sunset border) ----------
BG = (11, 10, 22)               # near-black page
PANEL = (20, 16, 34)            # panel fill
PANEL_HI = (30, 24, 50)
CYAN = (236, 242, 252)          # "lit" fill / main text: the letters' chrome, top (name kept from 0.1)
BLUE = (122, 142, 188)          # chrome, bottom (steel blue)
ORANGE = (255, 140, 70)         # border gradient: top-left
CORAL = (255, 90, 90)
PINK = (255, 60, 120)           # hot pink (secondary text, played bars)
MAGENTA = (205, 30, 125)
PURPLE = (92, 28, 132)          # border gradient: bottom-right
BORDER_STOPS = [ORANGE, CORAL, PINK, MAGENTA, PURPLE]
CUE_COLOURS = [ORANGE, CORAL, PINK, MAGENTA, (150, 70, 220)]   # cue 1..5, along the logo's border
BLACK = (0, 0, 0)
TEXT_DIM = (150, 150, 185)
BORDER = 2.5                    # panel gradient ring, px (halved in 0.2)
BORDER_BIG = 3.0                # waveform and scope panels
INNER_STROKE = 1.5              # black line inside the ring
BUTTON_RING = 1.5


def argb(rgb, a=255):
    """Colour for TUI.json: AARRGGBB."""
    return '%02x%02x%02x%02x' % (a, rgb[0], rgb[1], rgb[2])


# ---- DECK tab -----------------------------------------------------------------------------
LOGO = (370, 4, 540, 130)
WAVE_PANEL = (20, 140, 1240, 248)
WAVE_VIEW = (34, 154, 1212, 220)            # area the waveform bars fill

# Hot cues: a row of five pads and CLEAR between the waveform and the bottom panels.
NUM_CUES = 5
CUE_ROW_Y, CUE_ROW_H = 398, 48
CLEAR_W = 150
CUE_GAP = 12
CUE_W = (1240 - CLEAR_W - NUM_CUES * CUE_GAP) // NUM_CUES
CUE_PADS = [(20 + i * (CUE_W + CUE_GAP), CUE_ROW_Y, CUE_W, CUE_ROW_H) for i in range(NUM_CUES)]
CUE_CLEAR = (20 + 1240 - CLEAR_W, CUE_ROW_Y, CLEAR_W, CUE_ROW_H)
CUE_MARKER_W = 30

PLAYHEAD_W = 64

P_SAMPLE = (20, 456, 400, 164)
P_DECK = (440, 456, 400, 164)
P_READ = (860, 456, 400, 164)

NUM_BARS = 128
BAR_STEPS = 24                              # bar heights; bar.png has 2 x 24 frames (ahead, then played)

# SAMPLE panel: < name > with the info line under the name, RESCAN in the corner.
SAMPLE_PREV = (P_SAMPLE[0] + 16, P_SAMPLE[1] + 50, 56, 98)
SAMPLE_NEXT = (P_SAMPLE[0] + P_SAMPLE[2] - 72, P_SAMPLE[1] + 50, 56, 98)
SAMPLE_NAME = (P_SAMPLE[0] + 84, P_SAMPLE[1] + 52, P_SAMPLE[2] - 168, 56)
SAMPLE_INFO = (P_SAMPLE[0] + 84, P_SAMPLE[1] + 110, P_SAMPLE[2] - 168, 34)
SAMPLE_RESCAN = (P_SAMPLE[0] + P_SAMPLE[2] - 136, P_SAMPLE[1] + 10, 120, 34)

DECK_BUTTONS = {
    'cue':  (P_DECK[0] + 16, P_DECK[1] + 50, 124, 98),
    'rpm33': (P_DECK[0] + 152, P_DECK[1] + 50, 110, 44),
    'rpm45': (P_DECK[0] + 274, P_DECK[1] + 50, 110, 44),
    'rel':  (P_DECK[0] + 152, P_DECK[1] + 104, 110, 44),
    'abs':  (P_DECK[0] + 274, P_DECK[1] + 104, 110, 44),
    'loop': (P_READ[0] + 254, P_READ[1] + 50, 130, 44),
    'thru': (P_READ[0] + 254, P_READ[1] + 104, 130, 44),
}
DECK_READOUTS = {
    'lock': (P_READ[0] + 16, P_READ[1] + 108, 226, 40),
}
# Zoom: - [value] + in the STATUS panel (pitch moved to the SCOPE page in 0.4).
ZOOM_OUT = (P_READ[0] + 16, P_READ[1] + 50, 56, 48)
ZOOM_LABEL = (P_READ[0] + 76, P_READ[1] + 50, 106, 48)
ZOOM_IN = (P_READ[0] + 186, P_READ[1] + 50, 56, 48)
ZOOM_CHOICES = ['0.25 s', '0.5 s', '1 s', '2 s', '4 s', '8 s', 'All']

# ---- SCOPE tab ----------------------------------------------------------------------------
RING_PANEL = (20, 20, 588, 588)
RING_VIEW = (44, 44, 540, 540)
DOT = 18
NUM_DOTS = 32
PLOT_SPEED_PANEL = (628, 20, 632, 214)
PLOT_SPEED_VIEW = (644, 62, 600, 156)
PLOT_LEVEL_PANEL = (628, 250, 632, 150)
PLOT_LEVEL_VIEW = (644, 292, 600, 92)
SCOPE_READ_PANEL = (628, 416, 632, 192)
SCOPE_READOUTS = {               # name -> (caption rect, value rect)
    'speed': ((644, 452, 140, 26), (644, 476, 190, 44)),
    'dir':   ((852, 452, 140, 26), (852, 476, 160, 44)),
    'lock':  ((644, 528, 140, 26), (644, 552, 190, 44)),
    'pos':   ((852, 528, 140, 26), (852, 552, 190, 44)),
}
SCOPE_BUTTONS = {
    'swap': (1080, 462, 162, 52),
    'test': (1080, 536, 162, 52),
}


# ---- bundled samples ----------------------------------------------------------------------
def list_samples():
    """[(display name, file name, duration seconds)] for every WAV in ./wav, sorted by file name."""
    out = []
    for f in sorted(os.listdir(WAV_DIR), key=str.lower):
        if not f.lower().endswith('.wav') or f.startswith('.'):
            continue
        with wave.open(os.path.join(WAV_DIR, f), 'rb') as w:
            dur = w.getnframes() / float(w.getframerate())
        out.append((os.path.splitext(f)[0], f, dur))
    return out
