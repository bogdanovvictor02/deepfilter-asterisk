/*
 * func_deepfilter.c - AI Noise Reduction for Asterisk using DeepFilterNet
 *
 * Copyright (C) 2026
 *
 * Dialplan usage:
 *   Set(DEEPFILTER(rx)=on)          ; enable on incoming audio
 *   Set(DEEPFILTER(tx)=on)          ; enable on outgoing audio
 *   Set(DEEPFILTER(rx)=off)         ; disable
 *   Set(DEEPFILTER_ATTEN(rx)=40)    ; set attenuation limit (dB)
 *
 * Test extensions (see extensions.conf):
 *   8001/8002  - call with DeepFilter
 *   9998       - echo test with DeepFilter
 */

/*** MODULEINFO
	<depend>speexdsp</depend>
	<support_level>extended</support_level>
 ***/

#define AST_MODULE "func_deepfilter"

#include "asterisk.h"
#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/utils.h"
#include "asterisk/audiohook.h"
#include "asterisk/config.h"
#include "asterisk/logger.h"
#include "asterisk/lock.h"

#include <speex/speex_resampler.h>
#include <string.h>
#include <math.h>

/*
 * df_bridge.h provides pure C declarations for the DeepFilterNet
 * C API functions exported by libdeepfilter.so.
 * We use our own header instead of the auto-generated deep_filter.h
 * because the latter includes C++ headers (<cstdarg>).
 */
#include "df_bridge.h"

/* ================================================================
 * Constants
 * ================================================================ */

#define DEFAULT_MODEL_PATH  "/usr/share/asterisk/deepfilter/DeepFilterNet3.tar.gz"
#define DEFAULT_ATTEN_LIM   100.0f

/* DeepFilterNet3 parameters */
#define DF_HOP_SIZE         480     /* samples per DF frame (10ms at 48kHz) */
#define DF_SAMPLE_RATE      48000   /* model native sample rate */

/* Buffer sizes */
#define DF_MAX_BUF_SAMPLES  4096    /* ring buffer capacity */
#define SPEEX_RESAMPLE_QUALITY 5    /* 0=worst/fastest, 10=best/slowest */

/* Maximum supported Asterisk frame size (20ms at 48kHz) */
#define MAX_AST_FRAME_SAMPLES 960

/* ================================================================
 * Per-direction state (rx or tx independently)
 * ================================================================ */

struct df_direction_info {
	/* DeepFilterNet instance */
	DFState *df_handle;

	/* Speex resamplers for sample rate conversion */
	SpeexResamplerState *upsampler;    /* ast_rate -> 48kHz */
	SpeexResamplerState *downsampler;  /* 48kHz -> ast_rate */

	/* Input ring buffer: accumulates upsampled data before DF processing */
	float input_buf[DF_MAX_BUF_SAMPLES];
	int input_buf_pos;

	/* Output ring buffer: holds DF-processed data before downsampling */
	float output_buf[DF_MAX_BUF_SAMPLES];
	int output_buf_avail;

	/* State */
	int enabled;
	int ast_rate;       /* current Asterisk sample rate on this channel */
	float atten_lim;    /* current attenuation limit dB */
};

/* ================================================================
 * Per-channel state (holds audiohook + both directions)
 * ================================================================ */

struct df_info {
	struct ast_audiohook audiohook;  /* MUST be first field */
	int lastrate;
	struct df_direction_info *tx;
	struct df_direction_info *rx;
};

/* ================================================================
 * Direction lifecycle: create / destroy
 * ================================================================ */

static void df_direction_destroy(struct df_direction_info *ddi)
{
	if (!ddi)
		return;

	if (ddi->df_handle)
		df_free(ddi->df_handle);
	if (ddi->upsampler)
		speex_resampler_destroy(ddi->upsampler);
	if (ddi->downsampler)
		speex_resampler_destroy(ddi->downsampler);

	ast_free(ddi);
}

static struct df_direction_info *df_direction_create(int ast_rate, float atten_lim)
{
	struct df_direction_info *ddi;
	int err;

	ddi = ast_calloc(1, sizeof(*ddi));
	if (!ddi)
		return NULL;

	/* Create DeepFilterNet instance (loads model, heavy operation) */
	ddi->df_handle = df_create(DEFAULT_MODEL_PATH, atten_lim);
	if (!ddi->df_handle) {
		ast_log(LOG_ERROR, "DeepFilter: failed to create DF instance "
			"(model: %s, atten: %.1f dB)\n", DEFAULT_MODEL_PATH, atten_lim);
		ast_free(ddi);
		return NULL;
	}

