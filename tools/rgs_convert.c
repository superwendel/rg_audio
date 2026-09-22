// rgs_convert - encode a WAV file as a stable RGS v1 stream.
//
// Usage:
//   rgs_convert input.wav output.rgs [--quality high|medium|low]
//                                     [--target-kbps N]

// SDL3 performs WAV loading, sample-format conversion, and the portable
// same-directory rename used to publish the completed output atomically.

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "../src/rg_rgs.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rgs_ascii_equal_ignore_case(const char* a, const char* b)
{
	if (a == NULL || b == NULL)
	{
		return 0;
	}
	while (*a != '\0' && *b != '\0')
	{
		unsigned char ca = (unsigned char)*a++;
		unsigned char cb = (unsigned char)*b++;
		if (ca >= (unsigned char)'A' && ca <= (unsigned char)'Z')
		{
			ca = (unsigned char)(ca + ((unsigned char)'a' - (unsigned char)'A'));
		}
		if (cb >= (unsigned char)'A' && cb <= (unsigned char)'Z')
		{
			cb = (unsigned char)(cb + ((unsigned char)'a' - (unsigned char)'A'));
		}
		if (ca != cb)
		{
			return 0;
		}
	}
	return *a == *b;
}

static int rgs_has_extension(const char* path, const char* extension)
{
	const char* dot;
	if (path == NULL)
	{
		return 0;
	}
	dot = strrchr(path, '.');
	return dot != NULL && rgs_ascii_equal_ignore_case(dot, extension);
}

static int rgs_parse_quality(const char* text, RgRgsQuality* out_quality)
{
	if (rgs_ascii_equal_ignore_case(text, "high"))
	{
		*out_quality = RG_RGS_QUALITY_HIGH;
		return 1;
	}
	if (rgs_ascii_equal_ignore_case(text, "medium"))
	{
		*out_quality = RG_RGS_QUALITY_MEDIUM;
		return 1;
	}
	if (rgs_ascii_equal_ignore_case(text, "low"))
	{
		*out_quality = RG_RGS_QUALITY_LOW;
		return 1;
	}
	return 0;
}

static int rgs_parse_u32(const char* text, uint32_t* out_value)
{
	char* end = NULL;
	unsigned long value;
	if (text == NULL || *text == '\0' || *text == '-')
	{
		return 0;
	}
	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || value > UINT32_MAX)
	{
		return 0;
	}
	*out_value = (uint32_t)value;
	return 1;
}

static int rgs_load_wav_s16(const char* path,
                            int16_t** out_pcm,
                            uint32_t* out_frames,
                            uint32_t* out_channels,
                            uint32_t* out_samplerate)
{
	SDL_AudioSpec source_spec;
	SDL_AudioSpec destination_spec;
	Uint8* source_data = NULL;
	Uint32 source_size = 0u;
	Uint8* converted_data = NULL;
	int converted_size = 0;
	size_t frame_size;
	int16_t* pcm;

	if (!SDL_LoadWAV(path, &source_spec, &source_data, &source_size))
	{
		fprintf(stderr, "SDL_LoadWAV failed for '%s': %s\n", path, SDL_GetError());
		return 0;
	}
	if (source_size > (Uint32)INT_MAX || source_spec.channels <= 0 ||
	    source_spec.channels > (int)RG_RGS_MAX_CHANNELS || source_spec.freq <= 0)
	{
		fprintf(stderr, "Unsupported WAV format in '%s'.\n", path);
		SDL_free(source_data);
		return 0;
	}

	destination_spec.format = SDL_AUDIO_S16;
	destination_spec.channels = source_spec.channels;
	destination_spec.freq = source_spec.freq;
	if (!SDL_ConvertAudioSamples(&source_spec,
	                             source_data,
	                             (int)source_size,
	                             &destination_spec,
	                             &converted_data,
	                             &converted_size))
	{
		fprintf(stderr, "SDL_ConvertAudioSamples failed for '%s': %s\n", path, SDL_GetError());
		SDL_free(source_data);
		return 0;
	}
	SDL_free(source_data);

	frame_size = (size_t)destination_spec.channels * sizeof(int16_t);
	if (converted_size <= 0 || ((size_t)converted_size % frame_size) != 0u ||
	    (size_t)converted_size / frame_size > UINT32_MAX)
	{
		fprintf(stderr, "WAV contains no usable PCM frames: '%s'.\n", path);
		SDL_free(converted_data);
		return 0;
	}

	pcm = (int16_t*)malloc((size_t)converted_size);
	if (pcm == NULL)
	{
		SDL_free(converted_data);
		return 0;
	}
	memcpy(pcm, converted_data, (size_t)converted_size);
	SDL_free(converted_data);

	*out_pcm = pcm;
	*out_frames = (uint32_t)((size_t)converted_size / frame_size);
	*out_channels = (uint32_t)destination_spec.channels;
	*out_samplerate = (uint32_t)destination_spec.freq;
	return 1;
}

