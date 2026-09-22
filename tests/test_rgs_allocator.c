#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static size_t allocation_count;
static size_t free_count;
static int fail_allocations;

static void* tracked_malloc(size_t size)
{
	allocation_count++;
	if (fail_allocations)
	{
		return NULL;
	}
	return malloc(size);
}

static void tracked_free(void* ptr)
{
	if (ptr != NULL)
	{
		free_count++;
	}
	free(ptr);
}

#define RG_RGS_MALLOC(size) tracked_malloc(size)
#define RG_RGS_FREE(ptr) tracked_free(ptr)
#include "rg_rgs.h"

static int failures;

#define CHECK(condition)                                                         \
	do                                                                           \
	{                                                                            \
		if (!(condition))                                                        \
		{                                                                        \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			failures++;                                                          \
		}                                                                        \
	} while (0)

static void fill_pcm(int16_t* pcm, uint32_t frames, uint32_t channels)
{
	uint32_t i;
	for (i = 0u; i < frames * channels; i++)
	{
		pcm[i] = (int16_t)((int32_t)((i * 7919u) & 32767u) - 16384);
	}
}

int main(void)
{
	enum
	{
		TEST_FRAMES = 1000,
		TEST_CHANNELS = 2
	};
	const uint32_t frames = TEST_FRAMES;
	const uint32_t channels = TEST_CHANNELS;
	int16_t pcm[TEST_FRAMES * TEST_CHANNELS];
	int16_t decoded[TEST_FRAMES * TEST_CHANNELS];
	size_t bound_44100;
	size_t bound_48000;
	uint8_t* encoded_44100;
	uint8_t* encoded_48000;
	size_t size_44100;
	size_t size_48000;
	RgRgsDecoder decoder;
	uint32_t decoded_frames;
	size_t before_allocations;
	size_t before_frees;

	fill_pcm(pcm, frames, channels);
	bound_44100 = rg_rgs_encode_bound(frames, channels, 44100u);
	bound_48000 = rg_rgs_encode_bound(frames, channels, 48000u);
	encoded_44100 = (uint8_t*)malloc(bound_44100);
	encoded_48000 = (uint8_t*)malloc(bound_48000);
	CHECK(encoded_44100 != NULL && encoded_48000 != NULL);
	if (encoded_44100 == NULL || encoded_48000 == NULL)
	{
		free(encoded_48000);
		free(encoded_44100);
		return 1;
	}

	allocation_count = 0u;
	free_count = 0u;
	size_44100 = rg_rgs_encode_s16(pcm, frames, channels, 44100u, encoded_44100, bound_44100);
	CHECK(size_44100 != 0u);
	CHECK(allocation_count == 0u);
	CHECK(free_count == 0u);

	size_48000 = rg_rgs_encode_s16(pcm, frames, channels, 48000u, encoded_48000, bound_48000);
	CHECK(size_48000 != 0u);
	CHECK(allocation_count == 1u);
	CHECK(free_count == 1u);

	before_allocations = allocation_count;
	before_frees = free_count;
	CHECK(rg_rgs_read_header(encoded_44100, size_44100, NULL));
	CHECK(rg_rgs_decode_s16(encoded_44100, size_44100, decoded, frames * channels, NULL) == frames * channels);
	CHECK(rg_rgs_decode_trusted_s16(encoded_44100, size_44100, decoded, frames * channels, NULL) == frames * channels);
	CHECK(rg_rgs_decoder_init(&decoder, encoded_44100, size_44100, NULL));
	CHECK(rg_rgs_decoder_next_s16(&decoder, decoded, frames * channels, &decoded_frames) == RG_RGS_DECODE_FRAME);
	CHECK(decoded_frames == frames);
	CHECK(rg_rgs_decoder_next_s16(&decoder, decoded, frames * channels, &decoded_frames) == RG_RGS_DECODE_END);
	rg_rgs_decoder_reset(&decoder);
	CHECK(allocation_count == before_allocations);
	CHECK(free_count == before_frees);

	fail_allocations = 1;
	CHECK(rg_rgs_encode_s16(pcm, frames, channels, 48000u, encoded_48000, bound_48000) == 0u);
	CHECK(allocation_count == before_allocations + 1u);
	CHECK(free_count == before_frees);
	fail_allocations = 0;

	free(encoded_48000);
	free(encoded_44100);
	printf("rg_rgs allocator: %d failures\n", failures);
	return failures == 0 ? 0 : 1;
}
