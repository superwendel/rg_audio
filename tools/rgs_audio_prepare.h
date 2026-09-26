#ifndef RGS_AUDIO_PREPARE_H
#define RGS_AUDIO_PREPARE_H

/* Offline PCM preparation shared by the converter, player, and benchmarks.
 * The runtime codec never includes or links libsoxr. Output is owned by the
 * caller and released with free(); failures leave the output empty. */
#include "rg_rgs.h"
#include <soxr.h>
#include <stdlib.h>
#include <string.h>

typedef struct RgsPreparedAudio
{
	int16_t* pcm;
	uint32_t frames;
	uint32_t channels;
	uint32_t samplerate;
} RgsPreparedAudio;

/* Return NULL on success, otherwise a static diagnostic string. Rates at or
 * below 44100 Hz are copied exactly. Higher rates use single-threaded, linear
 * phase VHQ resampling with deterministic PCM16 quantization and no dither.
 * The frame count is what soxr actually produces after flushing, not an
 * independently rounded duration. Do not pass an output with live storage. */
static inline const char* rgs_audio_prepare_s16(const int16_t* src,
                                               uint32_t frames,
                                               uint32_t channels,
                                               uint32_t samplerate,
                                               RgsPreparedAudio* out)
{
	uint64_t capacity64;
	size_t capacity;
	size_t input_done = 0u;
	size_t output_done = 0u;
	int16_t* pcm;
	soxr_t resampler;
	soxr_error_t error = NULL;
	soxr_io_spec_t io;
	soxr_quality_spec_t quality;
	soxr_runtime_spec_t runtime;
	const char* failure = NULL;
	if (out == NULL)
		return "Missing prepared-audio destination";
	memset(out, 0, sizeof(*out));
	if (src == NULL || frames == 0u || channels == 0u ||
	    channels > RG_RGS_MAX_CHANNELS || samplerate == 0u ||
	    samplerate > 16777215u ||
	    (uint64_t)frames * channels > SIZE_MAX / sizeof(int16_t))
		return "Invalid PCM description";
	if (samplerate <= RG_RGS_MAX_STORED_SAMPLERATE)
	{
		const size_t bytes = (size_t)frames * channels * sizeof(int16_t);
		pcm = (int16_t*)malloc(bytes);
		if (pcm == NULL)
			return "Could not allocate prepared PCM";
		memcpy(pcm, src, bytes);
		out->pcm = pcm;
		out->frames = frames;
		out->channels = channels;
		out->samplerate = samplerate;
		return NULL;
	}

	/* Extra capacity allows an explicit final flush/probe even when the
	 * rounded timeline fills the estimated output exactly. */
	capacity64 = ((uint64_t)frames * RG_RGS_MAX_STORED_SAMPLERATE + samplerate - 1u) /
	             samplerate + 32u;
	if (capacity64 > UINT32_MAX)
		capacity64 = UINT32_MAX;
	if (capacity64 * channels > SIZE_MAX / sizeof(int16_t))
		return "Prepared PCM exceeds addressable storage";
	capacity = (size_t)capacity64;
	pcm = (int16_t*)malloc(capacity * channels * sizeof(int16_t));
	if (pcm == NULL)
		return "Could not allocate prepared PCM";
	io = soxr_io_spec(SOXR_INT16_I, SOXR_INT16_I);
	io.flags |= SOXR_NO_DITHER;
	quality = soxr_quality_spec(SOXR_VHQ, 0u);
	runtime = soxr_runtime_spec(1u);
	resampler = soxr_create((double)samplerate,
	                        (double)RG_RGS_MAX_STORED_SAMPLERATE,
	                        channels, &error, &io, &quality, &runtime);
	if (resampler == NULL || error != NULL)
	{
		if (resampler != NULL) soxr_delete(resampler);
		free(pcm);
		return "Could not initialize VHQ resampler";
	}
	for (;;)
	{
		size_t used = 0u;
		size_t made = 0u;
		const int flushing = input_done == frames;
		if (output_done == capacity)
		{
			uint64_t grown = (uint64_t)capacity * 2u;
			int16_t* replacement;
			if (grown > UINT32_MAX) grown = UINT32_MAX;
			if (grown <= capacity || grown * channels > SIZE_MAX / sizeof(int16_t))
			{
				failure = "Prepared PCM exceeds addressable storage";
				break;
			}
			replacement = (int16_t*)realloc(pcm, (size_t)grown * channels * sizeof(int16_t));
			if (replacement == NULL)
			{
				failure = "Could not grow prepared PCM";
				break;
			}
			pcm = replacement;
			capacity = (size_t)grown;
		}
		error = soxr_process(resampler,
		                     flushing ? NULL : src + input_done * channels,
		                     flushing ? 0u : (size_t)frames - input_done, &used,
		                     pcm + output_done * channels, capacity - output_done, &made);
		if (error != NULL)
		{
			failure = "VHQ resampling failed";
			break;
		}
		input_done += used;
		output_done += made;
		if (flushing && made == 0u) break;
		if (!flushing && used == 0u && made == 0u)
		{
			failure = "VHQ resampler made no progress";
			break;
		}
	}
	soxr_delete(resampler);
	if (failure == NULL && output_done == 0u)
		failure = "Source is too short to produce a stored PCM frame";
	if (failure != NULL)
	{
		free(pcm);
		return failure;
	}
	out->pcm = pcm;
	out->frames = (uint32_t)output_done;
	out->channels = channels;
	out->samplerate = RG_RGS_MAX_STORED_SAMPLERATE;
	return NULL;
}

#endif
