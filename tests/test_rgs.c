#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rg_rgs.h"

static int tests_run;
static int tests_failed;

#define CHECK(condition)                                                         \
	do                                                                           \
	{                                                                            \
		tests_run++;                                                             \
		if (!(condition))                                                        \
		{                                                                        \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			tests_failed++;                                                      \
		}                                                                        \
	} while (0)

static uint32_t test_prng(uint32_t* state)
{
	*state = *state * 1664525u + 1013904223u;
	return *state;
}

static void fill_pcm(int16_t* pcm, uint32_t frames, uint32_t channels, uint32_t seed)
{
	uint32_t state = seed;
	int32_t smooth[RG_RGS_MAX_CHANNELS] = {0};
	uint32_t f;
	uint32_t c;
	for (f = 0u; f < frames; f++)
	{
		for (c = 0u; c < channels; c++)
		{
			int32_t noise = (int32_t)(test_prng(&state) >> 17u) - 16384;
			int32_t pulse = ((f + c * 17u) % 211u) == 0u ? 12000 : 0;
			smooth[c] = (smooth[c] * 7 + noise + pulse) / 8;
			pcm[(size_t)f * channels + c] = (int16_t)smooth[c];
		}
	}
}

static int encode_buffer(const int16_t* pcm,
                         uint32_t frames,
                         uint32_t channels,
                         uint32_t rate,
                         const RgRgsEncodeOptions* options,
                         uint8_t** out_data,
                         size_t* out_size)
{
	size_t bound = rg_rgs_encode_bound(frames, channels, rate);
	uint8_t* data;
	size_t size;
	if (bound == 0u)
	{
		return 0;
	}
	data = (uint8_t*)malloc(bound);
	if (data == NULL)
	{
		return 0;
	}
	size = options == NULL ? rg_rgs_encode_s16(pcm, frames, channels, rate, data, bound) : rg_rgs_encode_s16_ex(pcm, frames, channels, rate, data, bound, options);
	if (size == 0u || size > bound)
	{
		free(data);
		return 0;
	}
	*out_data = data;
	*out_size = size;
	return 1;
}

static void test_constants_and_defaults(void)
{
	RgRgsEncodeOptions options = rg_rgs_default_options();
	volatile uint32_t version = RG_RGS_VERSION;
	volatile uint32_t max_channels = RG_RGS_MAX_CHANNELS;
	volatile uint32_t max_frame_samples = RG_RGS_MAX_FRAME_SAMPLES;
	volatile uint32_t max_stored_samplerate = RG_RGS_MAX_STORED_SAMPLERATE;
	CHECK(version == 1u);
	CHECK(max_channels == 8u);
	CHECK(max_frame_samples == 5120u);
	CHECK(max_stored_samplerate == 44100u);
	CHECK(options.quality == RG_RGS_QUALITY_MEDIUM);
	CHECK(options.target_kbps == 0u);
}

static void test_invalid_descriptors(void)
{
	int16_t pcm[8] = {0};
	uint8_t dst[128];
	RgRgsEncodeOptions options = rg_rgs_default_options();
	memset(dst, 0x5a, sizeof(dst));
	CHECK(rg_rgs_encode_bound(0u, 1u, 44100u) == 0u);
	CHECK(rg_rgs_encode_bound(1u, 0u, 44100u) == 0u);
	CHECK(rg_rgs_encode_bound(1u, 9u, 44100u) == 0u);
	CHECK(rg_rgs_encode_bound(1u, 1u, 0u) == 0u);
	CHECK(rg_rgs_encode_bound(1u, 1u, RG_RGS_MAX_SAMPLERATE + 1u) == 0u);
	CHECK(rg_rgs_encode_s16(NULL, 1u, 1u, 44100u, dst, sizeof(dst)) == 0u);
	CHECK(rg_rgs_encode_s16(pcm, 1u, 1u, 44100u, NULL, sizeof(dst)) == 0u);
	options.quality = (RgRgsQuality)-1;
	CHECK(rg_rgs_encode_s16_ex(pcm, 1u, 1u, 44100u, dst, sizeof(dst), &options) == 0u);
	options.quality = (RgRgsQuality)3;
	CHECK(rg_rgs_encode_s16_ex(pcm, 1u, 1u, 44100u, dst, sizeof(dst), &options) == 0u);
	CHECK(dst[0] == 0x5a);
}

