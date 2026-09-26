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
#include "rgs_tool_io.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <errno.h>
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
		return SDL_SetError("Encoder input has an unsupported size or format");
	}
	encoded = (uint8_t*)malloc(bound);
	if (encoded == NULL)
	{
		return SDL_SetError("Could not allocate the encoded output buffer");
	}
	written = rg_rgs_encode_s16_ex(
	    pcm, frames, channels, samplerate, encoded, bound, options);
	if (written == 0u || !rg_rgs_read_header(encoded, written, out_info))
	{
		free(encoded);
		return SDL_SetError("RGS encoding failed to produce a valid stream");
	}
	if (!rgs_tool_write_atomic(output_path, encoded, written))
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
	RgsWavSource source = {0};
	RgRgsInfo info = {0};
	RgRgsEncodeOptions options = rg_rgs_default_options();
	size_t encoded_size = 0u;
	int i;
	RgsPreparedAudio prepared = {0};

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

	if (!rgs_tool_load_wav(input_path, &prepared, &source))
	{
		fprintf(stderr, "Could not load WAV '%s': %s\n", input_path, SDL_GetError());
		return 1;
	}
	if (!rgs_encode_file(output_path,
	                     prepared.pcm,
	                     prepared.frames,
	                     prepared.channels,
	                     prepared.samplerate,
	                     &options,
	                     &info,
	                     &encoded_size))
	{
		fprintf(stderr, "RGS encode failed for '%s': %s\n", input_path, SDL_GetError());
		free(prepared.pcm);
		return 1;
	}

	printf("%s -> %s\n", input_path, output_path);
	printf("source: %u Hz, %u ch, %u frames\n", source.samplerate, source.channels, source.frames);
	printf("stored: %u Hz, %u ch, %u frames, %.2f MiB, %.1f kbps\n",
	       info.samplerate,
	       info.channels,
	       info.samples,
	       (double)encoded_size / (1024.0 * 1024.0),
	       (double)encoded_size * 8.0 * (double)info.samplerate /
	           ((double)info.samples * 1000.0));

	free(prepared.pcm);
	return 0;
}
