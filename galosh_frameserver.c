/* galosh_frameserver.c — GALOSH as a frameserver plugin (VapourSynth now,
 * AviSynth+ entry in the same DLL when built with GALOSH_HAVE_AVISYNTH).
 *
 * EN: Frame-INDEPENDENT single-frame denoising only — this is the
 *     SPL-submitted GALOSH algorithm behind a frameserver API; no motion
 *     estimation, no temporal filtering, no claim extensions.  The whole
 *     canonical CPU implementation is embedded by #including
 *     standalone/galosh_yuv_cpu.c (GALOSH_YUV_NOMAIN) — zero algorithm
 *     code is duplicated here; this file is container plumbing only.
 * JP: 完全フレーム独立（SPL 投稿済み単フレームアルゴリズムの配管のみ）。
 *     ME/MC/時間方向処理は入れない。正規実装を .c ごと include。
 *
 * API (VapourSynth):
 *   core.galosh.Denoise(clip, luma=1.0, chroma=1.0,
 *                       matrix="bt709", range="limited", eotf="bt709",
 *                       siting="left", noise="fit")
 *   clip: YUV420P8..P16 / YUV444P8..P16 (integer).
 *   noise="fit"  : blind Poisson-Gaussian fit per frame (deterministic,
 *                  matches `galosh_yuv_cpu.exe --pix=...` bit-for-bit).
 *   noise="hold" : fit once on the first frame this instance processes,
 *                  reuse for all others (the exe's --noise-state semantics
 *                  moved into instance memory; intended for linear encodes
 *                  — with heavy seeking the fitted frame may vary).
 *
 * Threading: the filter runs one frame at a time per instance
 *   (fmUnordered / MT_SERIALIZED) because the embedded pipeline is already
 *   OpenMP-parallel inside a frame — host-level frame parallelism would
 *   only oversubscribe cores, and serial frames make `hold` race-free.
 *
 * (code: Apache-2.0)
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- embed the canonical CPU implementation (no CLI main) ---- */
#define GALOSH_YUV_NOMAIN
#include "extern/GALOSH/standalone/galosh_yuv_cpu.c"

/* ================================================================
 * Process-wide serialization (BUGFIX 2026-07-11).
 *
 * EN: The embedded canonical pipeline keeps the inverse-GAT LUT (and its
 *     per-frame rebuild) in GLOBALS — harmless in the single-image exe,
 *     but two concurrently-processing FILTER INSTANCES (settings-
 *     comparison scripts, previewers with parallel requests) rebuild the
 *     LUT under each other's frames; the OpenMP row loops then read a
 *     half-swapped table → horizontal garbage bands.  fmUnordered / AVS
 *     MT flags only serialize WITHIN one instance, so frame processing
 *     takes a process-wide lock.  Throughput cost ~0: each frame already
 *     saturates every core via the internal OpenMP parallelism.
 * JP: 正規実装の逆 GAT LUT はグローバル（単一画像 exe では無害）。複数
 *     インスタンス並行で帯状破壊 → プロセス全域ロックで直列化。フレーム
 *     内部が OpenMP 全コア並列なので性能損失は実質ゼロ。
 * ================================================================ */
static SRWLOCK g_galosh_proc_lock = SRWLOCK_INIT;

/* ================================================================
 * Host-agnostic core
 * ================================================================ */
typedef struct
{
  float luma, chroma;
  galosh420_matrix_t mat;
  galosh420_range_t  range;
  galosh420_eotf_t   eotf;
  galosh420_siting_t siting;
  int   noise_hold;                    /* 0 = fit per frame, 1 = hold */
  /* held blind fit (guarded by host-serialized frame processing) */
  int   have_l, have_c;
  float l_a, l_s2, c_a, c_s2;
} galosh_fs;

/* Read a (possibly strided) integer plane into a contiguous float array of
 * container CODE VALUES (dequantization happens separately). */
