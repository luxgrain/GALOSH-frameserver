/* galosh_fs_vk.c — Vulkan engine TU for the GALOSH frameserver plugin.
 *
 * EN: Embeds the canonical Vulkan YUV host (extern/GALOSH submodule,
 *     GALOSH_YUV_VK_NOMAIN) in its own translation unit — the CPU TU and
 *     this one only meet through the narrow galosh_fs_vk.h API, so the
 *     two embedded canonical sources cannot collide.  This file adds NO
 *     algorithm code: it reproduces the canonical run_420 composition
 *     (luma-only gray pass at full res + native-lattice chroma pass)
 *     against caller-provided planes instead of files, with the noise
 *     model held in caller memory (galosh_vk_memstate).
 * JP: Vulkan エンジン専用 TU。正規 Vulkan ホストを丸ごと埋め込み、
 *     run_420 と同一の合成をファイル I/O 抜きで再現するだけ。
 *     アルゴリズムコードの追加はゼロ。
 * (code: Apache-2.0)
 */
#define GALOSH_YUV_VK_NOMAIN
#include "extern/GALOSH/standalone/vk/galosh_yuv_vk.c"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "galosh_fs_vk.h"

/* DLL-local module directory -> g_exe_dir so load_spv() finds
 * "<dll dir>/shaders/*.spv" (the CLI derives this from argv[0]). */
static void fsvk_set_module_dir(void)
{
  HMODULE hm = NULL;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCSTR)&fsvk_set_module_dir, &hm);
  GetModuleFileNameA(hm, g_exe_dir, sizeof(g_exe_dir) - 1);
  char *s1 = strrchr(g_exe_dir, '\\'), *s2 = strrchr(g_exe_dir, '/');
  char *cut = s1 > s2 ? s1 : s2;
  if(cut) *cut = 0; else strcpy(g_exe_dir, ".");
}


/* ================================================================
 * Composite transfer-curve LUTs (per-process, rebuilt on eotf change).
 *
 * EN: The four host conversion loops each evaluate a 1-D composite of
 *     two transfer curves per sample (container-EOTF -> linear -> sRGB
 *     gamma for the core, and back).  pow() dominated the frame wall
 *     time (measured 63 ms of a 126 ms 1080p frame), so the composites
 *     are tabulated once (16384 knots + linear interpolation, domain
 *     [-0.25, 1.25] to cover dequant excursions; endpoints clamp).
 *     Curve error ~1e-6 — far below one 16-bit code.  The gray-image
 *     variant (inner [0,1] clip) equals the unclipped composite
 *     evaluated on clamp01(x) because both curves are monotone.
 * JP: 4 本のホスト変換ループの合成カーブを 1 次元 LUT 化（powf 撲滅）。
 *     単調性により内側クリップ版は引数クランプで等価。
 * ================================================================ */
#define FSVK_LUT_N 16384
static struct
{
  int valid;
  galosh420_eotf_t eotf;
  float lo, hi, scale;
  float to_srgb[FSVK_LUT_N + 1];    /* container gamma -> sRGB gamma  */
  float from_srgb[FSVK_LUT_N + 1];  /* sRGB gamma -> container gamma  */
} g_fsvk_lut;

static void fsvk_lut_build(const galosh420_eotf_t eotf)
{
  if(g_fsvk_lut.valid && g_fsvk_lut.eotf == eotf) return;
  g_fsvk_lut.lo = -0.25f; g_fsvk_lut.hi = 1.25f;
  g_fsvk_lut.scale = FSVK_LUT_N / (g_fsvk_lut.hi - g_fsvk_lut.lo);
  for(int i = 0; i <= FSVK_LUT_N; i++)
  {
    const float x = g_fsvk_lut.lo + (float)i / g_fsvk_lut.scale;
    g_fsvk_lut.to_srgb[i] =
        galosh420_eotf_fwd_f(galosh420_eotf_inv_f(x, eotf), GALOSH420_EOTF_SRGB);
    g_fsvk_lut.from_srgb[i] =
        galosh420_eotf_fwd_f(galosh420_eotf_inv_f(x, GALOSH420_EOTF_SRGB), eotf);
  }
  g_fsvk_lut.eotf = eotf;
  g_fsvk_lut.valid = 1;
}