	/* Verify model frame length matches our expectation */
	int hop = df_get_frame_length(ddi->df_handle);
	if (hop != DF_HOP_SIZE) {
		ast_log(LOG_WARNING, "DeepFilter: unexpected hop_size "
			"(got %d, expected %d)\n", hop, DF_HOP_SIZE);
	}

	/* Create upsample resampler: ast_rate -> 48kHz */
	ddi->upsampler = speex_resampler_init(1, ast_rate, DF_SAMPLE_RATE,
		SPEEX_RESAMPLE_QUALITY, &err);
	if (!ddi->upsampler || err != 0) {
		ast_log(LOG_ERROR, "DeepFilter: upsampler init failed "
			"(%d -> %d, err=%d)\n", ast_rate, DF_SAMPLE_RATE, err);
		df_free(ddi->df_handle);
		ast_free(ddi);
		return NULL;
	}

	/* Create downsample resampler: 48kHz -> ast_rate */
	ddi->downsampler = speex_resampler_init(1, DF_SAMPLE_RATE, ast_rate,
		SPEEX_RESAMPLE_QUALITY, &err);
	if (!ddi->downsampler || err != 0) {
		ast_log(LOG_ERROR, "DeepFilter: downsampler init failed "
			"(%d -> %d, err=%d)\n", DF_SAMPLE_RATE, ast_rate, err);
		speex_resampler_destroy(ddi->upsampler);
		df_free(ddi->df_handle);
		ast_free(ddi);
		return NULL;
	}

	ddi->ast_rate = ast_rate;
	ddi->atten_lim = atten_lim;
	ddi->enabled = 1;
	ddi->input_buf_pos = 0;
	ddi->output_buf_avail = 0;

	ast_log(LOG_NOTICE, "DeepFilter: direction created "
		"(rate=%d, atten=%.1f dB)\n", ast_rate, atten_lim);

	return ddi;
}

/*
 * Recreate resamplers when the channel sample rate changes mid-call.
 * Returns 0 on success, -1 on failure.
 */
static int df_direction_update_rate(struct df_direction_info *ddi, int new_rate)
{
	int err;

	if (ddi->upsampler)
		speex_resampler_destroy(ddi->upsampler);
	if (ddi->downsampler)
		speex_resampler_destroy(ddi->downsampler);

	ddi->upsampler = speex_resampler_init(1, new_rate, DF_SAMPLE_RATE,
		SPEEX_RESAMPLE_QUALITY, &err);
	if (!ddi->upsampler || err != 0) {
		ast_log(LOG_ERROR, "DeepFilter: upsampler reinit failed\n");
		return -1;
	}

	ddi->downsampler = speex_resampler_init(1, DF_SAMPLE_RATE, new_rate,
		SPEEX_RESAMPLE_QUALITY, &err);
	if (!ddi->downsampler || err != 0) {
		speex_resampler_destroy(ddi->upsampler);
		ddi->upsampler = NULL;
		ast_log(LOG_ERROR, "DeepFilter: downsampler reinit failed\n");
		return -1;
	}

	/* Reset buffers on rate change to avoid stale data */
	ddi->input_buf_pos = 0;
	ddi->output_buf_avail = 0;
	ddi->ast_rate = new_rate;

	ast_log(LOG_NOTICE, "DeepFilter: resamplers updated to rate %d\n", new_rate);
	return 0;
}

/* ================================================================
 * Datastore (ties df_info lifecycle to channel)
 * ================================================================ */

static void destroy_callback(void *data)
{
	struct df_info *di = data;

	ast_audiohook_destroy(&di->audiohook);
	df_direction_destroy(di->rx);
	df_direction_destroy(di->tx);
	ast_free(di);

	ast_log(LOG_NOTICE, "DeepFilter: channel state destroyed\n");
}

static const struct ast_datastore_info df_datastore = {
	.type = "deepfilter",
	.destroy = destroy_callback,
};

/* ================================================================
 * MAIN CALLBACK - called for every audio frame (~20ms)
 *
 * Pipeline per frame:
 *   1. int16 -> float32 (normalize to -1.0..1.0)
 *   2. Upsample from ast_rate to 48kHz
 *   3. Buffer and process through DeepFilterNet in DF_HOP_SIZE chunks
 *   4. Downsample from 48kHz back to ast_rate
 *   5. float32 -> int16 (write back to frame in-place)
 * ================================================================ */