static void fs_load_plane(const uint8_t *src, ptrdiff_t stride,
                          int w, int h, int wide, float *dst)
{
  for(int y = 0; y < h; y++)
  {
    const uint8_t *row = src + (ptrdiff_t)y * stride;
    float *o = dst + (size_t)y * w;
    if(wide)
    {
      const uint16_t *r16 = (const uint16_t *)row;
      for(int x = 0; x < w; x++) o[x] = (float)r16[x];
    }
    else
      for(int x = 0; x < w; x++) o[x] = (float)row[x];
  }
}

static void fs_store_code(uint8_t *dst, ptrdiff_t stride, int w, int y,
                          int wide, int x, int code)
{
  if(wide) ((uint16_t *)(dst + (ptrdiff_t)y * stride))[x] = (uint16_t)code;
  else     (dst + (ptrdiff_t)y * stride)[x] = (uint8_t)code;
}

/* One frame: planar integer YCbCr in -> denoised planar integer YCbCr out.
 * subsampled: 1 = 4:2:0 (cw=W/2, ch=H/2), 0 = 4:4:4.  Mirrors
 * galosh_yuv420_main() exactly (same call sequence, same clips), with the
 * blind fit hoisted out so `hold` can cache it.  Returns 0 on success. */
static int fs_process(galosh_fs *p,
                      const uint8_t *sY, ptrdiff_t stY,
                      const uint8_t *sU, ptrdiff_t stU,
                      const uint8_t *sV, ptrdiff_t stV,
                      uint8_t *dY, ptrdiff_t dtY,
                      uint8_t *dU, ptrdiff_t dtU,
                      uint8_t *dV, ptrdiff_t dtV,
                      const int W, const int H, const int depth,
                      const int subsampled)
{
  const int wide = (depth > 8);
  const int cw = subsampled ? W / 2 : W;
  const int ch = subsampled ? H / 2 : H;
  const size_t ysz = (size_t)W * H;
  const size_t csz = (size_t)cw * ch;

  /* serialize ALL instances in the process (see g_galosh_proc_lock) */
  AcquireSRWLockExclusive(&g_galosh_proc_lock);

  float *Yp   = dt_alloc_align_float(ysz);
  float *Cb   = dt_alloc_align_float(csz);
  float *Cr   = dt_alloc_align_float(csz);
  float *Ylin = dt_alloc_align_float(ysz);
  float *Yden = dt_alloc_align_float(ysz);
  float *Yg   = dt_alloc_align_float(csz);
  float *rgb  = dt_alloc_align_float(csz * 3);
  float *yint = dt_alloc_align_float(csz);
  if(!Yp || !Cb || !Cr || !Ylin || !Yden || !Yg || !rgb || !yint)
  {
    fprintf(stderr, "[galosh_fs] alloc failed\n");
    ReleaseSRWLockExclusive(&g_galosh_proc_lock);
    return 1;
  }

  /* ---- load + dequantise (gamma-domain container values) ---- */
  fs_load_plane(sY, stY, W, H, wide, Yp);
  fs_load_plane(sU, stU, cw, ch, wide, Cb);
  fs_load_plane(sV, stV, cw, ch, wide, Cr);
  DT_OMP_FOR()
  for(size_t i = 0; i < ysz; i++)
    Yp[i] = galosh420_dequant_y(Yp[i], depth, p->range);
  DT_OMP_FOR()
  for(size_t i = 0; i < csz; i++)
  {
    Cb[i] = galosh420_dequant_c(Cb[i], depth, p->range);
    Cr[i] = galosh420_dequant_c(Cr[i], depth, p->range);
  }

  /* ---- LUMA (full res, chroma-independent) ---- */
  DT_OMP_FOR()
  for(size_t i = 0; i < ysz; i++)
    Ylin[i] = galosh420_eotf_inv_f(Yp[i], p->eotf);
  /* blind fit hoisted from galosh_yuv_denoise_luma_plane (same formulas,
   * same estimator, same buffer) so `hold` can cache the scalars. */
  if(!(p->noise_hold && p->have_l))
  {
    const float s = estimate_sigma_plane(Ylin, W, H);
    p->l_s2 = fmaxf(s * s, 1e-8f);
    p->l_a  = fmaxf(s * 0.1f, 1e-5f);
    p->have_l = 1;
  }
  galosh_yuv_denoise_luma_plane(Ylin, Yden, W, H, p->luma, p->l_a, p->l_s2);

  /* ---- CHROMA (native lattice; guide phase per siting) ---- */
  if(subsampled)
    galosh420_down_luma(Yp, W, H, Yg, p->siting);
  else
    memcpy(Yg, Yp, ysz * sizeof(float));
  DT_OMP_FOR()
  for(size_t i = 0; i < csz; i++)
  {
    float R, G, B;
    galosh420_ncl_inv(Yg[i], Cb[i], Cr[i], p->mat, &R, &G, &B);
    rgb[3 * i + 0] = galosh420_eotf_inv_f(R, p->eotf);
    rgb[3 * i + 1] = galosh420_eotf_inv_f(G, p->eotf);
    rgb[3 * i + 2] = galosh420_eotf_inv_f(B, p->eotf);
  }
  /* blind fit hoisted from galosh_yuv_denoise_linear_rgb: internal BT.709
   * Y of the reconstruction (identical estimator input). */
  if(!(p->noise_hold && p->have_c))
  {
    DT_OMP_FOR()
    for(size_t i = 0; i < csz; i++)
    {
      float Y, cb2, cr2;
      rgb_to_ycbcr(rgb[3 * i + 0], rgb[3 * i + 1], rgb[3 * i + 2],
                   &Y, &cb2, &cr2);
      yint[i] = Y;
    }
    const float s = estimate_sigma_plane(yint, cw, ch);
    p->c_s2 = fmaxf(s * s, 1e-8f);
    p->c_a  = fmaxf(s * 0.1f, 1e-5f);
    p->have_c = 1;
  }
  galosh_yuv_denoise_linear_rgb(rgb, rgb, cw, ch, p->luma, p->chroma,
                                p->c_a, p->c_s2);

  /* ---- requantise + store (format-preserving; same clips as the exe) --- */
  const int hi = (1 << depth) - 1;
  (void)hi;
  DT_OMP_FOR()
  for(int y = 0; y < H; y++)
    for(int x = 0; x < W; x++)
    {
      const size_t i = (size_t)y * W + x;
      const float yl = fminf(fmaxf(Yden[i], 0.0f), 1.0f);
      fs_store_code(dY, dtY, W, y, wide, x,
                    galosh420_requant_y(galosh420_eotf_fwd_f(yl, p->eotf),
                                        depth, p->range));
    }
  DT_OMP_FOR()
  for(int y = 0; y < ch; y++)
    for(int x = 0; x < cw; x++)
    {
      const size_t i = (size_t)y * cw + x;
      const float Rp = galosh420_eotf_fwd_f(rgb[3 * i + 0], p->eotf);
      const float Gp = galosh420_eotf_fwd_f(rgb[3 * i + 1], p->eotf);
      const float Bp = galosh420_eotf_fwd_f(rgb[3 * i + 2], p->eotf);
      float yy, cbo, cro;
      galosh420_ncl_fwd(Rp, Gp, Bp, p->mat, &yy, &cbo, &cro);
      fs_store_code(dU, dtU, cw, y, wide, x,
                    galosh420_requant_c(cbo, depth, p->range));
      fs_store_code(dV, dtV, cw, y, wide, x,
                    galosh420_requant_c(cro, depth, p->range));
    }

  dt_free_align(Yp);   dt_free_align(Cb);  dt_free_align(Cr);
  dt_free_align(Ylin); dt_free_align(Yden);
  dt_free_align(Yg);   dt_free_align(rgb); dt_free_align(yint);
  ReleaseSRWLockExclusive(&g_galosh_proc_lock);
  return 0;
}