static inline float fsvk_lut_eval(const float *t, float x)
{
  float u = (x - g_fsvk_lut.lo) * g_fsvk_lut.scale;
  if(u < 0.0f) u = 0.0f;
  if(u > (float)FSVK_LUT_N) u = (float)FSVK_LUT_N;
  const int i = (int)u;
  const float f = u - (float)i;
  return t[i] + f * (t[(i < FSVK_LUT_N) ? i + 1 : i] - t[i]);
}
/* clamp01 argument = inner-[0,1]-clip variant (monotone curves) */
static inline float fsvk_c01(float x)
{ return (x < 0.0f) ? 0.0f : (x > 1.0f) ? 1.0f : x; }

int galosh_fsvk_init(void)
{
  static int tried = 0, ok = 0;
  if(tried) return ok ? 0 : 1;
  tried = 1;
  fsvk_set_module_dir();
  g_vk_quiet = 1;                 /* no per-frame spam inside host apps */
  g_verbose = getenv("GALOSH_VERBOSE") != NULL;
  if(galosh_yuv_vk_init_device()) return 1;
  ok = 1;
  return 0;
}

int galosh_fsvk_process(const float *Yp, const float *Cb, const float *Cr,
                        int W, int H, int cw, int ch, int subsampled,
                        galosh420_matrix_t mat, galosh420_eotf_t eotf,
                        galosh420_siting_t siting,
                        float luma, float chroma,
                        float *Yd, float *CbD, float *CrD,
                        galosh_fsvk_state *ml, galosh_fsvk_state *mc)
{
  const size_t ysz = (size_t)W * H;
  const size_t csz = (size_t)cw * ch;
  (void)siting;

  /* ---- LUMA: full-res gray image, luma-only fast path ---- */
  float *gray = (float *)malloc(ysz * 3 * 4);
  if(!gray) return 1;
  fsvk_lut_build(eotf);
  #pragma omp parallel for schedule(static)
  for(long long i = 0; i < (long long)ysz; i++)
  {
    const float v = fsvk_lut_eval(g_fsvk_lut.to_srgb, fsvk_c01(Yp[i]));
    gray[3 * i + 0] = v; gray[3 * i + 1] = v; gray[3 * i + 2] = v;
  }
  if(run_core(gray, W, H, luma, chroma, /*luma_only=*/1, "",
              (galosh_vk_memstate *)ml))
  { free(gray); return 1; }
  #pragma omp parallel for schedule(static)
  for(long long i = 0; i < (long long)ysz; i++)
    Yd[i] = fsvk_lut_eval(g_fsvk_lut.from_srgb, fsvk_c01(gray[3 * i + 1]));
  free(gray);

  /* ---- CHROMA: native lattice (guide built by the CALLER's shared
   * front-end — the caller passes the siting-phased guide via Yg below
   * only implicitly: we rebuild it here to keep the API narrow) ---- */
  float *Yg  = (float *)malloc(csz * 4);
  float *rgb = (float *)malloc(csz * 3 * 4);
  if(!Yg || !rgb) { free(Yg); free(rgb); return 1; }
  if(subsampled)
    galosh420_down_luma(Yp, W, H, Yg, siting);
  else
    memcpy(Yg, Yp, ysz * sizeof(float));
  #pragma omp parallel for schedule(static)
  for(long long i = 0; i < (long long)csz; i++)
  {
    float R, G, B;
    galosh420_ncl_inv(Yg[i], Cb[i], Cr[i], mat, &R, &G, &B);
    rgb[3 * i + 0] = fsvk_lut_eval(g_fsvk_lut.to_srgb, R);
    rgb[3 * i + 1] = fsvk_lut_eval(g_fsvk_lut.to_srgb, G);
    rgb[3 * i + 2] = fsvk_lut_eval(g_fsvk_lut.to_srgb, B);
  }
  if(run_core(rgb, cw, ch, luma, chroma, /*luma_only=*/0, "",
              (galosh_vk_memstate *)mc))
  { free(Yg); free(rgb); return 1; }
  #pragma omp parallel for schedule(static)
  for(long long i = 0; i < (long long)csz; i++)
  {
    const float Rp = fsvk_lut_eval(g_fsvk_lut.from_srgb, rgb[3 * i + 0]);
    const float Gp = fsvk_lut_eval(g_fsvk_lut.from_srgb, rgb[3 * i + 1]);
    const float Bp = fsvk_lut_eval(g_fsvk_lut.from_srgb, rgb[3 * i + 2]);
    float yy;
    galosh420_ncl_fwd(Rp, Gp, Bp, mat, &yy, &CbD[i], &CrD[i]);
  }
  free(Yg); free(rgb);
  return 0;
}
