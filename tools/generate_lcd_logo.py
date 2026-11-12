#!/usr/bin/env python3
"""Render the project SVG to the display's packed 1-bit C bitmap (ImageMagick required)."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
svg = root / 'logo' / 'rfsentinel-oled-icon.svg'
raw = subprocess.check_output([
    'convert', '-background', 'black', str(svg), '-resize', '64x64!',
    '-alpha', 'remove', '-colorspace', 'gray', '-threshold', '50%',
    '-depth', '8', 'gray:-',
])
if len(raw) != 64 * 64:
    raise SystemExit('Unexpected raster size: %d' % len(raw))
packed = bytearray()
for row in range(64):
    for start in range(0, 64, 8):
        value = 0
        for col in range(8):
            value = (value << 1) | (raw[row * 64 + start + col] >= 128)
        packed.append(value)
lines = ['#pragma once', '#include <stdint.h>',
         '#define OLED_LOGO_W 64', '#define OLED_LOGO_H 64',
         '/* Generated from logo/rfsentinel-oled-icon.svg by tools/generate_lcd_logo.py. */',
         'static const uint8_t oled_logo_bits[] = {']
for offset in range(0, len(packed), 8):
    lines.append('    ' + ', '.join('0x%02x' % b for b in packed[offset:offset + 8]) + ',')
lines.append('};')
(root / 'main' / 'oled_logo.h').write_text('\n'.join(lines) + '\n')
