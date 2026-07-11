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

# Vulkan engine (engine="vulkan"): compile the 14 SPIR-V kernels the
# canonical vk YUV host dispatches into ./shaders/ next to the DLL
# (needs glslc; skipped with a warning if absent — the DLL then still
# builds and engine="cpu" works, engine="vulkan" errors at filter create).
VK_SHADERS="yuv_srgb2ycc yuv_ycc2srgb yuv_lap_mad yuv_lap_mad_h16 \
yuv_synth_alpha yuv_gat_fwd yuv_sigma_norm yuv_sigma_denorm yuv_makitalo \
yuv_loess o32_build_inv_lut o32_lut_finalize o32_pass12 o32_pass12_sg"
if command -v glslc >/dev/null 2>&1; then
  mkdir -p shaders
  for k in $VK_SHADERS; do
    glslc -O --target-env=vulkan1.2 \
      "extern/GALOSH/standalone/vk/shaders/$k.comp" -o "shaders/$k.spv"
  done
  echo "vulkan shaders: $(echo $VK_SHADERS | wc -w) compiled -> shaders/"
else
  echo "WARNING: glslc not found — engine=vulkan will be unavailable"
fi

# MinGW runtimes (gcc/gomp/winpthread) are bundled statically so the DLL
# loads inside VS/AVS hosts without ucrt64 on PATH; vulkan-1 stays a
# dynamic import (the Khronos loader ships with every GPU driver).
gcc -O3 -fopenmp -std=gnu11 -shared \
    -static-libgcc \
    -I"$VS_INCLUDE" $AVS_FLAGS \
    galosh_frameserver.c galosh_fs_vk.c $AVS_LINK \
    -o galosh_frameserver.dll \
    -Wl,-Bstatic -lgomp -lwinpthread -Wl,-Bdynamic -lvulkan-1 -lm
echo "galosh_frameserver.dll built"
