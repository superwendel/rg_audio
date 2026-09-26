#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rg_defs.h"

/* Fail compilation if any codec path calls a heap allocator. Include its
 * dependencies first so these checks target the codec, not system headers. */
#define malloc(...) RG_RGS_HEAP_ALLOCATION_IS_FORBIDDEN
#define calloc(...) RG_RGS_HEAP_ALLOCATION_IS_FORBIDDEN
#define realloc(...) RG_RGS_HEAP_ALLOCATION_IS_FORBIDDEN
#define free(...) RG_RGS_HEAP_ALLOCATION_IS_FORBIDDEN
#include "rg_rgs.h"
#undef malloc
#undef calloc
#undef realloc
#undef free

static int failures;

#define CHECK(condition) \
	do { if (!(condition)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		failures++; \
	} } while (0)

int main(void)
{
	enum { TEST_FRAMES = 5121, TEST_CHANNELS = 8 };
	const size_t values = (size_t)TEST_FRAMES * TEST_CHANNELS;
	const size_t bound = rg_rgs_encode_bound(TEST_FRAMES, TEST_CHANNELS, 44100u);
	int16_t* pcm = (int16_t*)malloc(values * sizeof(int16_t));
	int16_t* decoded = (int16_t*)malloc(values * sizeof(int16_t));
	uint8_t* encoded = (uint8_t*)malloc(bound);
	uint32_t quality;
	size_t i;
	CHECK(pcm != NULL && decoded != NULL && encoded != NULL);
	if (pcm == NULL || decoded == NULL || encoded == NULL)
	{
		free(encoded);
		free(decoded);
		free(pcm);
		return 1;
	}
	for (i = 0u; i < values; i++)
	{
		pcm[i] = (int16_t)((int32_t)((i * 7919u) & 32767u) - 16384);
	}
	for (quality = 0u; quality <= (uint32_t)RG_RGS_QUALITY_LOW; quality++)
	{
		RgRgsEncodeOptions options = {(RgRgsQuality)quality, 0u};
		RgRgsDecoder decoder;
		uint32_t frames = 0u;
		size_t size = rg_rgs_encode_s16_ex(pcm, TEST_FRAMES, TEST_CHANNELS,
		    44100u, encoded, bound, &options);
		CHECK(size != 0u);
		CHECK(rg_rgs_read_header(encoded, size, NULL));
		CHECK(rg_rgs_decode_s16(encoded, size, decoded, values, NULL) == values);
		CHECK(rg_rgs_decode_trusted_s16(encoded, size, decoded, values, NULL) == values);
		CHECK(rg_rgs_decoder_init(&decoder, encoded, size, NULL));
		CHECK(rg_rgs_decoder_next_s16(&decoder, decoded, values, &frames) == RG_RGS_DECODE_FRAME);
		CHECK(frames == 5120u);
		CHECK(rg_rgs_decoder_next_s16(&decoder, decoded, values, &frames) == RG_RGS_DECODE_FRAME);
		CHECK(frames == 1u);
		CHECK(rg_rgs_decoder_next_s16(&decoder, decoded, values, &frames) == RG_RGS_DECODE_END);
		rg_rgs_decoder_reset(&decoder);
		CHECK(!decoder.failed && decoder.decoded_frames == 0u);
	}
	memset(encoded, 0x5a, bound);
	CHECK(rg_rgs_encode_bound(TEST_FRAMES, TEST_CHANNELS, 48000u) == 0u);
	CHECK(rg_rgs_encode_s16(pcm, TEST_FRAMES, TEST_CHANNELS,
	    48000u, encoded, bound) == 0u);
	CHECK(encoded[0] == 0x5a);
	free(encoded);
	free(decoded);
	free(pcm);
	printf("rg_rgs allocation-free codec: %d failures\n", failures);
	return failures == 0 ? 0 : 1;
}