static void test_v13_lineage_golden(void)
{
	/*
	 * Generated independently with rg-core-lab commit 4b3e28e, stable v13,
	 * for pcm[i] = i * 997 - 9000. Public v1 changes only file byte 8.
	 */
	static const uint8_t expected[] = {
	    0x72, 0x67, 0x73, 0x21, 0x14, 0x00, 0x00, 0x00, 0x01, 0x07, 0x00, 0x00,
	    0x61, 0x44, 0xac, 0x00, 0x14, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0x00, 0x40,
	    0xbe, 0x51, 0x41, 0x41, 0x10, 0x01};
	int16_t pcm[20];
	uint8_t encoded[512];
	size_t encoded_size;
	uint32_t i;
	for (i = 0u; i < 20u; i++)
	{
		pcm[i] = (int16_t)((int32_t)i * 997 - 9000);
	}
	encoded_size = rg_rgs_encode_s16(pcm, 20u, 1u, 44100u, encoded, sizeof(encoded));
	CHECK(encoded_size == sizeof(expected));
	CHECK(encoded_size == 0u || memcmp(encoded, expected, sizeof(expected)) == 0);
	if (encoded_size != 0u)
	{
		encoded[encoded_size - 1u] |= 0xf0u;
		CHECK(rg_rgs_decode_s16(encoded, encoded_size, pcm, 20u, NULL) == 0u);
	}
}

static void test_roundtrip_case(uint32_t frames, uint32_t channels, uint32_t rate)
{
	size_t values = (size_t)frames * channels;
	int16_t* pcm = (int16_t*)malloc(values * sizeof(int16_t));
	uint8_t* encoded = NULL;
	size_t encoded_size = 0u;
	RgRgsInfo info;
	uint32_t stored_frames;
	int16_t* checked;
	int16_t* trusted;
	size_t decoded_values;
	size_t i;
	uint64_t absolute_error = 0u;

	CHECK(pcm != NULL);
	if (pcm == NULL)
	{
		return;
	}
	fill_pcm(pcm, frames, channels, frames ^ (channels << 16u) ^ rate);
	CHECK(encode_buffer(pcm, frames, channels, rate, NULL, &encoded, &encoded_size));
	if (encoded == NULL)
	{
		free(pcm);
		return;
	}

	CHECK(encoded[0] == 'r' && encoded[1] == 'g' && encoded[2] == 's' && encoded[3] == '!');
	CHECK(encoded[8] == 1u);
	CHECK(encoded[9] == 7u);
	CHECK(encoded[10] == 0u && encoded[11] == 0u);
	CHECK(rg_rgs_read_header(encoded, encoded_size, &info));
	stored_frames = rate > RG_RGS_MAX_STORED_SAMPLERATE ? (uint32_t)(((uint64_t)frames * RG_RGS_MAX_STORED_SAMPLERATE + rate - 1u) / rate) : frames;
	CHECK(info.channels == channels);
	CHECK(info.samplerate == (rate > RG_RGS_MAX_STORED_SAMPLERATE ? RG_RGS_MAX_STORED_SAMPLERATE : rate));
	CHECK(info.samples == stored_frames);

	decoded_values = (size_t)stored_frames * channels;
	checked = (int16_t*)malloc(decoded_values * sizeof(int16_t));
	trusted = (int16_t*)malloc(decoded_values * sizeof(int16_t));
	CHECK(checked != NULL && trusted != NULL);
	if (checked != NULL && trusted != NULL)
	{
		CHECK(rg_rgs_decode_s16(encoded, encoded_size, checked, decoded_values, NULL) == decoded_values);
		CHECK(rg_rgs_decode_trusted_s16(encoded, encoded_size, trusted, decoded_values, NULL) == decoded_values);
		CHECK(memcmp(checked, trusted, decoded_values * sizeof(int16_t)) == 0);
		if (rate <= RG_RGS_MAX_STORED_SAMPLERATE)
		{
			for (i = 0u; i < decoded_values; i++)
			{
				int32_t error = (int32_t)pcm[i] - checked[i];
				absolute_error += (uint64_t)(error < 0 ? -error : error);
			}
			CHECK(absolute_error / decoded_values < 5000u);
		}
	}
	free(trusted);
	free(checked);
	free(encoded);
	free(pcm);
}

static void test_roundtrips(void)
{
	static const uint32_t boundaries[] = {1u, 19u, 20u, 21u, 5119u, 5120u, 5121u};
	size_t i;
	uint32_t channels;
	for (i = 0u; i < sizeof(boundaries) / sizeof(boundaries[0]); i++)
	{
		test_roundtrip_case(boundaries[i], 1u, 44100u);
	}
	for (channels = 1u; channels <= RG_RGS_MAX_CHANNELS; channels++)
	{
		test_roundtrip_case(73u, channels, 22050u);
	}
	test_roundtrip_case(137u, 2u, 48000u);
	test_roundtrip_case(79u, 3u, 8000u);
}

static int encoded_equal(const int16_t* pcm,
                         uint32_t frames,
                         const RgRgsEncodeOptions* a,
                         const RgRgsEncodeOptions* b)
{
	uint8_t* a_data = NULL;
	uint8_t* b_data = NULL;
	size_t a_size = 0u;
	size_t b_size = 0u;
	int equal = 0;
	if (encode_buffer(pcm, frames, 2u, 44100u, a, &a_data, &a_size) &&
	    encode_buffer(pcm, frames, 2u, 44100u, b, &b_data, &b_size))
	{
		equal = a_size == b_size && memcmp(a_data, b_data, a_size) == 0;
	}
	free(a_data);
	free(b_data);
	return equal;
}

