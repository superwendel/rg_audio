#ifndef RGS_TOOL_IO_H
#define RGS_TOOL_IO_H

/* Offline file handling shared by the converter and A/B player. */
#include "rgs_audio_prepare.h"
#include <SDL3/SDL.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>

typedef struct RgsWavSource
{
	uint32_t frames;
	uint32_t channels;
	uint32_t samplerate;
} RgsWavSource;

/* Load and prepare PCM for encoding. Release out->pcm with free(). Outputs
 * are empty on failure; SDL_GetError supplies the diagnostic. The optional
 * source describes the WAV before resampling. Do not pass live output storage. */
static inline int rgs_tool_load_wav(const char* path,
                                  RgsPreparedAudio* out,
                                  RgsWavSource* source)
{
	SDL_AudioSpec source_spec;
	SDL_AudioSpec destination_spec;
	Uint8* source_data = NULL;
	Uint32 source_size = 0u;
	Uint8* converted = NULL;
	int converted_size = 0;
	size_t frame_size;
	uint32_t frames;
	const char* error;
	if (source != NULL)
		memset(source, 0, sizeof(*source));
	if (out == NULL)
		return SDL_SetError("Missing prepared-audio destination");
	memset(out, 0, sizeof(*out));
	if (!SDL_LoadWAV(path, &source_spec, &source_data, &source_size))
		return 0;
	if (source_size > (Uint32)INT_MAX || source_spec.channels <= 0 ||
	    source_spec.channels > (int)RG_RGS_MAX_CHANNELS || source_spec.freq <= 0)
	{
		SDL_free(source_data);
		return SDL_SetError("Unsupported WAV channel count, sample rate, or size: %s", path);
	}
	destination_spec.format = SDL_AUDIO_S16;
	destination_spec.channels = source_spec.channels;
	destination_spec.freq = source_spec.freq;
	if (!SDL_ConvertAudioSamples(&source_spec, source_data, (int)source_size,
	                             &destination_spec, &converted, &converted_size))
	{
		SDL_free(source_data);
		return 0;
	}
	SDL_free(source_data);
	frame_size = (size_t)source_spec.channels * sizeof(int16_t);
	if (converted_size <= 0 || (size_t)converted_size % frame_size != 0u ||
	    (size_t)converted_size / frame_size > UINT32_MAX)
	{
		SDL_free(converted);
		return SDL_SetError("Converted WAV has an invalid sample count: %s", path);
	}
	frames = (uint32_t)((size_t)converted_size / frame_size);
	/* Preparation reads SDL's buffer directly, avoiding an intermediate PCM copy. */
	error = rgs_audio_prepare_s16((const int16_t*)converted, frames,
	                              (uint32_t)source_spec.channels,
	                              (uint32_t)source_spec.freq, out);
	SDL_free(converted);
	if (error != NULL)
		return SDL_SetError("Could not prepare WAV '%s': %s", path, error);
	if (source != NULL)
	{
		source->frames = frames;
		source->channels = (uint32_t)source_spec.channels;
		source->samplerate = (uint32_t)source_spec.freq;
	}
	return 1;
}

/* Publish a completed file by renaming a same-directory temporary file.
 * Failure leaves the destination untouched and reports through SDL_GetError. */
static inline int rgs_tool_write_atomic(const char* path, const void* data, size_t size)
{
	const size_t path_length = strlen(path);
	char* temporary_path;
	SDL_IOStream* file;
	char error[1024] = {0};
	int written;
	int ok;
	if (path_length > SIZE_MAX - 64u)
		return SDL_SetError("Output path is too long");
	temporary_path = (char*)malloc(path_length + 64u);
	if (temporary_path == NULL)
		return SDL_SetError("Could not allocate temporary output path");
	written = snprintf(temporary_path, path_length + 64u, "%s.tmp-%" PRIu64,
	                   path, (uint64_t)SDL_GetTicksNS());
	if (written < 0 || (size_t)written >= path_length + 64u)
	{
		free(temporary_path);
		return SDL_SetError("Temporary output path is too long");
	}
	file = SDL_IOFromFile(temporary_path, "wb");
	if (file == NULL)
	{
		free(temporary_path);
		return 0;
	}
	ok = SDL_WriteIO(file, data, size) == size;
	if (ok)
		ok = SDL_FlushIO(file) ? 1 : 0;
	if (!ok)
		SDL_strlcpy(error, SDL_GetError(), sizeof(error));
	/* Close even after a write failure; preserve the first failure diagnostic. */
	if (!SDL_CloseIO(file) && ok)
	{
		SDL_strlcpy(error, SDL_GetError(), sizeof(error));
		ok = 0;
	}
	if (ok && !SDL_RenamePath(temporary_path, path))
	{
		SDL_strlcpy(error, SDL_GetError(), sizeof(error));
		ok = 0;
	}
	if (!ok)
	{
		(void)SDL_RemovePath(temporary_path);
		SDL_SetError("%s", error);
	}
	free(temporary_path);
	return ok;
}

#endif
