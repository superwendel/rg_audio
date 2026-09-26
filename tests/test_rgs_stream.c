#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rg_rgs.h"

static int checks;
static int failures;

#define CHECK(condition)                                                         \
	do                                                                           \
	{                                                                            \
		checks++;                                                                \
		if (!(condition))                                                        \
		{                                                                        \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			failures++;                                                          \
		}                                                                        \
	} while (0)

static void fill_pcm(int16_t* pcm, uint32_t frames, uint32_t channels)
{
	uint32_t state = 0x31415926u;
	uint32_t f;
	uint32_t c;
	for (f = 0u; f < frames; f++)
	{
		for (c = 0u; c < channels; c++)
		{
			state = state * 1103515245u + 12345u;
			pcm[(size_t)f * channels + c] =
			    (int16_t)(((int32_t)(state >> 17u) - 16384) / 2 + (int32_t)((f * (c + 3u)) % 997u));
		}
	}
}

static uint8_t* encode(const int16_t* pcm,
                       uint32_t frames,
                       uint32_t channels,
                       size_t* out_size)
{
	size_t bound = rg_rgs_encode_bound(frames, channels, 44100u);
	uint8_t* data = (uint8_t*)malloc(bound);
	if (data != NULL)
	{
		*out_size = rg_rgs_encode_s16(pcm, frames, channels, 44100u, data, bound);
		if (*out_size == 0u)
		{
			free(data);
			data = NULL;
		}
	}
	return data;
}

static void test_sequence_and_reset(void)
{
	const uint32_t total_frames = RG_RGS_MAX_FRAME_SAMPLES * 2u + 93u;
	const uint32_t channels = 3u;
	const size_t total_values = (size_t)total_frames * channels;
	int16_t* pcm = (int16_t*)malloc(total_values * sizeof(int16_t));
	int16_t* whole = (int16_t*)malloc(total_values * sizeof(int16_t));
	int16_t* streamed = (int16_t*)malloc(total_values * sizeof(int16_t));
	int16_t frame[RG_RGS_MAX_FRAME_SAMPLES * 3u];
	uint8_t* encoded;
	size_t encoded_size = 0u;
	RgRgsDecoder decoder;
	RgRgsInfo info;
	RgRgsDecodeStatus status;
	uint32_t frames = 0u;
	size_t values_written = 0u;
	size_t old_offset;
	uint32_t old_decoded;

	CHECK(pcm != NULL && whole != NULL && streamed != NULL);
	if (pcm == NULL || whole == NULL || streamed == NULL)
	{
		free(streamed);
		free(whole);
		free(pcm);
		return;
	}
	fill_pcm(pcm, total_frames, channels);
	encoded = encode(pcm, total_frames, channels, &encoded_size);
	CHECK(encoded != NULL);
	if (encoded == NULL)
	{
		free(streamed);
		free(whole);
		free(pcm);
		return;
	}
	CHECK(rg_rgs_decode_s16(encoded, encoded_size, whole, total_values, NULL) == total_values);
	CHECK(rg_rgs_decoder_init(&decoder, encoded, encoded_size, &info));
	CHECK(info.samples == total_frames && info.channels == channels && info.samplerate == 44100u);

	old_offset = decoder.offset;
	old_decoded = decoder.decoded_frames;
	status = rg_rgs_decoder_next_s16(&decoder, frame, RG_RGS_MAX_FRAME_SAMPLES * channels - 1u, &frames);
	CHECK(status == RG_RGS_DECODE_OUTPUT_TOO_SMALL);
	CHECK(frames == RG_RGS_MAX_FRAME_SAMPLES);
	CHECK(decoder.offset == old_offset && decoder.decoded_frames == old_decoded);

	for (;;)
	{
		frames = 999u;
		status = rg_rgs_decoder_next_s16(&decoder, frame, sizeof(frame) / sizeof(frame[0]), &frames);
		if (status == RG_RGS_DECODE_END)
		{
			CHECK(frames == 0u);
			break;
		}
		CHECK(status == RG_RGS_DECODE_FRAME);
		if (status != RG_RGS_DECODE_FRAME)
		{
			break;
		}
		CHECK(frames == (values_written < RG_RGS_MAX_FRAME_SAMPLES * 2u * channels ? RG_RGS_MAX_FRAME_SAMPLES : 93u));
		memcpy(streamed + values_written, frame, (size_t)frames * channels * sizeof(int16_t));
		values_written += (size_t)frames * channels;
	}
	CHECK(values_written == total_values);
	CHECK(memcmp(streamed, whole, total_values * sizeof(int16_t)) == 0);
	CHECK(rg_rgs_decoder_next_s16(&decoder, frame, sizeof(frame) / sizeof(frame[0]), &frames) == RG_RGS_DECODE_END);

	rg_rgs_decoder_reset(&decoder);
	CHECK(decoder.offset == RG_RGS_HEADER_SIZE);
	CHECK(decoder.decoded_frames == 0u && !decoder.failed);
	CHECK(rg_rgs_decoder_next_s16(&decoder, frame, sizeof(frame) / sizeof(frame[0]), &frames) == RG_RGS_DECODE_FRAME);
	CHECK(frames == RG_RGS_MAX_FRAME_SAMPLES);
	CHECK(memcmp(frame, whole, (size_t)frames * channels * sizeof(int16_t)) == 0);

	free(encoded);
	free(streamed);
	free(whole);
	free(pcm);
}

