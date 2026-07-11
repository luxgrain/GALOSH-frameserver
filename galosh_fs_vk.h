/* galosh_fs_vk.h — narrow C API between the frameserver plugin TU and the
 * Vulkan engine TU (galosh_fs_vk.c embeds the canonical Vulkan host in a
 * SEPARATE translation unit to avoid symbol collisions with the embedded
 * CPU implementation).  (code: Apache-2.0) */
#ifndef GALOSH_FS_VK_H
#define GALOSH_FS_VK_H

#include "extern/GALOSH/standalone/galosh_yuv420.h"

/* mirrors galosh_vk_memstate in the canonical vk host */
typedef struct { int valid; float alpha, sigma_sq, sigma_gat; } galosh_fsvk_state;

/* One-time device init (idempotent). 0 = Vulkan engine available.
 * Honors GALOSH_VK_DEVICE / GALOSH_SG.  Loads SPIR-V from
 * "<dll dir>/shaders/". */
int galosh_fsvk_init(void);

/* One frame at the GPU: dequantised gamma-domain planes in, denoised
 * gamma-domain planes out (Yd / CbD / CrD, caller-allocated).  Mirrors
 * the canonical run_420 composition (luma-only gray pass + native-lattice
 * chroma pass).  ml / mc = per-instance noise state (valid=0 -> fit and
 * store back; valid=1 -> hold).  Caller must hold its process lock (the
 * device and descriptor pool are process-global).  0 = success. */
int galosh_fsvk_process(const float *Yp, const float *Cb, const float *Cr,
                        int W, int H, int cw, int ch, int subsampled,
                        galosh420_matrix_t mat, galosh420_eotf_t eotf,
                        galosh420_siting_t siting,
                        float luma, float chroma,
                        float *Yd, float *CbD, float *CrD,
                        galosh_fsvk_state *ml, galosh_fsvk_state *mc);

#endif
