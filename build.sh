#!/bin/bash
# Kobo Deluxe CD32X - REAL HARDWARE build (kobo32x_hw): SH2 -> Sub-CPU/68K -> mixed-mode disc.
# Same steps as kobo32x/build.sh, plus a safety check: BOTH programs must be in cart mode
# (KOBO_NOCART 0), so a Fusion (no-cart) setting can never end up on a hardware disc by mistake.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
echo "==> HARDWARE BUILD (cart mode) in $HERE"

grep -q '^#define KOBO_NOCART 0' "$HERE/sh2/sh2_main.c" \
  || { echo "*** sh2/sh2_main.c is not in cart mode (#define KOBO_NOCART 0) - stopping"; exit 1; }
grep -q '^#define KOBO_NOCART     0' "$HERE/subcpu/main.c" \
  || { echo "*** subcpu/main.c is not in cart mode (#define KOBO_NOCART     0) - stopping"; exit 1; }
echo "==> both programs are in cart mode - the cart must be plugged in on the console"

# Track02 length for the demo delay (sh2_main.c currently overrides it by hand: 60 s)
python3 - "$HERE/subcpu/Track02.wav" "$HERE/sh2/track_len.h" <<'PY'
import sys, struct
path, out = sys.argv[1], sys.argv[2]
d = open(path, "rb").read()
if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
    sys.exit("*** %s is not a WAV file" % path)
pos, byterate, datalen = 12, 0, 0
while pos + 8 <= len(d):
    cid, n = d[pos:pos + 4], struct.unpack("<I", d[pos + 4:pos + 8])[0]
    if cid == b"fmt ": byterate = struct.unpack("<I", d[pos + 16:pos + 20])[0]
    if cid == b"data": datalen = min(n, len(d) - pos - 8)
    pos += 8 + n + (n & 1)
if not byterate or not datalen:
    sys.exit("*** could not read the length of %s" % path)
secs = (datalen + byterate - 1) // byterate
open(out, "w").write("#define TRACK02_SECONDS %d   /* written by build.sh from Track02.wav */\n" % secs)
print("==> Track02.wav: %d s" % secs)
PY

echo "==> SH2";         cd "$HERE/sh2"    && make clean && make
echo "==> Sub-CPU";     cd "$HERE/subcpu" && make clean && make cd
echo "==> disc image";  python3 make_mixed_cd2.py CDROMPlayer.iso Track02.wav Track03.wav Track04.wav
echo "==> done: HARDWARE disc image is in $HERE/subcpu/ - burn it and play WITH the cart"
ls -1 "$HERE/subcpu" | grep -i "mixed" || true