static int df_callback(struct ast_audiohook *audiohook,
	struct ast_channel *chan,
	struct ast_frame *frame,
	enum ast_audiohook_direction direction)
{
	struct ast_datastore *datastore;
	struct df_info *di;
	struct df_direction_info *ddi;
	int16_t *frame_data;
	int nsamples;
	int sample_rate;

	/* Bail out if audiohook is shutting down */
	if (audiohook->status == AST_AUDIOHOOK_STATUS_DONE)
		return -1;

	/* Only process voice frames */
	if (frame->frametype != AST_FRAME_VOICE)
		return 0;

	/* Find our datastore on this channel */
	ast_channel_lock(chan);
	datastore = ast_channel_datastore_find(chan, &df_datastore, NULL);
	ast_channel_unlock(chan);

	if (!datastore)
		return -1;

	di = datastore->data;
	ddi = (direction == AST_AUDIOHOOK_DIRECTION_READ) ? di->rx : di->tx;

	/* Nothing to do if this direction is not active */
	if (!ddi || !ddi->enabled || !ddi->df_handle)
		return 0;

	frame_data = frame->data.ptr;
	nsamples = frame->samples;
	sample_rate = ast_format_get_sample_rate(frame->subclass.format);

	/* Sanity check frame size */
	if (nsamples <= 0 || nsamples > MAX_AST_FRAME_SAMPLES)
		return 0;

	/* Handle sample rate change mid-call */
	if (sample_rate != ddi->ast_rate) {
		ast_log(LOG_NOTICE, "DeepFilter: sample rate changed %d -> %d\n",
			ddi->ast_rate, sample_rate);
		if (df_direction_update_rate(ddi, sample_rate) != 0) {
			ddi->enabled = 0;
			return 0;
		}
	}

	/* ---- STEP 1: int16 -> float32 ---- */
	float tmp_in[MAX_AST_FRAME_SAMPLES];
	for (int i = 0; i < nsamples; i++) {
		tmp_in[i] = (float)frame_data[i] / 32768.0f;
	}

	/* ---- STEP 2: Upsample to 48kHz ---- */
	int ratio = DF_SAMPLE_RATE / sample_rate;
	int max_upsampled = nsamples * (ratio + 1);
	float upsampled[max_upsampled];

	spx_uint32_t in_len = (spx_uint32_t)nsamples;
	spx_uint32_t out_len = (spx_uint32_t)max_upsampled;

	speex_resampler_process_float(ddi->upsampler, 0,
		tmp_in, &in_len, upsampled, &out_len);

	/* ---- STEP 3: Buffer + DeepFilterNet processing ---- */

	/* Append upsampled data to input ring buffer */
	if (ddi->input_buf_pos + (int)out_len > DF_MAX_BUF_SAMPLES) {
		ast_log(LOG_WARNING, "DeepFilter: input buffer overflow "
			"(pos=%d, new=%u, max=%d), resetting\n",
			ddi->input_buf_pos, out_len, DF_MAX_BUF_SAMPLES);
		ddi->input_buf_pos = 0;
		ddi->output_buf_avail = 0;
		return 0;
	}

	memcpy(ddi->input_buf + ddi->input_buf_pos,
		upsampled, out_len * sizeof(float));
	ddi->input_buf_pos += (int)out_len;

	/* Process all complete DF frames (480 samples each) */
	while (ddi->input_buf_pos >= DF_HOP_SIZE) {
		float df_in[DF_HOP_SIZE];
		float df_out[DF_HOP_SIZE];

		/* Extract one hop from input buffer */
		memcpy(df_in, ddi->input_buf, DF_HOP_SIZE * sizeof(float));

		/* Shift remaining data to front */
		ddi->input_buf_pos -= DF_HOP_SIZE;
		if (ddi->input_buf_pos > 0) {
			memmove(ddi->input_buf,
				ddi->input_buf + DF_HOP_SIZE,
				ddi->input_buf_pos * sizeof(float));
		}

		/* Run DeepFilterNet inference (returns local SNR) */
		df_process_frame(ddi->df_handle, df_in, df_out);

		/* Append result to output buffer */
		if (ddi->output_buf_avail + DF_HOP_SIZE <= DF_MAX_BUF_SAMPLES) {
			memcpy(ddi->output_buf + ddi->output_buf_avail,
				df_out, DF_HOP_SIZE * sizeof(float));
			ddi->output_buf_avail += DF_HOP_SIZE;
		} else {
			ast_log(LOG_WARNING, "DeepFilter: output buffer overflow\n");
		}
	}