/* shared string-arg parsing (defaults per API doc above) */
static int fs_parse_args(galosh_fs *p,
                         const char *matrix, const char *range,
                         const char *eotf, const char *siting,
                         const char *noise, char *errbuf, size_t errlen)
{
  p->mat = GALOSH420_MAT_BT709;
  p->range = GALOSH420_RANGE_LIMITED;
  p->eotf = GALOSH420_EOTF_BT709;
  p->siting = GALOSH420_SITING_LEFT;
  p->noise_hold = 0;
  p->have_l = p->have_c = 0;
  if(matrix && galosh420_parse_matrix(matrix, &p->mat))
  { snprintf(errbuf, errlen, "bad matrix '%s' (bt601|bt709|bt2020|custom:Kr,Kb)", matrix); return 1; }
  if(range && galosh420_parse_range(range, &p->range))
  { snprintf(errbuf, errlen, "bad range '%s' (limited|full)", range); return 1; }
  if(eotf && galosh420_parse_eotf(eotf, &p->eotf))
  { snprintf(errbuf, errlen, "bad eotf '%s' (srgb|g22|g24|bt709|hlg|pq|linear)", eotf); return 1; }
  if(siting && galosh420_parse_siting(siting, &p->siting))
  { snprintf(errbuf, errlen, "bad siting '%s' (center|left|topleft)", siting); return 1; }
  if(noise)
  {
    if(strcmp(noise, "fit") == 0)       p->noise_hold = 0;
    else if(strcmp(noise, "hold") == 0) p->noise_hold = 1;
    else { snprintf(errbuf, errlen, "bad noise '%s' (fit|hold)", noise); return 1; }
  }
  return 0;
}

