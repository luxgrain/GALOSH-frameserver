#!/usr/bin/env python3
"""Smoke test for the GALOSH frameserver plugin (VapourSynth side).

Gates:
  1. plugin loads + filter runs on YUV420P8 and YUV444P16
  2. denoising works: PSNR(out, clean) > PSNR(noisy, clean) + margin
  3. STRONG: plugin output is BYTE-IDENTICAL to `galosh_yuv_cpu.exe
     --pix=420` on the same planar input with the same flags (the plugin
     embeds the same translation unit; only the plumbing differs)
  4. noise="hold": frame 0 identical to fit; later frames still denoise

Run:  python test_fs_smoke.py
"""
import os, subprocess, sys, tempfile
from pathlib import Path

import numpy as np
import vapoursynth as vs

HERE = Path(__file__).resolve().parent
ROOT = HERE
EXE = ROOT / "extern" / "GALOSH" / "standalone" / "galosh_yuv_cpu.exe"
core = vs.core
core.std.LoadPlugin(path=str(HERE / "galosh_frameserver.dll"))

rng = np.random.default_rng(42)
W, H = 640, 480


def make_planes(depth, sub, seed_shift=0):
    """clean + noisy planar YCbCr code arrays (limited range)."""
    hi = (1 << depth) - 1
    yy, xx = np.mgrid[0:H, 0:W]
    # smooth gradient + disc structure (denoiser-friendly content)
    y = 0.25 + 0.5 * (xx / W) + 0.15 * np.exp(-(((xx - W / 2) ** 2 + (yy - H / 2) ** 2)) / 8000.0)
    cb = 0.10 * np.sin(xx / 37.0) * np.cos(yy / 53.0)
    cr = -0.08 * np.cos(xx / 41.0) * np.sin(yy / 47.0)
    if sub:
        cb = cb.reshape(H // 2, 2, W // 2, 2).mean(axis=(1, 3))
        cr = cr.reshape(H // 2, 2, W // 2, 2).mean(axis=(1, 3))
    g = np.random.default_rng(7 + seed_shift)
    yn = y + g.normal(0, 0.03, y.shape)
    cbn = cb + g.normal(0, 0.03, cb.shape)
    crn = cr + g.normal(0, 0.03, cr.shape)
    s = 1 << (depth - 8)

    def qy(v): return np.clip(np.rint(v * 219 * s + 16 * s), 0, hi)
    def qc(v): return np.clip(np.rint(v * 224 * s + 128 * s), 0, hi)
    dt = np.uint8 if depth <= 8 else np.uint16
    return ((qy(y).astype(dt), qc(cb).astype(dt), qc(cr).astype(dt)),
            (qy(yn).astype(dt), qc(cbn).astype(dt), qc(crn).astype(dt)))


def clip_from_planes(planes, depth, sub, length=1):
    fmt = core.query_video_format(vs.YUV, vs.INTEGER, depth,
                                  1 if sub else 0, 1 if sub else 0)
    c = core.std.BlankClip(width=W, height=H, format=fmt, length=length)

    def put(n, f):
        fo = f.copy()
        for i, p in enumerate(planes):
            np.asarray(fo[i])[:] = p
        return fo
    return core.std.ModifyFrame(c, c, put)


def planes_of(clip, n=0):
    f = clip.get_frame(n)
    return [np.asarray(f[i]).copy() for i in range(3)]


def psnr(a, b, hi):
    mse = float(np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2))
    return 99.0 if mse == 0 else 10 * np.log10(hi * hi / mse)


fails = 0


def check(name, ok, detail=""):
    global fails
    print(f"[{'PASS' if ok else 'FAIL'}] {name} {detail}")
    if not ok: fails += 1


# ---- gate 1+2: 420P8 denoising ----
clean, noisy = make_planes(8, sub=True)
nclip = clip_from_planes(noisy, 8, sub=True)
out = core.galosh.Denoise(nclip, luma=1.0, chroma=1.0)
op = planes_of(out)
p_in = [psnr(noisy[i], clean[i], 255) for i in range(3)]
p_out = [psnr(op[i], clean[i], 255) for i in range(3)]
check("420P8 runs + denoises",
      all(p_out[i] > p_in[i] + 3.0 for i in range(3)),
      f"Y {p_in[0]:.1f}->{p_out[0]:.1f} Cb {p_in[1]:.1f}->{p_out[1]:.1f} "
      f"Cr {p_in[2]:.1f}->{p_out[2]:.1f} dB")

# ---- gate 3: byte-identity vs the canonical exe ----
blob = noisy[0].tobytes() + noisy[1].tobytes() + noisy[2].tobytes()
with tempfile.TemporaryDirectory() as td:
    ip, opath = Path(td) / "in.yuv", Path(td) / "out.yuv"
    ip.write_bytes(blob)
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")  # libgomp
    r = subprocess.run([str(EXE), str(ip), str(opath), str(W), str(H),
                        "1.0", "1.0", "--pix=420", "--depth=8",
                        "--range=limited", "--matrix=bt709",
                        "--eotf=bt709", "--siting=left"],
                       capture_output=True, timeout=600, env=env)
    assert r.returncode == 0, r.stderr.decode("utf-8", "replace")[-300:]
    exe_out = opath.read_bytes()
plug_out = op[0].tobytes() + op[1].tobytes() + op[2].tobytes()
check("420P8 BYTE-IDENTICAL to galosh_yuv_cpu.exe --pix=420",
      plug_out == exe_out,
      "" if plug_out == exe_out else
      f"(differs: {sum(a != b for a, b in zip(plug_out, exe_out))} bytes)")

# ---- gate 4: 444P16 ----
clean4, noisy4 = make_planes(16, sub=False)
out4 = core.galosh.Denoise(clip_from_planes(noisy4, 16, sub=False))
op4 = planes_of(out4)
hi16 = 65535
p_in4 = [psnr(noisy4[i], clean4[i], hi16) for i in range(3)]
p_out4 = [psnr(op4[i], clean4[i], hi16) for i in range(3)]
check("444P16 runs + denoises",
      all(p_out4[i] > p_in4[i] + 3.0 for i in range(3)),
      f"Y {p_in4[0]:.1f}->{p_out4[0]:.1f} dB")

# ---- gate 5: noise="hold" (frame 0 == fit; frame 1 still denoises) ----
c2, n2 = make_planes(8, sub=True, seed_shift=1)
two = clip_from_planes(noisy, 8, sub=True) + clip_from_planes(n2, 8, sub=True)
hold = core.galosh.Denoise(two, noise="hold")
h0 = planes_of(hold, 0)
check("hold frame0 == fit frame0",
      all((h0[i] == op[i]).all() for i in range(3)))
h1 = planes_of(hold, 1)
p1 = [psnr(h1[i], c2[i], 255) for i in range(3)]
p1n = [psnr(n2[i], c2[i], 255) for i in range(3)]
check("hold frame1 denoises with held model",
      all(p1[i] > p1n[i] + 3.0 for i in range(3)),
      f"Y {p1n[0]:.1f}->{p1[0]:.1f} dB")

# ---- gate 6: bad-arg rejection ----
try:
    core.galosh.Denoise(nclip, matrix="bt2021")
    check("bad matrix rejected", False)
except vs.Error:
    check("bad matrix rejected", True)

print("\n" + ("ALL PASS" if fails == 0 else f"{fails} FAILURE(S)"))
sys.exit(1 if fails else 0)