	/* ---- STEP 4: Downsample + float32 -> int16 ---- */
	if (ddi->output_buf_avail > 0) {
		float downsampled[MAX_AST_FRAME_SAMPLES + 64];
		spx_uint32_t ds_in = (spx_uint32_t)ddi->output_buf_avail;
		spx_uint32_t ds_out = (spx_uint32_t)nsamples;

		speex_resampler_process_float(ddi->downsampler, 0,
			ddi->output_buf, &ds_in, downsampled, &ds_out);

		/* Shift consumed data in output buffer */
		ddi->output_buf_avail -= (int)ds_in;
		if (ddi->output_buf_avail > 0) {
			memmove(ddi->output_buf,
				ddi->output_buf + ds_in,
				ddi->output_buf_avail * sizeof(float));
		}

		/* Convert float -> int16 and write back to frame (in-place) */
		int copy_len = ((int)ds_out < nsamples) ? (int)ds_out : nsamples;
		for (int i = 0; i < copy_len; i++) {
			float v = downsampled[i] * 32768.0f;
			if (v > 32767.0f)
				v = 32767.0f;
			else if (v < -32768.0f)
				v = -32768.0f;
			frame_data[i] = (int16_t)v;
		}

		/* Zero-fill any remaining samples if we got fewer than expected */
		for (int i = copy_len; i < nsamples; i++) {
			frame_data[i] = 0;
		}
	}

	return 0;
}

/* ================================================================
 * DIALPLAN FUNCTION: DEEPFILTER(rx|tx) write handler
 *
 * Called when dialplan executes:
 *   Set(DEEPFILTER(rx)=on)
 *   Set(DEEPFILTER(tx)=off)
 *   Set(DEEPFILTER_ATTEN(rx)=40)
 * ================================================================ */

static int df_write(struct ast_channel *chan, const char *cmd,
	char *data, const char *value)
{
	struct ast_datastore *datastore = NULL;
	struct df_info *di = NULL;
	struct df_direction_info **ddi_ptr = NULL;
	int is_new = 0;

	if (!chan) {
		ast_log(LOG_WARNING, "DeepFilter: no channel\n");
		return -1;
	}

	/* Validate direction argument */
	if (ast_strlen_zero(data) ||
		(strcasecmp(data, "rx") && strcasecmp(data, "tx"))) {
		ast_log(LOG_ERROR,
			"DeepFilter: invalid direction '%s' (use rx or tx)\n",
			data ? data : "(null)");
		return -1;
	}

	/* Find or create our datastore on this channel */
	ast_channel_lock(chan);
	datastore = ast_channel_datastore_find(chan, &df_datastore, NULL);
	ast_channel_unlock(chan);

	if (!datastore) {
		/* First time - create everything */
		datastore = ast_datastore_alloc(&df_datastore, NULL);
		if (!datastore)
			return -1;

		di = ast_calloc(1, sizeof(*di));
		if (!di) {
			ast_datastore_free(datastore);
			return -1;
		}

		if (ast_audiohook_init(&di->audiohook,
				AST_AUDIOHOOK_TYPE_MANIPULATE,
				"deepfilter",
				AST_AUDIOHOOK_MANIPULATE_ALL_RATES)) {
			ast_free(di);
			ast_datastore_free(datastore);
			return -1;
		}

		di->audiohook.manipulate_callback = df_callback;
		di->lastrate = 8000;  /* default, will be updated on first frame */
		is_new = 1;
	} else {
		di = datastore->data;
	}

	/* Select the direction pointer */
	ddi_ptr = (!strcasecmp(data, "rx")) ? &di->rx : &di->tx;

	if (!strcasecmp(cmd, "DEEPFILTER")) {
		if (ast_true(value)) {
			/* Enable noise reduction */
			if (!*ddi_ptr) {
				*ddi_ptr = df_direction_create(di->lastrate,
					DEFAULT_ATTEN_LIM);
				if (!*ddi_ptr) {
					ast_log(LOG_ERROR,
						"DeepFilter: failed to create %s instance\n",
						data);
					if (is_new) {
						ast_audiohook_destroy(&di->audiohook);
						ast_free(di);
						ast_datastore_free(datastore);
					}
					return -1;
				}
				ast_log(LOG_NOTICE,
					"DeepFilter: enabled on %s\n", data);
			}
			(*ddi_ptr)->enabled = 1;
		} else {
			/* Disable noise reduction */
			if (*ddi_ptr) {
				(*ddi_ptr)->enabled = 0;
				ast_log(LOG_NOTICE,
					"DeepFilter: disabled on %s\n", data);
			}
		}
	} else if (!strcasecmp(cmd, "DEEPFILTER_ATTEN")) {
		/* Set attenuation limit */
		float atten = 0;
		if (sscanf(value, "%f", &atten) == 1) {
			if (*ddi_ptr && (*ddi_ptr)->df_handle) {
				df_set_atten_lim((*ddi_ptr)->df_handle, atten);
				(*ddi_ptr)->atten_lim = atten;
				ast_log(LOG_NOTICE,
					"DeepFilter: %s attenuation set to %.1f dB\n",
					data, atten);
			} else {
				ast_log(LOG_WARNING,
					"DeepFilter: cannot set atten on %s "
					"(not enabled)\n", data);
			}
		}
	}

	/* Attach audiohook if this is a new datastore */
	if (is_new) {
		datastore->data = di;

		ast_channel_lock(chan);
		ast_channel_datastore_add(chan, datastore);
		ast_channel_unlock(chan);

		ast_audiohook_attach(chan, &di->audiohook);

		ast_log(LOG_NOTICE,
			"DeepFilter: audiohook attached to channel\n");
	}

	return 0;
}