static void test_quality_and_bitrate_hint(void)
{
	int16_t pcm[400u * 2u];
	RgRgsEncodeOptions high = {RG_RGS_QUALITY_HIGH, 0u};
	RgRgsEncodeOptions medium = {RG_RGS_QUALITY_MEDIUM, 0u};
	RgRgsEncodeOptions low = {RG_RGS_QUALITY_LOW, 0u};
	RgRgsEncodeOptions hinted;
	fill_pcm(pcm, 400u, 2u, 0x12345678u);

	hinted = high;
	hinted.target_kbps = 225u;
	CHECK(encoded_equal(pcm, 400u, &high, &hinted));
	hinted = high;
	hinted.target_kbps = 224u;
	CHECK(encoded_equal(pcm, 400u, &medium, &hinted));
	hinted = high;
	hinted.target_kbps = 160u;
	CHECK(encoded_equal(pcm, 400u, &low, &hinted));
	hinted = medium;
	hinted.target_kbps = 1u;
	CHECK(encoded_equal(pcm, 400u, &low, &hinted));
	hinted = medium;
	hinted.target_kbps = 224u;
	CHECK(encoded_equal(pcm, 400u, &medium, &hinted));
	hinted = low;
	hinted.target_kbps = 1000u;
	CHECK(encoded_equal(pcm, 400u, &low, &hinted));
}

static void test_rejections(void)
{
	int16_t pcm[5300u * 2u];
	uint8_t* encoded = NULL;
	size_t encoded_size = 0u;
	int16_t* decoded;
	uint8_t* copy;
	uint32_t version;
	size_t cut;
	fill_pcm(pcm, 5300u, 2u, 0xabcdef01u);
	CHECK(encode_buffer(pcm, 5300u, 2u, 44100u, NULL, &encoded, &encoded_size));
	if (encoded == NULL)
	{
		return;
	}
	decoded = (int16_t*)malloc(5300u * 2u * sizeof(int16_t));
	copy = (uint8_t*)malloc(encoded_size + 1u);
	CHECK(decoded != NULL && copy != NULL);
	if (decoded == NULL || copy == NULL)
	{
		free(copy);
		free(decoded);
		free(encoded);
		return;
	}

	for (cut = 0u; cut < RG_RGS_HEADER_SIZE + RG_RGS_FRAME_HEADER_SIZE; cut++)
	{
		CHECK(!rg_rgs_read_header(encoded, cut, NULL));
		CHECK(rg_rgs_decode_s16(encoded, cut, decoded, 5300u * 2u, NULL) == 0u);
	}
	CHECK(rg_rgs_decode_s16(encoded, encoded_size - 1u, decoded, 5300u * 2u, NULL) == 0u);
	memcpy(copy, encoded, encoded_size);
	copy[encoded_size] = 0xa5u;
	CHECK(rg_rgs_decode_s16(copy, encoded_size + 1u, decoded, 5300u * 2u, NULL) == 0u);
	CHECK(rg_rgs_decode_s16(encoded, encoded_size, decoded, 5300u * 2u - 1u, NULL) == 0u);

	for (version = 0u; version <= 255u; version++)
	{
		if (version == RG_RGS_VERSION)
		{
			continue;
		}
		memcpy(copy, encoded, encoded_size);
		copy[8] = (uint8_t)version;
		CHECK(!rg_rgs_read_header(copy, encoded_size, NULL));
	}

	memcpy(copy, encoded, encoded_size);
	copy[9] ^= 1u;
	CHECK(!rg_rgs_read_header(copy, encoded_size, NULL));
	memcpy(copy, encoded, encoded_size);
	copy[10] = 1u;
	CHECK(!rg_rgs_read_header(copy, encoded_size, NULL));
	memcpy(copy, encoded, encoded_size);
	copy[11] = 1u;
	CHECK(!rg_rgs_read_header(copy, encoded_size, NULL));
	memcpy(copy, encoded, encoded_size);
	copy[12] = (uint8_t)((copy[12] & 0x0fu) | 0x80u);
	CHECK(!rg_rgs_read_header(copy, encoded_size, NULL));
	memcpy(copy, encoded, encoded_size);
	copy[16] = 1u;
	copy[17] = 0u;
	CHECK(!rg_rgs_read_header(copy, encoded_size, NULL));

	/* Extreme serialized LMS values exercise defined predictor arithmetic. */
	memcpy(copy, encoded, encoded_size);
	memset(copy + RG_RGS_HEADER_SIZE + RG_RGS_FRAME_HEADER_SIZE, 0xff, 32u);
	CHECK(rg_rgs_decode_s16(copy, encoded_size, decoded, 5300u * 2u, NULL) == 5300u * 2u);

	free(copy);
	free(decoded);
	free(encoded);
}

int main(void)
{
	test_constants_and_defaults();
	test_invalid_descriptors();
	test_v13_lineage_golden();
	test_roundtrips();
	test_quality_and_bitrate_hint();
	test_rejections();
	printf("rg_rgs: %d checks, %d failures\n", tests_run, tests_failed);
	return tests_failed == 0 ? 0 : 1;
}
