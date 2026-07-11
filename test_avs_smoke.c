/* test_avs_smoke.c — minimal AviSynth+ smoke harness for the GALOSH
 * frameserver DLL (no ffmpeg/VirtualDub needed).
 *
 * Evaluates an .avs script via the AviSynth C API, reads frame 0's Y
 * plane, and prints the mean absolute deviation from the flat gray the
 * test scripts are built on — the denoised script must sit far below the
 * noisy one.  Usage: test_avs_smoke.exe script.avs
 *
 * Build: gcc -O2 -std=gnu11 test_avs_smoke.c /c/Windows/System32/AviSynth.dll
 *            -Iinclude_avs -o test_avs_smoke.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include "avisynth_c.h"

int main(int argc, char **argv)
{
  if(argc < 2) { fprintf(stderr, "usage: %s script.avs\n", argv[0]); return 2; }

  AVS_ScriptEnvironment *env = avs_create_script_environment(6);
  if(!env) { fprintf(stderr, "no script environment\n"); return 1; }

  AVS_Value arg = avs_new_value_string(argv[1]);
  AVS_Value res = avs_invoke(env, "Import", arg, NULL);
  if(avs_is_error(res))
  { fprintf(stderr, "AVS error: %s\n", avs_as_string(res)); return 1; }
  if(!avs_is_clip(res)) { fprintf(stderr, "script returned no clip\n"); return 1; }

  AVS_Clip *clip = avs_take_clip(res, env);
  avs_release_value(res);
  const AVS_VideoInfo *vi = avs_get_video_info(clip);
  AVS_VideoFrame *f = avs_get_frame(clip, 0);
  if(!f) { fprintf(stderr, "no frame\n"); return 1; }

  const unsigned char *y = avs_get_read_ptr_p(f, AVS_PLANAR_Y);
  const int pitch = avs_get_pitch_p(f, AVS_PLANAR_Y);
  double acc = 0.0;
  for(int r = 0; r < vi->height; r++)
    for(int x = 0; x < vi->width; x++)
      acc += (y[(size_t)r * pitch + x] > 128)
           ?  (y[(size_t)r * pitch + x] - 128)
           : -(y[(size_t)r * pitch + x] - 128);
  printf("MAD_vs_gray %.4f\n", acc / ((double)vi->width * vi->height));

  avs_release_video_frame(f);
  avs_release_clip(clip);
  avs_delete_script_environment(env);
  return 0;
}