/* ================================================================
 * DIALPLAN FUNCTION: DEEPFILTER(rx|tx) read handler
 *
 * Returns "on" or "off" for DEEPFILTER()
 * Returns attenuation value for DEEPFILTER_ATTEN()
 * ================================================================ */

static int df_read(struct ast_channel *chan, const char *cmd,
	char *data, char *buf, size_t len)
{
	struct ast_datastore *datastore;
	struct df_info *di;
	struct df_direction_info *ddi;

	if (!chan) {
		snprintf(buf, len, "off");
		return 0;
	}

	ast_channel_lock(chan);
	datastore = ast_channel_datastore_find(chan, &df_datastore, NULL);
	ast_channel_unlock(chan);

	if (!datastore) {
		snprintf(buf, len, "off");
		return 0;
	}

	di = datastore->data;
	ddi = (!strcasecmp(data, "rx")) ? di->rx : di->tx;

	if (!strcasecmp(cmd, "DEEPFILTER")) {
		snprintf(buf, len, "%s",
			(ddi && ddi->enabled) ? "on" : "off");
	} else if (!strcasecmp(cmd, "DEEPFILTER_ATTEN")) {
		snprintf(buf, len, "%.1f",
			ddi ? ddi->atten_lim : 0.0f);
	}

	return 0;
}

/* ================================================================
 * Module registration
 * ================================================================ */

static struct ast_custom_function deepfilter_function = {
	.name = "DEEPFILTER",
	.write = df_write,
	.read = df_read,
	.read_max = 22,
};

static struct ast_custom_function deepfilter_atten_function = {
	.name = "DEEPFILTER_ATTEN",
	.write = df_write,
	.read = df_read,
	.read_max = 22,
};

static int unload_module(void)
{
	ast_custom_function_unregister(&deepfilter_function);
	ast_custom_function_unregister(&deepfilter_atten_function);

	ast_log(LOG_NOTICE, "DeepFilter: module unloaded\n");
	return 0;
}

static int load_module(void)
{
	if (ast_custom_function_register(&deepfilter_function)) {
		ast_log(LOG_ERROR, "DeepFilter: failed to register DEEPFILTER()\n");
		return AST_MODULE_LOAD_DECLINE;
	}

	if (ast_custom_function_register(&deepfilter_atten_function)) {
		ast_custom_function_unregister(&deepfilter_function);
		ast_log(LOG_ERROR, "DeepFilter: failed to register DEEPFILTER_ATTEN()\n");
		return AST_MODULE_LOAD_DECLINE;
	}

	ast_log(LOG_NOTICE,
		"DeepFilter: AI noise suppression module loaded\n"
		"  Model path: %s\n"
		"  Default attenuation limit: %.0f dB\n"
		"  DF sample rate: %d Hz, hop size: %d samples\n",
		DEFAULT_MODEL_PATH, DEFAULT_ATTEN_LIM,
		DF_SAMPLE_RATE, DF_HOP_SIZE);

	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY,
	"AI Noise Reduction using DeepFilterNet");