/* ================================================================
 * VapourSynth (API 4) wrapper
 * ================================================================ */
#include "VapourSynth4.h"
#include "VSHelper4.h"

typedef struct { VSNode *node; galosh_fs p; } vs_data;

static const VSFrame *VS_CC vs_getframe(int n, int activationReason,
    void *instanceData, void **frameData, VSFrameContext *frameCtx,
    VSCore *core, const VSAPI *vsapi)
{
  vs_data *d = (vs_data *)instanceData;
  (void)frameData;
  if(activationReason == arInitial)
  {
    vsapi->requestFrameFilter(n, d->node, frameCtx);
    return NULL;
  }
  if(activationReason != arAllFramesReady) return NULL;

  const VSFrame *src = vsapi->getFrameFilter(n, d->node, frameCtx);
  const VSVideoFormat *fi = vsapi->getVideoFrameFormat(src);
  const int W = vsapi->getFrameWidth(src, 0);
  const int H = vsapi->getFrameHeight(src, 0);
  VSFrame *dst = vsapi->newVideoFrame(fi, W, H, src, core);

  const int rc = fs_process(&d->p,
      vsapi->getReadPtr(src, 0), vsapi->getStride(src, 0),
      vsapi->getReadPtr(src, 1), vsapi->getStride(src, 1),
      vsapi->getReadPtr(src, 2), vsapi->getStride(src, 2),
      vsapi->getWritePtr(dst, 0), vsapi->getStride(dst, 0),
      vsapi->getWritePtr(dst, 1), vsapi->getStride(dst, 1),
      vsapi->getWritePtr(dst, 2), vsapi->getStride(dst, 2),
      W, H, fi->bitsPerSample, fi->subSamplingW ? 1 : 0);
  vsapi->freeFrame(src);
  if(rc)
  {
    vsapi->setFilterError("galosh.Denoise: frame processing failed (alloc)", frameCtx);
    vsapi->freeFrame(dst);
    return NULL;
  }
  return dst;
}

static void VS_CC vs_free(void *instanceData, VSCore *core, const VSAPI *vsapi)
{
  vs_data *d = (vs_data *)instanceData;
  (void)core;
  vsapi->freeNode(d->node);
  free(d);
}

