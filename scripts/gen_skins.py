#!/usr/bin/env python3
"""Write plugin/skins/TUI.json and Q-Links.json for Scratchivision.

Standard library only. Hand-built skin (the SDK's gen_ui.py cannot do scrolling waveforms or
scopes); build.py copies plugin/skins/ verbatim. Parameter indices come from
plugin/build/params.json (written by `build.py --target native`), looked up by name, so
the skin always matches the binary. Layout comes from scripts/skin_layout.py; the PNGs from
scripts/make_skin_art.py.

How the moving parts work (see docs/SPIKE.md for what to check on the device):
  * Waveform: 128 film-strip images across the waveform window, each bound to "Bar n". The strip
    has 2 x 24 frames: heights ahead of the playhead (chrome), then already played (pink), so the
    plugin colours each bar by choosing the frame. Zoom 0.25..8 s centres the window on the play
    position under the fixed playhead image; Zoom All shows the whole sample still and hides the
    fixed playhead in favour of a moving one (an XYPad bound to "Play X").
  * Sample: < and > buttons (Prev / Next triggers) around a label showing "Sample"'s value (the
    plugin reports the file name as the value) and one showing "Sample Info"; RESCAN.
  * Hot cues: per cue two Button layers ("empty" / "set") bound to the "Cue n" trigger, the
    visible one picked by IndexedEnabling on "Cue n State"; CLEAR is a toggle. Markers are
    XYPads (the puck stays inside its pad, so each pad is the window width plus one marker
    width, offset by half a marker: x = 0.5 is the playhead), hidden by "Cue n Shown".
  * Playhead: a static image over the centre of the window, drawn after the bars and markers.
  * Scope: 32 XYPads, puck = a glowing dot, x/y bound to "Dot n X" / "Dot n Y".
  * Plotters: "Speed Plot" and "Level" with history.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import skin_layout as L  # noqa: E402

PARAMS = os.path.join(L.ROOT, 'plugin', 'build', 'params.json')
OUT = os.path.join(L.ROOT, 'plugin', 'skins')

params = json.load(open(PARAMS))['params']
IDX = {p['name']: p['index'] for p in params}


def P(name):
    return 'Parameter %d' % IDX[name]


FIXED_HALF = ("Fixed/displayRange 0 1 valueRange 0 1 fineIncrement 0.01 coarseIncrement 0.1 "
              "defaultValue 0.5 displayString 'Mid' name 'Mid' value 0.5")
NO_BG = {'version': 1, 'focussed': {'version': 1, 'colour': '0', 'image': ''}, 'unfocussed': {'version': 1, 'colour': '0', 'image': ''}}


def rect(r):
    return ' '.join('%g' % round(v, 2) for v in r)


def bounds(r, show='Show', only=None, focus='No'):
    b = {'version': 2 if only else 1, 'acceptsHWFocus': focus, 'showWhenDataModelInvalid': show,
         'whenVisible': 'Always', 'boundsType': 'Absolute', 'bounds': rect(r)}
    if only:
        b['additionalInvalidatingHandles'] = [only]
    return b


def entry(name, typ, data, r, show='Show', only=None, focus='No'):
    return {'version': 2, 'componentData': {'version': 1, 'name': name, 'type': typ, 'data': data},
            'handle remapping': {'version': 1, 'map': []}, 'bounds': bounds(r, show, only, focus)}


def image(name, img, r, only=None):
    return entry(name, 'Image', {'version': 2, 'imageType': 'Regular', 'colour': '0', 'image': img}, r, 'Show', only)


def button(name, img, handle, r, button_id=1, n=1, only=None):
    return entry(name, 'Button', {'version': 2, 'onImage': img + '_on.png', 'offImage': img + '_off.png',
                                  'buttonId': button_id, 'numButtonsInGroup': n, 'handleName': handle,
                                  'gestureBehaviour': 'Instant'}, r, 'Hide', only)


def filmstrip(name, img, frames, handle, r):
    return entry(name, 'Image', {'version': 2, 'imageType': 'FilmStrip', 'colour': '0', 'image': img,
                                 'numFrames': frames, 'handleName': handle}, r)


def label(name, handle, r, size, colour, kind='Value', text=None, style='Bold', just='left verticallyCentred'):
    data = {'version': 1,
            'textStyle': {'version': 1, 'font': {'version': 1, 'name': 'Nexa', 'style': style, 'height': float(size)},
                          'colour': L.argb(colour), 'justification': just, 'case': 'Original'},
            'type': kind, 'handleName': handle}
    if text is not None:
        data['text'] = text
    return entry(name, 'Label', data, r, 'Hide' if kind != 'Text' else 'Show')


def xypad(name, x_handle, y_handle, img, r, only=None):
    return entry(name, 'XYPad', {'version': 1, 'xPositionHandle': x_handle, 'yPositionHandle': y_handle, 'image': img},
                 r, 'Show', only)


def plotter(name, handle, r, c0, c1, fill0, fill1, zero):
    return entry(name, 'Plotter', {
        'version': 2, 'backgroundColour': '0', 'drawLine': True,
        'lineColourStart': L.argb(c0), 'lineColourEnd': L.argb(c1), 'horizLineGradient': True, 'lineWidth': 3.0,
        'fillColourStart': L.argb(fill0, 0x50), 'fillColourEnd': L.argb(fill1, 0x00), 'horizFillGradient': False,
        'fillZero': zero, 'dataHandle': handle, 'dataMin': 0.0, 'dataMax': 1.0,
        'drawHistory': True, 'historyDirection': 'Left',
        'historyColour1': L.argb(c0), 'historyColour2': L.argb(c1, 0x60),
        'historyTime': 4.0, 'historySkipFrames': 1}, r, 'Show')


def container(children):
    return {'version': 2, 'actions': [], 'backgroundData': NO_BG, 'ignoreMousePresses': False,
            'disableCoarseDataWheel': False, 'componentsData': children}


def tab(name, key, bg, children):
    return {'version': 1, 'tabName': name, 'fnKeyIndex': key, 'fnKeySubIndex': 0,
            'componentDefinition': {
                'version': 2, 'actions': [],
                'backgroundData': {'version': 1,
                                   'focussed': {'version': 1, 'colour': L.argb(L.BG), 'image': bg},
                                   'unfocussed': {'version': 1, 'colour': L.argb(L.BG), 'image': bg}},
                'ignoreMousePresses': False, 'disableCoarseDataWheel': False,
                'componentsData': children},
            'qlinkBoundsData': []}


def main():
    vx, vy, vw, vh = L.WAVE_VIEW

    # ---- DECK ----
    bw = vw / float(L.NUM_BARS)
    bars = [filmstrip('bar %d' % (k + 1), 'bar.png', 2 * L.BAR_STEPS, P('Bar %d' % (k + 1)), (vx + k * bw, vy, bw, vh))
            for k in range(L.NUM_BARS)]
    nz = len(L.ZOOM_CHOICES)
    zoom_all = 'IndexedEnabling/%d/%d/%s' % (nz - 1, nz, P('Zoom'))
    zoom_centred = 'RangedEnabling/0/%d/%d/%s' % (nz - 2, nz, P('Zoom'))
    B = L.DECK_BUTTONS
    mw = L.CUE_MARKER_W
    markers = [xypad('cue marker %d' % (c + 1), P('Cue %d X' % (c + 1)), FIXED_HALF, 'cue_marker_%d.png' % (c + 1),
                     (-mw / 2.0, 0, vw + mw, vh), 'IndexedEnabling/1/2/%s' % P('Cue %d Shown' % (c + 1)))
               for c in range(L.NUM_CUES)]
    pw = L.PLAYHEAD_W
    markers.append(xypad('moving playhead', P('Play X'), FIXED_HALF, 'playhead.png', (-pw / 2.0, 0, vw + pw, vh), zoom_all))
    cue_pads = []
    for c, r in enumerate(L.CUE_PADS):
        n = c + 1
        for k, look in enumerate(('empty', 'set')):
            cue_pads.append(button('cue %d %s' % (n, look), 'cue%d_%s' % (n, look), P('Cue %d' % n), r,
                                   only='IndexedEnabling/%d/2/%s' % (k, P('Cue %d State' % n))))
    cue_pads.append(button('clear', 'clear', P('Clear'), L.CUE_CLEAR))
    deck = [image('logo', 'logo.png', L.LOGO)] + bars + [
        entry('cue markers', 'cueClip', {'version': 1, 'handleName': 'Data'}, L.WAVE_VIEW),
        image('playhead', 'playhead.png', (vx + vw // 2 - L.PLAYHEAD_W // 2, vy, L.PLAYHEAD_W, vh), zoom_centred),
        button('prev', 'prev', P('Prev'), L.SAMPLE_PREV),
        button('next', 'next', P('Next'), L.SAMPLE_NEXT),
        button('rescan', 'rescan', P('Rescan'), L.SAMPLE_RESCAN),
        label('sample name', P('Sample'), L.SAMPLE_NAME, 30, L.CYAN, just='horizontallyCentred verticallyCentred'),
        label('sample info', P('Sample Info'), L.SAMPLE_INFO, 18, L.PINK, just='horizontallyCentred verticallyCentred'),
        button('cue', 'cue', P('Cue'), B['cue']),
        button('33', 'rpm33', P('Speed'), B['rpm33'], 0, 2),
        button('45', 'rpm45', P('Speed'), B['rpm45'], 1, 2),
        button('rel', 'rel', P('Mode'), B['rel'], 0, 2),
        button('abs', 'abs', P('Mode'), B['abs'], 1, 2),
        button('loop', 'loop', P('Loop'), B['loop']),
        button('thru', 'thru', P('Thru'), B['thru']),
        button('zoom out', 'zoomout', P('Zoom Out'), L.ZOOM_OUT),
        button('zoom in', 'zoomin', P('Zoom In'), L.ZOOM_IN),
        label('zoom', P('Zoom'), L.ZOOM_LABEL, 26, L.CYAN, just='horizontallyCentred verticallyCentred'),
        label('lock', P('Lock'), L.DECK_READOUTS['lock'], 26, L.PINK)] + cue_pads

    # ---- SCOPE ----
    rx, ry, rw, rh = L.RING_VIEW
    dots = []
    for d in range(L.NUM_DOTS):   # oldest first, so the newest are drawn on top and brightest
        age = min(3, (L.NUM_DOTS - 1 - d) // 8)
        dots.append(xypad('dot %d' % (d + 1), P('Dot %d X' % (d + 1)), P('Dot %d Y' % (d + 1)), 'dot_%d.png' % age, (0, 0, rw, rh)))
    scope = [entry('scope window', 'scopeClip', {'version': 1, 'handleName': 'Data'}, L.RING_VIEW),
             plotter('speed plot', P('Speed Plot'), L.PLOT_SPEED_VIEW, L.CYAN, L.BLUE, L.CYAN, L.BLUE, 0.5),
             plotter('level plot', P('Level'), L.PLOT_LEVEL_VIEW, L.PINK, L.MAGENTA, L.PINK, L.MAGENTA, 0.0)]
    names = {'speed': 'Pitch', 'dir': 'Direction', 'lock': 'Lock', 'pos': 'Position'}
    for key, (_, val) in L.SCOPE_READOUTS.items():
        scope.append(label(key, P(names[key]), val, 32, L.CYAN))
    scope += [button('swap', 'swap', P('Swap'), L.SCOPE_BUTTONS['swap']),
              button('test', 'test', P('Test'), L.SCOPE_BUTTONS['test'])]

    # No replacement strings for Sample: its value text is the file name, which the plugin reports.
    reps = [{'key': P('Speed'), 'value': ['33', '45']},
            {'key': P('Zoom'), 'value': L.ZOOM_CHOICES},
            {'key': P('Mode'), 'value': ['Rel', 'Abs']}]
    for b in ('Loop', 'Thru', 'Cue', 'Swap', 'Test', 'Prev', 'Next', 'Rescan', 'Clear', 'Zoom Out', 'Zoom In'):
        reps.append({'key': P(b), 'value': ['Off', 'On']})
    for c in range(1, L.NUM_CUES + 1):
        reps.append({'key': P('Cue %d' % c), 'value': ['Off', 'On']})
        reps.append({'key': P('Cue %d State' % c), 'value': ['Unset', 'Set']})
        reps.append({'key': P('Cue %d Shown' % c), 'value': ['Off', 'On']})

    tui = {'pageData': {
        'version': 1,
        'componentDefinitions': {'version': 2, 'importFiles': [], 'localComponentDefinitions': [
            {'key': 'scopeClip', 'value': container(dots)},
            {'key': 'cueClip', 'value': container(markers)}]},
        'replacementStrings': reps,
        'info': {'version': 1, 'type': 'CompleteDescription'},
        'tabs': [tab('DECK', 0, 'bg_deck.png', deck), tab('SCOPE', 1, 'bg_scope.png', scope)]}}

    # Q-Links: left column (13, 9, 5, 1) = Gain, Sample, Speed, Mode; next column = Loop, Thru, Swap, Zoom.
    # Cue (a trigger) and the read-only displays never go on a knob.
    q = {'Q-Link %d' % k: -1 for k in range(1, 17)}
    for k, name in zip((13, 9, 5, 1, 14, 10, 6, 2), ('Gain', 'Sample', 'Speed', 'Mode', 'Loop', 'Thru', 'Swap', 'Zoom')):
        q['Q-Link %d' % k] = IDX[name]
    prog = {'Q-Link %d' % k: -1 for k in range(1, 17)}
    for k, name in enumerate(('Gain', 'Sample', 'Speed', 'Mode', 'Loop', 'Thru', 'Swap', 'Zoom')):
        prog['Q-Link %d' % (k + 1)] = IDX[name]
    qlinks = {'version': 4, 'info': {'version': 1, 'type': 'CompleteDescription'},
              'Screen Mode Q-Links': {'version': 4, 'map': [
                  {'Tab': t, 'SubTab': 1, 'Bank Direction': 'Column', 'Q-Links': dict(q)} for t in (1, 2)]},
              'Program Mode Q-Links': prog}

    os.makedirs(OUT, exist_ok=True)
    json.dump(tui, open(os.path.join(OUT, 'TUI.json'), 'w'), indent=2)
    json.dump(qlinks, open(os.path.join(OUT, 'Q-Links.json'), 'w'), indent=2)
    print('skins: TUI.json (%d DECK, %d SCOPE components), Q-Links.json' % (len(deck), len(scope)))


if __name__ == '__main__':
    main()
