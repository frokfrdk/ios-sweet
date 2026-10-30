#!/usr/bin/env python3
"""Make libsweetwall's packages from a LayeredAnimation wallpaper of the iOS root.

    python3 -I tools/sunfish/sweetwall-pkg.py SRC.wallpaper OUTDIR

SRC.wallpaper is e.g. /Library/Wallpaper/Collections/iOS_16~iphone.wallpaperCollection/Wallpapers/
7400.WWDC_2022-375w-812h@3x~iphone.wallpaper on a mounted root. Its Wallpaper.plist names the
background, foreground and floating Core Animation packages; each is copied to
OUTDIR/{Background,Foreground,Floating}.ca with its HEIC assets decoded to PNG (heic2png.py,
ffmpeg's HEVC decoder) and main.caml pointing at the PNGs, so SpringBoard's ImageIO needs no
HEVC decoder. The output is Apple's artwork: keep it out of git.
"""
import os
import plistlib
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import heic2png  # noqa: E402

KEYS = (('Background', 'backgroundAnimationFileName'), ('Foreground', 'foregroundAnimationFileName'),
        ('Floating', 'floatingAnimationFileNameKey'))


def find(d, key):
    """The first value for key anywhere in a plist structure."""
    if isinstance(d, dict):
        if key in d:
            return d[key]
        d = list(d.values())
    if isinstance(d, list):
        for v in d:
            r = find(v, key)
            if r is not None:
                return r
    return None


def main():
    src, out = sys.argv[1], sys.argv[2]
    with open(os.path.join(src, 'Wallpaper.plist'), 'rb') as f:
        info = plistlib.load(f)
    assert find(info, 'type') == 'LayeredAnimation', f'{src}: not a LayeredAnimation wallpaper'
    os.makedirs(out, exist_ok=True)
    for name, key in KEYS:
        pkg = find(info, key)
        assert pkg, f'{src}: no {key}'
        dst = os.path.join(out, name + '.ca')
        if os.path.exists(dst):
            shutil.rmtree(dst)
        shutil.copytree(os.path.join(src, pkg), dst)
        caml = os.path.join(dst, 'main.caml')
        with open(caml, encoding='utf-8') as f:
            text = f.read()
        adir = os.path.join(dst, 'assets')
        for a in sorted(os.listdir(adir)):
            stem, ext = os.path.splitext(a)
            if ext.lower() != '.heic':
                continue
            heic2png.convert(os.path.join(adir, a), os.path.join(adir, stem + '.png'))
            os.remove(os.path.join(adir, a))
            ref = 'assets/' + a.replace(' ', '%20')
            assert ref in text, f'{caml}: {ref} not referenced'
            text = text.replace(ref, 'assets/' + stem.replace(' ', '%20') + '.png')
        left = re.findall(r'src="[^"]*\.heic"', text, re.I)      # layer names keep ".HEIC"
        assert not left, f'{caml}: {left} left'
        with open(caml, 'w', encoding='utf-8') as f:
            f.write(text)
        print(f'{pkg} -> {dst}')


main()