static void VS_CC vs_create(const VSMap *in, VSMap *out, void *userData,
                            VSCore *core, const VSAPI *vsapi)
{
  (void)userData;
  vs_data *d = (vs_data *)calloc(1, sizeof(vs_data));
  int err = 0;
  char ebuf[256];

  d->node = vsapi->mapGetNode(in, "clip", 0, NULL);
  const VSVideoInfo *vi = vsapi->getVideoInfo(d->node);

  if(!vsh_isConstantVideoFormat(vi) ||
     vi->format.colorFamily != cfYUV ||
     vi->format.sampleType != stInteger ||
     vi->format.bitsPerSample < 8 || vi->format.bitsPerSample > 16 ||
     vi->format.subSamplingW != vi->format.subSamplingH ||
     vi->format.subSamplingW > 1)
  {
    vsapi->mapSetError(out, "galosh.Denoise: only constant-format integer "
                            "YUV420P8..P16 / YUV444P8..P16 are supported "
                            "(use resize/fmtc for other formats)");
    vsapi->freeNode(d->node);
    free(d);
    return;
  }

  d->p.luma = (float)vsapi->mapGetFloat(in, "luma", 0, &err);
  if(err) d->p.luma = 1.0f;
  d->p.chroma = (float)vsapi->mapGetFloat(in, "chroma", 0, &err);
  if(err) d->p.chroma = 1.0f;

  const char *matrix = vsapi->mapGetData(in, "matrix", 0, &err); if(err) matrix = NULL;
  const char *range  = vsapi->mapGetData(in, "range",  0, &err); if(err) range  = NULL;
  const char *eotf   = vsapi->mapGetData(in, "eotf",   0, &err); if(err) eotf   = NULL;
  const char *siting = vsapi->mapGetData(in, "siting", 0, &err); if(err) siting = NULL;
  const char *noise  = vsapi->mapGetData(in, "noise",  0, &err); if(err) noise  = NULL;
  if(fs_parse_args(&d->p, matrix, range, eotf, siting, noise, ebuf, sizeof(ebuf)))
  {
    char msg[300];
    snprintf(msg, sizeof(msg), "galosh.Denoise: %s", ebuf);
    vsapi->mapSetError(out, msg);
    vsapi->freeNode(d->node);
    free(d);
    return;
  }

  init_galosh_kaiser();   /* idempotent table init (CLI main does this) */

  /* fmUnordered: one frame at a time per instance — the embedded pipeline
   * is OpenMP-parallel inside the frame, and `hold` stays race-free. */
  VSFilterDependency deps[] = { { d->node, rpStrictSpatial } };
  vsapi->createVideoFilter(out, "Denoise", vi, vs_getframe, vs_free,
                           fmUnordered, deps, 1, d, core);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin,
                                             const VSPLUGINAPI *vspapi)
{
  vspapi->configPlugin("com.luxgrain.galosh", "galosh",
                       "GALOSH blind training-free denoiser (single-frame)",
                       VS_MAKE_VERSION(0, 4), VAPOURSYNTH_API_VERSION, 0, plugin);
  vspapi->registerFunction("Denoise",
      "clip:vnode;luma:float:opt;chroma:float:opt;"
      "matrix:data:opt;range:data:opt;eotf:data:opt;"
      "siting:data:opt;noise:data:opt;",
      "clip:vnode;", vs_create, NULL, plugin);
}

/* ================================================================
 * AviSynth+ (C API) wrapper — same DLL, second entry point.
 * Built when GALOSH_HAVE_AVISYNTH is defined and avisynth_c.h is on the
 * include path (headers ship with the AviSynth+ FilterSDK; they carry the
 * plugin linking exception).  Declared MT_SERIALIZED for the same reason
 * the VS side uses fmUnordered.
 * ================================================================ */
#ifdef GALOSH_HAVE_AVISYNTH
#include "avisynth_c.h"

typedef struct { galosh_fs p; } avs_data;