static void test_invalid_poison_and_trailing(void)
{
	const uint32_t total_frames = RG_RGS_MAX_FRAME_SAMPLES + 7u;
	int16_t* pcm = (int16_t*)malloc((size_t)total_frames * sizeof(int16_t));
	int16_t frame[RG_RGS_MAX_FRAME_SAMPLES];
	uint8_t* encoded;
	uint8_t* broken;
	uint8_t* trailing;
	size_t encoded_size = 0u;
	uint32_t first_size;
	uint32_t out_frames = 0u;
	RgRgsDecoder decoder;

	CHECK(pcm != NULL);
	if (pcm == NULL)
	{
		return;
	}
	fill_pcm(pcm, total_frames, 1u);
	encoded = encode(pcm, total_frames, 1u, &encoded_size);
	CHECK(encoded != NULL);
	if (encoded == NULL)
	{
		free(pcm);
		return;
	}
	broken = (uint8_t*)malloc(encoded_size);
	trailing = (uint8_t*)malloc(encoded_size + 1u);
	CHECK(broken != NULL && trailing != NULL);
	if (broken == NULL || trailing == NULL)
	{
		free(trailing);
		free(broken);
		free(encoded);
		free(pcm);
		return;
	}

	first_size = (uint32_t)encoded[RG_RGS_HEADER_SIZE + 6u] |
	             ((uint32_t)encoded[RG_RGS_HEADER_SIZE + 7u] << 8u);
	CHECK(!rg_rgs_decoder_init(&decoder, encoded,
	    RG_RGS_HEADER_SIZE + RG_RGS_FRAME_HEADER_SIZE, NULL));
	CHECK(decoder.failed);
	CHECK(rg_rgs_decoder_next_s16(&decoder, frame,
	    RG_RGS_MAX_FRAME_SAMPLES, &out_frames) == RG_RGS_DECODE_INVALID);
	CHECK(out_frames == 0u);
	CHECK(!rg_rgs_decoder_init(&decoder, encoded,
	    RG_RGS_HEADER_SIZE + first_size - 1u, NULL));
	/* Initialization proves the first frame, not the complete file. */
	CHECK(rg_rgs_decoder_init(&decoder, encoded,
	    RG_RGS_HEADER_SIZE + first_size, NULL));
	CHECK(rg_rgs_decoder_next_s16(&decoder, frame,
	    RG_RGS_MAX_FRAME_SAMPLES, &out_frames) == RG_RGS_DECODE_FRAME);
	CHECK(rg_rgs_decoder_next_s16(&decoder, frame,
	    RG_RGS_MAX_FRAME_SAMPLES, &out_frames) == RG_RGS_DECODE_INVALID);
	memcpy(broken, encoded, encoded_size);
	broken[RG_RGS_HEADER_SIZE + first_size + 6u] = 1u;
	broken[RG_RGS_HEADER_SIZE + first_size + 7u] = 0u;
	CHECK(rg_rgs_decoder_init(&decoder, broken, encoded_size, NULL));
	CHECK(rg_rgs_decoder_next_s16(&decoder, frame, RG_RGS_MAX_FRAME_SAMPLES, &out_frames) == RG_RGS_DECODE_FRAME);
	CHECK(rg_rgs_decoder_next_s16(&decoder, frame, RG_RGS_MAX_FRAME_SAMPLES, &out_frames) == RG_RGS_DECODE_INVALID);
	CHECK(decoder.failed);
	CHECK(rg_rgs_decoder_next_s16(&decoder, frame, RG_RGS_MAX_FRAME_SAMPLES, &out_frames) == RG_RGS_DECODE_INVALID);
	rg_rgs_decoder_reset(&decoder);
	CHECK(!decoder.failed);
	CHECK(rg_rgs_decoder_next_s16(&decoder, frame, RG_RGS_MAX_FRAME_SAMPLES, &out_frames) == RG_RGS_DECODE_FRAME);
	CHECK(rg_rgs_decoder_next_s16(&decoder, frame, RG_RGS_MAX_FRAME_SAMPLES, &out_frames) == RG_RGS_DECODE_INVALID);

	memcpy(trailing, encoded, encoded_size);
	trailing[encoded_size] = 0x42u;
	CHECK(rg_rgs_decoder_init(&decoder, trailing, encoded_size + 1u, NULL));
	CHECK(rg_rgs_decoder_next_s16(&decoder, frame, RG_RGS_MAX_FRAME_SAMPLES, &out_frames) == RG_RGS_DECODE_FRAME);
	CHECK(rg_rgs_decoder_next_s16(&decoder, frame, RG_RGS_MAX_FRAME_SAMPLES, &out_frames) == RG_RGS_DECODE_FRAME);
	CHECK(rg_rgs_decoder_next_s16(&decoder, frame, RG_RGS_MAX_FRAME_SAMPLES, &out_frames) == RG_RGS_DECODE_INVALID);
	CHECK(rg_rgs_decoder_next_s16(&decoder, frame, RG_RGS_MAX_FRAME_SAMPLES, &out_frames) == RG_RGS_DECODE_INVALID);

	CHECK(rg_rgs_decoder_init(&decoder, encoded, encoded_size, NULL));
	CHECK(rg_rgs_decoder_next_s16(&decoder, NULL, RG_RGS_MAX_FRAME_SAMPLES, &out_frames) == RG_RGS_DECODE_INVALID);
	CHECK(rg_rgs_decoder_next_s16(&decoder, frame, RG_RGS_MAX_FRAME_SAMPLES, &out_frames) == RG_RGS_DECODE_INVALID);
	CHECK(rg_rgs_decoder_init(&decoder, encoded, encoded_size, NULL));
	CHECK(rg_rgs_decoder_next_s16(&decoder, frame, RG_RGS_MAX_FRAME_SAMPLES, NULL) == RG_RGS_DECODE_INVALID);

	free(trailing);
	free(broken);
	free(encoded);
	free(pcm);
}

int main(void)
{
	test_sequence_and_reset();
	test_invalid_poison_and_trailing();
	printf("rg_rgs stream: %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
