/*
 * df_bridge.h - Pure C declarations for DeepFilterNet C API
 *
 * These declarations match the symbols exported by libdeepfilter.so
 * built from https://github.com/Rikorose/DeepFilterNet with --features capi.
 *
 * We use this instead of the auto-generated deep_filter.h because
 * that header includes C++ constructs (<cstdarg>, extern "C").
 */

#ifndef DF_BRIDGE_H
#define DF_BRIDGE_H

#include <stdint.h>
#include <stddef.h>

/* Opaque handle to a DeepFilter instance */
typedef struct DFState DFState;

/*
 * Create a new DeepFilter instance.
 *
 * model_path: path to the ONNX .tar.gz model file
 * atten_lim:  attenuation limit in dB (0-100, higher = more aggressive)
 *
 * Returns: handle on success, NULL on failure
 */
DFState *df_create(const char *model_path, float atten_lim);

/*
 * Get the expected frame length (hop_size) in samples.
 * Typically 480 for DeepFilterNet3 at 48kHz (= 10ms).
 */
int df_get_frame_length(DFState *st);

/*
 * Set the attenuation limit in dB.
 */
void df_set_atten_lim(DFState *st, float lim_db);

/*
 * Set the post-filter beta parameter.
 * A beta of 0 disables the post filter.
 * Suitable range: 0 to 0.05.
 */
void df_set_post_filter_beta(DFState *st, float beta);

/*
 * Process a single audio frame through DeepFilterNet.
 *
 * st:     DeepFilter instance
 * input:  float[] buffer of length df_get_frame_length(), normalized -1.0..1.0
 * output: float[] buffer of same length for the result
 *
 * Returns: local SNR of the current frame
 */
float df_process_frame(DFState *st, float *input, float *output);

/*
 * Free a DeepFilter instance and all associated resources.
 */
void df_free(DFState *st);

#endif /* DF_BRIDGE_H */
