#!/usr/bin/env python3
"""Copy ./wav/*.wav into plugin/Content/Samples/: the samples bundled with the plugin.

Standard library only; run by scripts/build.sh. The plugin lists Content/Samples at run time
(then the user's Scratchivision/ folder at the drive root), so nothing is compiled in.
"""
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import skin_layout as L  # noqa: E402

samples = L.list_samples()
content = os.path.join(L.ROOT, 'plugin', 'Content', 'Samples')
if os.path.isdir(content):
    shutil.rmtree(content)
os.makedirs(content)
for _, f, _ in samples:
    shutil.copy(os.path.join(L.WAV_DIR, f), os.path.join(content, f))
print('bundled samples: %s' % ', '.join('%s (%.2f s)' % (n, d) for n, _, d in samples))