static AVS_VideoFrame *AVSC_CC avs_getframe(AVS_FilterInfo *fi, int n)
{
  avs_data *d = (avs_data *)fi->user_data;
  AVS_VideoFrame *src = avs_get_frame(fi->child, n);
  if(!src) return NULL;
  AVS_VideoFrame *dst = avs_new_video_frame_p(fi->env, &fi->vi, src);

  const int W = fi->vi.width, H = fi->vi.height;
  const int depth = avs_bits_per_component(&fi->vi);
  const int sub = avs_is_420(&fi->vi) ? 1 : 0;

  const int rc = fs_process(&d->p,
      avs_get_read_ptr_p(src, AVS_PLANAR_Y), avs_get_pitch_p(src, AVS_PLANAR_Y),
      avs_get_read_ptr_p(src, AVS_PLANAR_U), avs_get_pitch_p(src, AVS_PLANAR_U),
      avs_get_read_ptr_p(src, AVS_PLANAR_V), avs_get_pitch_p(src, AVS_PLANAR_V),
      avs_get_write_ptr_p(dst, AVS_PLANAR_Y), avs_get_pitch_p(dst, AVS_PLANAR_Y),
      avs_get_write_ptr_p(dst, AVS_PLANAR_U), avs_get_pitch_p(dst, AVS_PLANAR_U),
      avs_get_write_ptr_p(dst, AVS_PLANAR_V), avs_get_pitch_p(dst, AVS_PLANAR_V),
      W, H, depth, sub);
  avs_release_video_frame(src);
  if(rc)
  {
    avs_release_video_frame(dst);
    fi->error = "galosh_Denoise: frame processing failed (alloc)";
    return NULL;
  }
  return dst;
}

static void AVSC_CC avs_freefilter(AVS_FilterInfo *fi)
{
  free(fi->user_data);
}

static AVS_Value AVSC_CC avs_create_denoise(AVS_ScriptEnvironment *env,
                                            AVS_Value args, void *userData)
{
  (void)userData;
  AVS_FilterInfo *fi;
  AVS_Clip *clip = avs_new_c_filter(env, &fi, avs_array_elt(args, 0), 1);
  avs_data *d = (avs_data *)calloc(1, sizeof(avs_data));
  char ebuf[256];

  const int depth = avs_bits_per_component(&fi->vi);
  if(!avs_is_planar(&fi->vi) || avs_is_rgb(&fi->vi) ||
     !(avs_is_420(&fi->vi) || avs_is_444(&fi->vi)) ||
     depth < 8 || depth > 16 ||
     avs_component_size(&fi->vi) > 2)
  {
    avs_release_clip(clip);
    free(d);
    return avs_new_value_error("galosh_Denoise: only integer YUV420/YUV444 "
                               "(8-16 bit planar) is supported");
  }

  d->p.luma   = avs_defined(avs_array_elt(args, 1))
              ? (float)avs_as_float(avs_array_elt(args, 1)) : 1.0f;
  d->p.chroma = avs_defined(avs_array_elt(args, 2))
              ? (float)avs_as_float(avs_array_elt(args, 2)) : 1.0f;
  const char *matrix = avs_defined(avs_array_elt(args, 3)) ? avs_as_string(avs_array_elt(args, 3)) : NULL;
  const char *range  = avs_defined(avs_array_elt(args, 4)) ? avs_as_string(avs_array_elt(args, 4)) : NULL;
  const char *eotf   = avs_defined(avs_array_elt(args, 5)) ? avs_as_string(avs_array_elt(args, 5)) : NULL;
  const char *siting = avs_defined(avs_array_elt(args, 6)) ? avs_as_string(avs_array_elt(args, 6)) : NULL;
  const char *noise  = avs_defined(avs_array_elt(args, 7)) ? avs_as_string(avs_array_elt(args, 7)) : NULL;
  if(fs_parse_args(&d->p, matrix, range, eotf, siting, noise, ebuf, sizeof(ebuf)))
  {
    avs_release_clip(clip);
    free(d);
    return avs_new_value_error("galosh_Denoise: bad argument (see docs)");
  }

  init_galosh_kaiser();

  fi->user_data = d;
  fi->get_frame = avs_getframe;
  fi->free_filter = avs_freefilter;
  AVS_Value ret = avs_new_value_clip(clip);
  avs_release_clip(clip);
  return ret;
}

const char *AVSC_CC avisynth_c_plugin_init(AVS_ScriptEnvironment *env)
{
  avs_add_function(env, "galosh_Denoise",
      "c[luma]f[chroma]f[matrix]s[range]s[eotf]s[siting]s[noise]s",
      avs_create_denoise, NULL);
  return "GALOSH blind training-free denoiser (single-frame)";
}
#endif /* GALOSH_HAVE_AVISYNTH */
