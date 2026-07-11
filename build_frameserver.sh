#!/usr/bin/env bash
# build_frameserver.sh — GALOSH frameserver plugin DLL (VapourSynth +
# optional AviSynth+ entry in the SAME DLL).
#
# Usage:
#   PATH=/c/msys64/ucrt64/bin:$PATH ./build_frameserver.sh
# Env:
#   VS_INCLUDE  — dir containing VapourSynth4.h (auto-probed from the pip
#                 install if unset)
#   AVS_INCLUDE — dir containing avisynth_c.h (AviSynth+ FilterSDK); if set,
#                 the AviSynth entry point is compiled in (GALOSH_HAVE_AVISYNTH)
set -eu
cd "$(dirname "$0")"

if [ -z "${VS_INCLUDE:-}" ]; then
  for c in \
    "/c/Users/${USERNAME:-$(whoami)}/AppData/Local/Programs/Python/Python312/Lib/site-packages/vapoursynth/include" \
    "/c/Program Files/VapourSynth/sdk/include/vapoursynth"; do
    if [ -f "$c/VapourSynth4.h" ]; then VS_INCLUDE="$c"; break; fi
  done
fi
[ -f "${VS_INCLUDE:-}/VapourSynth4.h" ] || { echo "VapourSynth4.h not found — set VS_INCLUDE"; exit 1; }

# AviSynth+ side: headers auto-detected in include_avs/ (fetch once with
#   curl -sL https://github.com/AviSynth/AviSynthPlus/archive/refs/tags/v3.7.5.tar.gz \
#     | tar -xz --strip-components=3 -C include_avs AviSynthPlus-3.7.5/avs_core/include
# — GPLv2 WITH plugin linking exception, kept out of the repo), linked
# directly against the installed AviSynth.dll.
# NOTE: the dual DLL then IMPORTS AviSynth.dll — on machines WITHOUT
# AviSynth+ installed, build without AVS (pure-VS DLL) instead.
AVS_INCLUDE="${AVS_INCLUDE:-include_avs}"
AVS_FLAGS=""; AVS_LINK=""
AVS_DLL="${AVS_DLL:-/c/Windows/System32/AviSynth.dll}"
if [ -f "$AVS_INCLUDE/avisynth_c.h" ] && [ -f "$AVS_DLL" ]; then
  AVS_FLAGS="-DGALOSH_HAVE_AVISYNTH -I$AVS_INCLUDE"
  AVS_LINK="$AVS_DLL"
  echo "AviSynth+ entry: ENABLED ($AVS_INCLUDE, $AVS_DLL)"
else
  echo "AviSynth+ entry: disabled (need $AVS_INCLUDE/avisynth_c.h + $AVS_DLL)"
fi

# -static/-static-libgcc: bundle the MinGW runtimes into the DLL so the
# plugin loads inside VS/AVS hosts that don't have ucrt64 on PATH.
gcc -O3 -fopenmp -std=gnu11 -shared \
    -static -static-libgcc \
    -I"$VS_INCLUDE" $AVS_FLAGS \
    galosh_frameserver.c $AVS_LINK -o galosh_frameserver.dll -lm
echo "galosh_frameserver.dll built"
