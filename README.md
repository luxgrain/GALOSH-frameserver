# GALOSH-frameserver — VapourSynth + AviSynth+ plugin, one DLL

[GALOSH](https://github.com/luxgrain/GALOSH) (blind, training-free,
search-free image denoising — [arXiv:2607.03768](https://arxiv.org/abs/2607.03768))
behind a frameserver API. **Frame-independent single-frame denoising
only**: no motion estimation, no temporal filtering — this is the
published single-frame algorithm, verbatim.

The canonical CPU implementation is embedded from the pinned
`extern/GALOSH` submodule (`standalone/galosh_yuv_cpu.c`, included whole
via `GALOSH_YUV_NOMAIN`) — this repository contains **zero algorithm
code**, only container plumbing. The VapourSynth output is
**byte-identical** to `galosh_yuv_cpu.exe --pix=420` on the same input,
enforced by the test suite, so the plugin ⇔ paper correspondence is
machine-checked.

## Usage

VapourSynth:

```python
import vapoursynth as vs
core = vs.core
core.std.LoadPlugin(path="galosh_frameserver.dll")

clip = core.lsmas.LWLibavSource("input.mkv")     # any planar YUV420/444 source
clip = core.galosh.Denoise(clip, luma=1.0, chroma=1.2,
                           matrix="bt709", range="limited",
                           eotf="bt709", siting="left", noise="hold")
clip.set_output()
```

AviSynth+ (same DLL, same arguments):

```avs
LoadPlugin("galosh_frameserver.dll")

LWLibavVideoSource("input.mkv")                  # any planar YUV420/444 source
galosh_Denoise(luma=1.0, chroma=1.2, \
               matrix="bt709", range="limited", \
               eotf="bt709", siting="left", noise="hold")
```

Minimal forms — every argument is optional (defaults shown below):

```avs
galosh_Denoise()                        # AviSynth+: fully blind, bt709/limited
```
```python
clip = core.galosh.Denoise(clip)        # VapourSynth: same
```

High-bit-depth / other colorimetry, e.g. 10-bit HLG broadcast material:

```avs
galosh_Denoise(matrix="bt2020", eotf="hlg", siting="topleft")
```

- Formats: integer YUV420 / YUV444, 8–16 bit (convert other formats first).
- `matrix` = `bt601|bt709|bt2020|custom:Kr,Kb` · `range` = `limited|full` ·
  `eotf` = `srgb|g22|g24|bt709|hlg|pq|linear` ·
  `siting` = `center|left|topleft` (chroma sample-center position).
  Defaults are the video conventions: bt709 / limited / bt709 / left.
- `noise` = `fit`: blind Poisson–Gaussian fit per frame (deterministic).
  `noise` = `hold`: fit once on the first frame this instance processes,
  reuse for the rest — intended for linear encodes (with heavy seeking the
  fitted frame may vary).
- The filter processes one frame at a time per instance and is
  OpenMP-parallel across all cores *inside* the frame (host-level frame
  parallelism would only oversubscribe).

## Build (Windows / MSYS2 ucrt64)

```sh
git clone --recursive https://github.com/luxgrain/GALOSH-frameserver.git
cd GALOSH-frameserver
PATH=/c/msys64/ucrt64/bin:$PATH ./build_frameserver.sh
```

VapourSynth headers are auto-detected (pip install or SDK). For the
AviSynth+ entry point, fetch the headers once (GPLv2 with plugin-linking
exception — deliberately not vendored in this Apache-2.0 repo):

```sh
mkdir -p include_avs
curl -sL https://github.com/AviSynth/AviSynthPlus/archive/refs/tags/v3.7.5.tar.gz \
  | tar -xz --strip-components=3 -C include_avs AviSynthPlus-3.7.5/avs_core/include
```

The dual build links against the installed `AviSynth.dll`; without
AviSynth+ the DLL builds as pure-VapourSynth (auto-detected). MinGW
runtimes are linked statically, so the DLL loads in hosts without a MSYS2
environment.

## Tests

```sh
# canonical exe for the byte-identity gate:
(cd extern/GALOSH/standalone && bash build.sh yuv)
python test_fs_smoke.py       # VapourSynth: 6 gates incl. byte-identity
```

- `test_fs_smoke.py` — 420P8/444P16 denoise gates, byte-identity vs the
  canonical exe, `hold` semantics, bad-arg rejection.
- `test_avs_smoke.c` — AviSynth+ script-level run via the C API (flat
  gray + AddGrainC → MAD must drop; no external runner needed).

## Known limitations (v0)

- CPU engine (~0.3 s/frame at 1080p on a desktop CPU). A Vulkan backend
  (2.6 ms/frame at 1080p in the engine's video mode) is a planned
  follow-up.
- 4:2:2 not yet accepted natively — convert to 4:4:4 first.
- Frame props (`_Matrix` / `_ColorRange` / `_ChromaLocation`) are not read
  yet; colorimetry is explicit-argument only.

## License

Apache-2.0 (same as GALOSH). The AviSynth+ headers are fetched at build
time under their own license (GPLv2 with the plugin linking exception).