static int rgs_write_atomic(const char* path, const void* data, size_t size)
{
	const size_t path_length = strlen(path);
	char* temporary_path;
	FILE* file;
	int ok;

	if (path_length > SIZE_MAX - 64u)
	{
		return 0;
	}
	temporary_path = (char*)malloc(path_length + 64u);
	if (temporary_path == NULL)
	{
		return 0;
	}
	(void)snprintf(temporary_path,
	               path_length + 64u,
	               "%s.tmp-%" PRIu64,
	               path,
	               (uint64_t)SDL_GetTicksNS());

	file = fopen(temporary_path, "wb");
	if (file == NULL)
	{
		free(temporary_path);
		return 0;
	}
	ok = fwrite(data, 1u, size, file) == size;
	if (ok)
	{
		ok = fflush(file) == 0;
	}
	if (fclose(file) != 0)
	{
		ok = 0;
	}
	if (!ok || !SDL_RenamePath(temporary_path, path))
	{
		if (ok)
		{
			fprintf(stderr, "Could not replace '%s': %s\n", path, SDL_GetError());
		}
		(void)SDL_RemovePath(temporary_path);
		free(temporary_path);
		return 0;
	}

	free(temporary_path);
	return 1;
}

static int rgs_encode_file(const char* output_path,
                           const int16_t* pcm,
                           uint32_t frames,
                           uint32_t channels,
                           uint32_t samplerate,
                           const RgRgsEncodeOptions* options,
                           RgRgsInfo* out_info,
                           size_t* out_size)
{
	const size_t bound = rg_rgs_encode_bound(frames, channels, samplerate);
	uint8_t* encoded;
	size_t written;

	if (bound == 0u)
	{
		return 0;
	}
	encoded = (uint8_t*)malloc(bound);
	if (encoded == NULL)
	{
		return 0;
	}
	written = rg_rgs_encode_s16_ex(
	    pcm, frames, channels, samplerate, encoded, bound, options);
	if (written == 0u || !rg_rgs_read_header(encoded, written, out_info) ||
	    !rgs_write_atomic(output_path, encoded, written))
	{
		free(encoded);
		return 0;
	}
	*out_size = written;
	free(encoded);
	return 1;
}

static void rgs_print_usage(const char* executable)
{
	fprintf(stderr,
	        "Usage: %s input.wav output.rgs [--quality high|medium|low] [--target-kbps N]\n",
	        executable);
}

int main(int argc, char** argv)
{
	const char* input_path;
	const char* output_path;
	int16_t* pcm = NULL;
	uint32_t frames = 0u;
	uint32_t channels = 0u;
	uint32_t samplerate = 0u;
	RgRgsInfo info = {0};
	RgRgsEncodeOptions options = rg_rgs_default_options();
	size_t encoded_size = 0u;
	int i;

	if (argc < 3)
	{
		rgs_print_usage(argv[0]);
		return 2;
	}
	input_path = argv[1];
	output_path = argv[2];
	if (!rgs_has_extension(input_path, ".wav") || !rgs_has_extension(output_path, ".rgs"))
	{
		fprintf(stderr, "Input must be .wav and output must be stable .rgs.\n");
		return 2;
	}

	for (i = 3; i < argc; ++i)
	{
		if (strcmp(argv[i], "--quality") == 0 && i + 1 < argc)
		{
			++i;
			if (!rgs_parse_quality(argv[i], &options.quality))
			{
				fprintf(stderr, "Unknown quality: '%s'.\n", argv[i]);
				return 2;
			}
		}
		else if (strcmp(argv[i], "--target-kbps") == 0 && i + 1 < argc)
		{
			++i;
			if (!rgs_parse_u32(argv[i], &options.target_kbps))
			{
				fprintf(stderr, "Invalid target bitrate: '%s'.\n", argv[i]);
				return 2;
			}
		}
		else
		{
			fprintf(stderr, "Unknown or incomplete argument: '%s'.\n", argv[i]);
			rgs_print_usage(argv[0]);
			return 2;
		}
	}

	if (!rgs_load_wav_s16(input_path, &pcm, &frames, &channels, &samplerate))
	{
		return 1;
	}
	if (!rgs_encode_file(output_path,
	                     pcm,
	                     frames,
	                     channels,
	                     samplerate,
	                     &options,
	                     &info,
	                     &encoded_size))
	{
		fprintf(stderr, "RGS encode failed for '%s'.\n", input_path);
		free(pcm);
		return 1;
	}

	printf("%s -> %s\n", input_path, output_path);
	printf("source: %u Hz, %u ch, %u frames\n", samplerate, channels, frames);
	printf("stored: %u Hz, %u ch, %u frames, %.2f MiB, %.1f kbps\n",
	       info.samplerate,
	       info.channels,
	       info.samples,
	       (double)encoded_size / (1024.0 * 1024.0),
	       (double)encoded_size * 8.0 * (double)info.samplerate /
	           ((double)info.samples * 1000.0));

	free(pcm);
	return 0;
}
