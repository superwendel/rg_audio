#include <stdint.h>
#include <limits.h>
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

static void test_floor_value(int64_t value, uint32_t shift)
{
	int64_t expected;
	if (shift == 63u)
	{
		expected = value < 0 ? -1 : 0;
	}
	else
	{
		/* Division truncates toward zero; a negative remainder requires the
		 * preceding integer. The divisor is positive, including at shift zero. */
		const int64_t divisor = INT64_C(1) << shift;
		expected = value / divisor;
		if (value % divisor < 0)
		{
			expected--;
		}
	}
	CHECK(rg_rgs_floor_div_pow2_i64(value, shift) == expected);
}

static void test_floor_div_pow2(void)
{
	static const int64_t extremes[] = {
	    INT64_MIN, INT64_MIN + 1, INT64_MIN + 15, INT64_MIN + 8191,
	    INT64_MIN + 8192, INT64_MIN + 65535,
	    INT64_MAX, INT64_MAX - 1, INT64_MAX - 15, INT64_MAX - 8191,
	    INT64_MAX - 8192, INT64_MAX - 65535,
	    INT32_MIN, INT32_MAX, -65537, -65536, -65535, -8193, -8192, -8191,
	    -17, -16, -15, -2, -1, 0, 1, 2, 15, 16, 17, 8191, 8192, 8193,
	    65535, 65536, 65537};
	uint32_t random = 0x243f6a88u;
	uint32_t shift;
	size_t i;
	for (shift = 0u; shift <= 63u; shift++)
	{
		for (i = 0u; i < sizeof(extremes) / sizeof(extremes[0]); i++)
		{
			test_floor_value(extremes[i], shift);
		}
		if (shift < 63u)
		{
			const int64_t divisor = INT64_C(1) << shift;
			test_floor_value(divisor - 1, shift);
			test_floor_value(divisor, shift);
			test_floor_value(divisor + 1, shift);
			test_floor_value(-divisor - 1, shift);
			test_floor_value(-divisor, shift);
			test_floor_value(-divisor + 1, shift);
		}
	}
	for (i = 0u; i < 1024u; i++)
	{
		uint64_t word = (uint64_t)test_prng(&random) << 32u;
		int64_t value;
		word |= test_prng(&random);
		value = (int64_t)(word & (uint64_t)INT64_MAX);
		if ((word >> 63u) != 0u)
		{
			value = -value - 1;
		}
		for (shift = 0u; shift <= 63u; shift++)
		{
			test_floor_value(value, shift);
		}
	}
}

static void test_clamp_boundaries(void)
{
	static const int values[] = {
	    INT_MIN, INT_MIN + 1, -65536, -32769, -32768, -32767,
	    -1, 0, 1, 32766, 32767, 32768, 65535, INT_MAX - 1, INT_MAX};
	size_t i;
	for (i = 0u; i < sizeof(values) / sizeof(values[0]); i++)
	{
		int expected = values[i];
		if (expected < -32768)
		{
			expected = -32768;
		}
		else if (expected > 32767)
		{
			expected = 32767;
		}
		CHECK(rg_rgs_clamp_s16(values[i]) == expected);
	}
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
	CHECK(rg_rgs_encode_bound(1u, 1u, 48000u) == 0u);
	CHECK(rg_rgs_encode_bound(1u, 1u, 96000u) == 0u);
	CHECK(rg_rgs_encode_s16(pcm, 1u, 1u, 44101u, dst, sizeof(dst)) == 0u);
	CHECK(rg_rgs_encode_s16(pcm, 1u, 1u, 48000u, dst, sizeof(dst)) == 0u);
	CHECK(rg_rgs_encode_s16(pcm, 1u, 1u, 96000u, dst, sizeof(dst)) == 0u);
	CHECK(rg_rgs_encode_s16(NULL, 1u, 1u, 44100u, dst, sizeof(dst)) == 0u);
	CHECK(rg_rgs_encode_s16(pcm, 1u, 1u, 44100u, NULL, sizeof(dst)) == 0u);
	options.quality = (RgRgsQuality)-1;
	CHECK(rg_rgs_encode_s16_ex(pcm, 1u, 1u, 44100u, dst, sizeof(dst), &options) == 0u);
	options.quality = (RgRgsQuality)3;
	CHECK(rg_rgs_encode_s16_ex(pcm, 1u, 1u, 44100u, dst, sizeof(dst), &options) == 0u);
	CHECK(dst[0] == 0x5a);
}

static void test_legacy_v1_golden(void)
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
	/* PCM independently replayed with mathematical floor division. Encoder
	 * quality improvements may change new files; this old stream stays valid. */
	static const int16_t expected_pcm[20] = {
	    -7718, -7263, -7238, -5140, -3578, -2966, -3562, -2834, -685, 227,
	    -255, 492, 2443, 2859, 4283, 6685, 7429, 8956, 8876, 9542};
	int16_t pcm[20];
	uint8_t damaged[sizeof(expected)];
	CHECK(rg_rgs_read_header(expected, sizeof(expected), NULL));
	CHECK(rg_rgs_decode_s16(expected, sizeof(expected), pcm, 20u, NULL) == 20u);
	CHECK(memcmp(pcm, expected_pcm, sizeof(pcm)) == 0);
	CHECK(rg_rgs_decode_trusted_s16(expected, sizeof(expected), pcm, 20u, NULL) == 20u);
	CHECK(memcmp(pcm, expected_pcm, sizeof(pcm)) == 0);
	memcpy(damaged, expected, sizeof(expected));
	damaged[sizeof(damaged) - 1u] |= 0xf0u;
	CHECK(!rg_rgs_read_header(damaged, sizeof(damaged), NULL));
	CHECK(rg_rgs_decode_s16(damaged, sizeof(damaged), pcm, 20u, NULL) == 0u);
}

static void test_roundtrip_case(uint32_t frames, uint32_t channels, uint32_t rate)
{
	size_t values = (size_t)frames * channels;
	int16_t* pcm = (int16_t*)malloc(values * sizeof(int16_t));
	uint8_t* encoded = NULL;
	size_t encoded_size = 0u;
	RgRgsInfo info = {0};
	int header_valid;
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
	header_valid = rg_rgs_read_header(encoded, encoded_size, &info);
	CHECK(header_valid);
	if (!header_valid)
	{
		free(encoded);
		free(pcm);
		return;
	}
	stored_frames = frames;
	CHECK(info.channels == channels);
	CHECK(info.samplerate == rate);
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
	test_roundtrip_case(137u, 2u, 44100u);
	test_roundtrip_case(7u, 1u, 1u);
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

static uint64_t test_hash_byte(uint64_t hash, uint8_t value)
{
	return (hash ^ value) * UINT64_C(1099511628211);
}

/* Deterministic high-frequency transients reproduce predictor runaway without
 * depending on downloaded audio. Frequency and level jump every 997 samples. */
static void fill_encoder_transients(int16_t* pcm, uint32_t frames,
                                    uint32_t channels, uint32_t c, uint32_t seed)
{
	uint32_t phase = seed * 0x9e3779b9u;
	uint32_t phase2 = seed * 0x85ebca6bu;
	uint32_t random = seed;
	uint32_t frequency = 0u;
	uint32_t frequency2 = 0u;
	int amplitude = 0;
	for (uint32_t i = 0u; i < frames; i++)
	{
		int triangle;
		int triangle2;
		int value;
		if (i % 997u == 0u)
		{
			random = random * 1664525u + 1013904223u;
			frequency = 0x10000u + (random & 0x3fffffffu);
			frequency2 = 0x20000u + ((random >> 1u) & 0x3fffffffu);
			amplitude = 22000 + (int)((random >> 16u) % 10000u);
		}
		phase += frequency + (i % 997u) * (seed % 31u) * 1024u;
		phase2 += frequency2;
		triangle = (int)((phase >> 15u) & 65535u);
		triangle2 = (int)((phase2 >> 15u) & 65535u);
		if ((phase & 0x80000000u) != 0u) triangle = 65535 - triangle;
		if ((phase2 & 0x80000000u) != 0u) triangle2 = 65535 - triangle2;
		value = ((triangle - 32768) * amplitude / 32768) * 3 / 4 +
		        ((triangle2 - 32768) * amplitude / 32768) / 4;
		if ((i / 997u) % 7u == 3u) value /= 16;
		pcm[(size_t)i * channels + c] = (int16_t)value;
	}
}

static void test_encoder_transient_recovery(void)
{
	enum { FRAMES = 15361 };
	for (uint32_t channels = 1u; channels <= 2u; channels++)
	{
		size_t values = (size_t)FRAMES * channels;
		size_t bound = rg_rgs_encode_bound(FRAMES, channels, 44100u);
		int16_t* pcm = (int16_t*)malloc(values * sizeof(int16_t));
		int16_t* decoded = (int16_t*)malloc(values * sizeof(int16_t));
		int16_t* trusted = (int16_t*)malloc(values * sizeof(int16_t));
		uint8_t* encoded = (uint8_t*)malloc(bound + 32u);
		CHECK(pcm != NULL && decoded != NULL && trusted != NULL && encoded != NULL);
		if (pcm == NULL || decoded == NULL || trusted == NULL || encoded == NULL)
		{
			free(encoded); free(trusted); free(decoded); free(pcm);
			return;
		}
		for (uint32_t c = 0u; c < channels; c++)
			fill_encoder_transients(pcm, FRAMES, channels, c, channels == 1u ? 7u : 2u + c);
		for (uint32_t quality = 0u; quality < 3u; quality++)
		{
			RgRgsEncodeOptions options = {(RgRgsQuality)quality, 0u};
			size_t size;
			memset(encoded, 0xa5, bound + 32u);
			size = rg_rgs_encode_s16_ex(pcm, FRAMES, channels, 44100u, encoded, bound, &options);
			CHECK(size != 0u && size <= bound);
			if (size == 0u || size > bound) continue;
			CHECK(encoded[8] == 1u);
			CHECK(rg_rgs_decode_s16(encoded, size, decoded, values, NULL) == values);
			CHECK(rg_rgs_decode_trusted_s16(encoded, size, trusted, values, NULL) == values);
			CHECK(memcmp(decoded, trusted, values * sizeof(int16_t)) == 0);
			for (size_t i = bound; i < bound + 32u; i++) CHECK(encoded[i] == 0xa5);
			for (uint32_t c = 0u; c < channels; c++)
			{
				uint64_t error = 0u;
				uint64_t energy = 0u;
				uint32_t full_scale = 0u;
				for (uint32_t i = 0u; i < FRAMES; i++)
				{
					int64_t source = pcm[(size_t)i * channels + c];
					int64_t output = decoded[(size_t)i * channels + c];
					int64_t delta = source - output;
					error += (uint64_t)(delta * delta);
					energy += (uint64_t)(source * source);
					full_scale += output == INT16_MIN || output == INT16_MAX;
				}
				/* Previously medium/low generated more error energy than signal
				 * and sustained full-scale output on these non-clipped signals. */
				CHECK(error < energy);
				CHECK(full_scale < FRAMES / 20u);
			}
		}
		free(encoded); free(trusted); free(decoded); free(pcm);
	}
}

static void test_encoder_corpus(void)
{
	/* Freeze the encoder's quality policy separately from the legacy decoder
	 * fixture above. PCM is hashed little-endian, independent of host endian. */
	static const uint32_t cases[][2] = {
	    {1u, 1u}, {19u, 2u}, {20u, 3u}, {21u, 6u}, {159u, 8u},
	    {160u, 1u}, {161u, 2u}, {5119u, 1u}, {5120u, 2u},
	    {5121u, 3u}, {10253u, 8u}};
	static const uint64_t expected_encoded[3] = {
	    UINT64_C(12027520164591838922), UINT64_C(15923796845561030895), UINT64_C(6106236903940296917)};
	static const uint64_t expected_decoded[3] = {
	    UINT64_C(13741165448962049277), UINT64_C(17172080833171131148), UINT64_C(4542994728474891246)};
	uint32_t quality;
	uint32_t observed_modes = 0u;
	for (quality = 0u; quality < 3u; quality++)
	{
		uint64_t encoded_hash = UINT64_C(14695981039346656037);
		uint64_t decoded_hash = UINT64_C(14695981039346656037);
		size_t test_case;
		for (test_case = 0u; test_case < sizeof(cases) / sizeof(cases[0]); test_case++)
		{
			const uint32_t frames = cases[test_case][0];
			const uint32_t channels = cases[test_case][1];
			const size_t values = (size_t)frames * channels;
			const size_t bound = rg_rgs_encode_bound(frames, channels, 44100u);
			int16_t* pcm = (int16_t*)malloc(values * sizeof(int16_t));
			int16_t* decoded = (int16_t*)malloc(values * sizeof(int16_t));
			uint8_t* encoded = (uint8_t*)malloc(bound + 32u);
			uint32_t pattern;
			CHECK(pcm != NULL && decoded != NULL && encoded != NULL);
			if (pcm == NULL || decoded == NULL || encoded == NULL)
			{
				free(encoded);
				free(decoded);
				free(pcm);
				return;
			}
			for (pattern = 0u; pattern < 4u; pattern++)
			{
				RgRgsEncodeOptions options = {(RgRgsQuality)quality, 0u};
				size_t size;
				size_t i;
				size_t cursor;
				fill_pcm(pcm, frames, channels, 0x12345678u);
				for (i = 0u; i < values; i++)
				{
					if (pattern == 0u || (pattern == 3u && i / channels % 200u < 100u))
					{
						pcm[i] = 0;
					}
					else if (pattern == 1u)
					{
						pcm[i] = (i / channels + i % channels) % 2u != 0u ? INT16_MIN : INT16_MAX;
					}
				}
				memset(encoded, 0xa5, bound + 32u);
				CHECK(rg_rgs_encode_s16_ex(pcm, frames, channels, 44100u,
				    encoded, bound - 1u, &options) == 0u);
				CHECK(encoded[0] == 0xa5);
				size = rg_rgs_encode_s16_ex(pcm, frames, channels, 44100u,
				    encoded, bound, &options);
				CHECK(size != 0u && size <= bound);
				for (i = bound; i < bound + 32u; i++)
				{
					CHECK(encoded[i] == 0xa5);
				}
				if (size == 0u || size > bound)
				{
					break;
				}
				CHECK(rg_rgs_decode_s16(encoded, size, decoded, values, NULL) == values);
				for (i = 0u; i < size; i++)
				{
					encoded_hash = test_hash_byte(encoded_hash, encoded[i]);
				}
				for (i = 0u; i < values; i++)
				{
					uint16_t sample = (uint16_t)decoded[i];
					decoded_hash = test_hash_byte(decoded_hash, (uint8_t)sample);
					decoded_hash = test_hash_byte(decoded_hash, (uint8_t)(sample >> 8u));
				}
				for (cursor = RG_RGS_HEADER_SIZE; cursor < size;)
				{
					observed_modes |= 1u << ((encoded[cursor] >> 4u) - 4u);
					cursor += rg_rgs_read_u16le(encoded + cursor + 6u);
				}
			}
			free(encoded);
			free(decoded);
			free(pcm);
		}
		if (encoded_hash != expected_encoded[quality] || decoded_hash != expected_decoded[quality])
		{
			fprintf(stderr, "Encoder corpus quality %u: encoded=%llu decoded=%llu\n", quality,
			    (unsigned long long)encoded_hash, (unsigned long long)decoded_hash);
		}
		CHECK(encoded_hash == expected_encoded[quality]);
		CHECK(decoded_hash == expected_decoded[quality]);
	}
	CHECK(observed_modes == 7u);
}

static void test_bound_limits(void)
{
	CHECK(rg_rgs_encode_bound(1u, 1u, 44100u) == 45u);
	CHECK(rg_rgs_encode_bound(5120u, 2u, 44100u) == 4212u);
	CHECK(rg_rgs_encode_bound(5121u, 2u, 44100u) == 4270u);
	/* The maximum descriptor is representable on 64-bit hosts. On 32-bit
	 * hosts the input PCM itself cannot fit in size_t and must be rejected. */
#if SIZE_MAX > UINT32_MAX
	CHECK(rg_rgs_encode_bound(UINT32_MAX, 1u, 44100u) == UINT64_C(1764963142));
#else
	CHECK(rg_rgs_encode_bound(UINT32_MAX, 1u, 44100u) == 0u);
#endif
}

static void test_slice_reference(void)
{
	uint32_t width;
	uint32_t scale;
	uint32_t length;
	uint32_t random = 0xdeadbeefu;
	for (width = 2u; width <= 3u; width++)
	{
		for (scale = 0u; scale < 16u; scale++)
		{
			for (length = 1u; length <= 20u; length++)
			{
				RgRgsLms actual = {{INT16_MIN, INT16_MAX, -1, 0},
				                   {INT16_MAX, INT16_MIN, INT16_MAX, INT16_MIN}};
				RgRgsLms expected = actual;
				int16_t output[20u * 8u];
				uint64_t packed = scale;
				uint32_t i;
				for (i = 0u; i < length; i++)
				{
					uint32_t code = test_prng(&random) >> (32u - width);
					packed |= (uint64_t)code << (4u + i * width);
				}
				memset(output, 0x5a, sizeof(output));
				rg_rgs_decode_packed_slice(packed, width, &actual, output, 8u, 7u, 0u, length);
				for (i = 0u; i < length; i++)
				{
					uint32_t tap;
					uint32_t code = (uint32_t)(packed >> (4u + i * width)) & ((1u << width) - 1u);
					int residual = width == 3u ? rg_rgs_dequant_tab[scale][code] : rg_rgs_dequant2_tab[scale][code];
					int delta = residual / 16 - (residual < 0 && residual % 16 != 0);
					int64_t prediction = 0;
					int64_t sample;
					for (tap = 0u; tap < 4u; tap++)
					{
						prediction += (int64_t)expected.weights[tap] * expected.history[tap];
					}
					sample = prediction / 8192 - (prediction < 0 && prediction % 8192 != 0) + residual;
					if (sample < INT16_MIN) sample = INT16_MIN;
					if (sample > INT16_MAX) sample = INT16_MAX;
					CHECK(output[i * 8u + 7u] == sample);
					for (tap = 0u; tap < 4u; tap++)
					{
						expected.weights[tap] += expected.history[tap] < 0 ? -delta : delta;
					}
					for (tap = 0u; tap < 3u; tap++) expected.history[tap] = expected.history[tap + 1u];
					expected.history[3] = (int)sample;
				}
				CHECK(memcmp(&actual, &expected, sizeof(actual)) == 0);
				for (i = 0u; i < 20u * 8u; i++)
				{
					if (i % 8u != 7u || i / 8u >= length) CHECK(output[i] == 0x5a5a);
				}
			}
		}
	}
}

static void test_full_frame_lms_limits(void)
{
	/* A valid frame can grow weights far beyond their serialized int16 range.
	 * Maximum same-sign residuals keep PCM saturated and produce a closed-form
	 * state, exercising wide products and every slice's state writeback. */
	uint32_t width;
	for (width = 2u; width <= 3u; width++)
	{
		uint32_t negative;
		for (negative = 0u; negative <= 1u; negative++)
		{
			const int sample = negative != 0u ? INT16_MIN : INT16_MAX;
			const int increment = width == 3u ? 896 : 576;
			const uint32_t code = (width == 3u ? 6u : 2u) + negative;
			RgRgsLms state;
			int16_t output[RG_RGS_MAX_FRAME_SAMPLES + 1u];
			uint64_t packed = 15u;
			uint32_t i;
			uint32_t offset;
			for (i = 0u; i < 4u; i++)
			{
				state.history[i] = sample;
				state.weights[i] = INT16_MAX;
			}
			for (i = 0u; i < 20u; i++)
			{
				packed |= (uint64_t)code << (4u + i * width);
			}
			memset(output, 0x5a, sizeof(output));
			for (offset = 0u; offset < RG_RGS_MAX_FRAME_SAMPLES; offset += 20u)
			{
				const int expected_weight = INT16_MAX + (int)(offset + 20u) * increment;
				rg_rgs_decode_packed_slice(packed, width, &state, output, 1u, 0u, offset, 20u);
				for (i = 0u; i < 4u; i++)
				{
					CHECK(state.history[i] == sample);
					CHECK(state.weights[i] == expected_weight);
				}
			}
			for (i = 0u; i < RG_RGS_MAX_FRAME_SAMPLES; i++)
			{
				CHECK(output[i] == sample);
			}
			CHECK(output[RG_RGS_MAX_FRAME_SAMPLES] == 0x5a5a);
		}
	}
}

static void test_store_le(uint8_t* dst, uint64_t value, uint32_t bytes)
{
	uint32_t i;
	for (i = 0u; i < bytes; i++) dst[i] = (uint8_t)(value >> (8u * i));
}

static int16_t test_reference_sample(RgRgsLms* state, int residual)
{
	int64_t prediction = 0;
	int64_t sample;
	const int delta = residual / 16 - (residual < 0 && residual % 16 != 0);
	uint32_t tap;
	for (tap = 0u; tap < 4u; tap++)
	{
		prediction += (int64_t)state->weights[tap] * state->history[tap];
	}
	/* Deliberately use division/remainder and an explicit history shift, not
	 * the decoder's floor, clamp, or packed-slice implementation. */
	sample = prediction / 8192 - (prediction < 0 && prediction % 8192 != 0) + residual;
	if (sample < INT16_MIN) sample = INT16_MIN;
	if (sample > INT16_MAX) sample = INT16_MAX;
	for (tap = 0u; tap < 4u; tap++)
	{
		state->weights[tap] += state->history[tap] < 0 ? -delta : delta;
	}
	for (tap = 0u; tap < 3u; tap++) state->history[tap] = state->history[tap + 1u];
	state->history[3] = (int)sample;
	return (int16_t)sample;
}

static void test_encoder_serialized_state(void)
{
	/* Exercise incoming weights outside their serialized range. Independently
	 * replay every packed sample and compare the encoder's final carried state. */
	enum { FRAMES = 161, CHANNELS = 8 };
	int16_t pcm[FRAMES * CHANNELS];
	uint8_t encoded[2048];
	const uint32_t slices = (FRAMES + 19u) / 20u;
	const uint32_t map_bytes = (slices + 7u) / 8u;
	for (uint32_t c = 0u; c < CHANNELS; c++)
		fill_encoder_transients(pcm, FRAMES, CHANNELS, c, c + 2u);
	for (uint32_t q = 0u; q < 3u; q++)
	{
		RgRgsEncodeOptions options = {(RgRgsQuality)q, 0u};
		RgRgsLms state[CHANNELS];
		size_t size;
		size_t position = 8u + 16u * CHANNELS;
		uint32_t flags;
		for (uint32_t c = 0u; c < CHANNELS; c++)
		{
			for (uint32_t tap = 0u; tap < 4u; tap++)
			{
				state[c].history[tap] = ((tap + c) & 1u) != 0u ? 30000 : -30000;
				state[c].weights[tap] = (tap & 1u) != 0u ? 70001 : -70001;
			}
		}
		memset(encoded, 0xa5, sizeof(encoded));
		size = rg_rgs_encode_frame_guarded(pcm, CHANNELS, 44100u, FRAMES,
		    state, encoded, &options);
		CHECK(size <= rg_rgs_encode_bound(FRAMES, CHANNELS, 44100u) - RG_RGS_HEADER_SIZE);
		flags = encoded[0] >> 4u;
		if (flags == 5u) position += CHANNELS * map_bytes;
		for (uint32_t c = 0u; c < CHANNELS; c++)
		{
			RgRgsLms expected;
			for (uint32_t tap = 0u; tap < 4u; tap++)
			{
				size_t offset = 8u + c * 16u + tap * 2u;
				int h = encoded[offset] | ((int)encoded[offset + 1u] << 8u);
				int w = encoded[offset + 8u] | ((int)encoded[offset + 9u] << 8u);
				expected.history[tap] = h < 32768 ? h : h - 65536;
				expected.weights[tap] = w < 32768 ? w : w - 65536;
			}
			for (uint32_t s = 0u; s < slices; s++)
			{
				uint32_t width = flags == 6u || (flags == 5u &&
				    (encoded[8u + CHANNELS * 16u + c * map_bytes + s / 8u] & (1u << (s % 8u)))) ? 2u : 3u;
				uint64_t packed = 0u;
				uint32_t length = FRAMES - s * 20u;
				uint32_t scale;
				if (length > 20u) length = 20u;
				for (uint32_t b = 0u; b < (width == 2u ? 6u : 8u); b++)
					packed |= (uint64_t)encoded[position++] << (b * 8u);
				scale = (uint32_t)(packed & 15u);
				packed >>= 4u;
				for (uint32_t i = 0u; i < length; i++)
				{
					uint32_t code = (uint32_t)(packed & ((1u << width) - 1u));
					int residual = width == 2u ? rg_rgs_dequant2_tab[scale][code] : rg_rgs_dequant_tab[scale][code];
					(void)test_reference_sample(&expected, residual);
					packed >>= width;
				}
			}
			for (uint32_t tap = 0u; tap < 4u; tap++)
			{
				CHECK(expected.history[tap] == state[c].history[tap]);
				CHECK(expected.weights[tap] == state[c].weights[tap]);
			}
		}
		CHECK(position == size);
		for (size_t i = rg_rgs_encode_bound(FRAMES, CHANNELS, 44100u) - RG_RGS_HEADER_SIZE;
		     i < sizeof(encoded); i++)
			CHECK(encoded[i] == 0xa5);
	}
}

static void test_encoder_weight_normalization(void)
{
	/* Both states serialize to the same header. This quiet one-sample input
	 * cannot trigger a retry, so a retry cannot hide a prediction mismatch. */
	const int16_t pcm[1] = {10};
	for (uint32_t q = 0u; q < 3u; q++)
	{
		RgRgsEncodeOptions options = {(RgRgsQuality)q, 0u};
		RgRgsLms wide = {{0, 0, 0, 1}, {0, 0, 0, 73728}};
		RgRgsLms stored = {{0, 0, 0, 1}, {0, 0, 0, 8192}};
		uint8_t actual[64];
		uint8_t expected[64];
		size_t actual_size = rg_rgs_encode_frame_guarded(pcm, 1u, 44100u, 1u,
		    &wide, actual, &options);
		size_t expected_size = rg_rgs_encode_frame_guarded(pcm, 1u, 44100u, 1u,
		    &stored, expected, &options);
		CHECK(actual_size == expected_size);
		CHECK(memcmp(actual, expected, expected_size) == 0);
		for (uint32_t tap = 0u; tap < 4u; tap++)
		{
			CHECK(wide.history[tap] == stored.history[tap]);
			CHECK(wide.weights[tap] == stored.weights[tap]);
		}
	}
}

static void test_encoder_channel_isolation(void)
{
	/* A retry on the transient channel must preserve the quiet channel's
	 * initial state, chosen slices, decoded PCM, and carried final state. */
	enum { FRAMES = 5120, CHANNELS = 2, SLICES = 256, MAP_BYTES = 32 };
	int16_t pcm[FRAMES * CHANNELS];
	uint8_t encoded[2][RG_RGS_FRAME_SIZE(CHANNELS, SLICES) + CHANNELS * MAP_BYTES];
	RgRgsLms final_state[2][CHANNELS];
	RgRgsEncodeOptions options = {RG_RGS_QUALITY_MEDIUM, 0u};
	size_t size[2];
	size_t position[2] = {8u + CHANNELS * 16u, 8u + CHANNELS * 16u};
	uint32_t flags[2];
	uint64_t left_error[2] = {0u, 0u};

	memset(pcm, 0, sizeof(pcm));
	fill_encoder_transients(pcm, FRAMES, CHANNELS, 0u, 7u);
	memset(final_state, 0, sizeof(final_state));
	for (uint32_t v = 0u; v < 2u; v++)
	{
		for (uint32_t c = 0u; c < CHANNELS; c++)
		{
			final_state[v][c].weights[2] = -8192;
			final_state[v][c].weights[3] = 16384;
		}
	}
	size[0] = rg_rgs_encode_frame_planar_mixed(pcm, CHANNELS, 44100u, FRAMES,
	    final_state[0], encoded[0], &options);
	size[1] = rg_rgs_encode_frame_guarded(pcm, CHANNELS, 44100u, FRAMES,
	    final_state[1], encoded[1], &options);
	for (uint32_t v = 0u; v < 2u; v++)
	{
		CHECK(size[v] >= position[v] && size[v] <= sizeof(encoded[v]));
		if (size[v] < position[v] || size[v] > sizeof(encoded[v])) return;
		flags[v] = encoded[v][0] >> 4u;
		CHECK(flags[v] == 4u || flags[v] == 5u || flags[v] == 6u);
		if (flags[v] != 4u && flags[v] != 5u && flags[v] != 6u) return;
		if (flags[v] == 5u) position[v] += CHANNELS * MAP_BYTES;
	}
	CHECK(memcmp(encoded[0] + 24u, encoded[1] + 24u, 16u) == 0);
	for (uint32_t c = 0u; c < CHANNELS; c++)
	{
		RgRgsLms replay[2];
		for (uint32_t v = 0u; v < 2u; v++)
		{
			for (uint32_t tap = 0u; tap < 4u; tap++)
			{
				size_t offset = 8u + c * 16u + tap * 2u;
				int h = encoded[v][offset] | ((int)encoded[v][offset + 1u] << 8u);
				int w = encoded[v][offset + 8u] | ((int)encoded[v][offset + 9u] << 8u);
				replay[v].history[tap] = h < 32768 ? h : h - 65536;
				replay[v].weights[tap] = w < 32768 ? w : w - 65536;
			}
		}
		for (uint32_t s = 0u; s < SLICES; s++)
		{
			uint32_t width[2];
			uint32_t scale[2];
			uint32_t payload_bytes[2];
			uint64_t packed[2] = {0u, 0u};
			for (uint32_t v = 0u; v < 2u; v++)
			{
				width[v] = flags[v] == 6u || (flags[v] == 5u &&
				    (encoded[v][8u + CHANNELS * 16u + c * MAP_BYTES + s / 8u] &
				     (1u << (s % 8u))) != 0u) ? 2u : 3u;
				payload_bytes[v] = width[v] == 2u ? 6u : 8u;
				CHECK(position[v] + payload_bytes[v] <= size[v]);
				if (position[v] + payload_bytes[v] > size[v]) return;
				for (uint32_t b = 0u; b < payload_bytes[v]; b++)
					packed[v] |= (uint64_t)encoded[v][position[v] + b] << (b * 8u);
				scale[v] = (uint32_t)(packed[v] & 15u);
				packed[v] >>= 4u;
			}
			if (c == 1u)
			{
				CHECK(width[0] == width[1]);
				if (width[0] == width[1])
					CHECK(memcmp(encoded[0] + position[0], encoded[1] + position[1], payload_bytes[0]) == 0);
			}
			position[0] += payload_bytes[0];
			position[1] += payload_bytes[1];
			for (uint32_t i = 0u; i < 20u; i++)
			{
				int16_t decoded[2];
				for (uint32_t v = 0u; v < 2u; v++)
				{
					uint32_t code = (uint32_t)(packed[v] & ((1u << width[v]) - 1u));
					int residual = width[v] == 2u ? rg_rgs_dequant2_tab[scale[v]][code] :
					                               rg_rgs_dequant_tab[scale[v]][code];
					decoded[v] = test_reference_sample(&replay[v], residual);
					packed[v] >>= width[v];
					if (c == 0u)
					{
						int64_t difference = (int64_t)pcm[(s * 20u + i) * CHANNELS] - decoded[v];
						left_error[v] += (uint64_t)(difference * difference);
					}
				}
				if (c == 1u) CHECK(decoded[0] == decoded[1]);
			}
		}
		if (c == 1u)
		{
			for (uint32_t tap = 0u; tap < 4u; tap++)
			{
				CHECK(final_state[0][c].history[tap] == final_state[1][c].history[tap]);
				CHECK(final_state[0][c].weights[tap] == final_state[1][c].weights[tap]);
				for (uint32_t v = 0u; v < 2u; v++)
				{
					CHECK(replay[v].history[tap] == final_state[v][c].history[tap]);
					CHECK(replay[v].weights[tap] == final_state[v][c].weights[tap]);
				}
			}
		}
	}
	CHECK(position[0] == size[0] && position[1] == size[1]);
	CHECK(left_error[1] < left_error[0]);
}

static void test_encoder_peak_recovery(void)
{
	/* A single polarity reversal on a constant signal produces a large local
	 * error despite low frame RMS error and high SNR. No source asset is used. */
	enum { FRAMES = 5120, SLICES = 256, MAP_BYTES = 32 };
	int16_t pcm[FRAMES];
	uint8_t encoded[RG_RGS_FRAME_SIZE(1u, SLICES) + MAP_BYTES];
	uint64_t energy = 0u;
	for (uint32_t i = 0u; i < FRAMES; i++)
	{
		pcm[i] = i == 256u ? -16000 : 16000;
		energy += (uint64_t)((int64_t)pcm[i] * pcm[i]);
	}
	for (uint32_t q = 0u; q < 3u; q++)
	{
		RgRgsEncodeOptions options = {(RgRgsQuality)q, 0u};
		uint64_t error[2] = {0u, 0u};
		uint32_t peak[2] = {0u, 0u};
		int unexpected_clipping[2] = {0, 0};
		for (uint32_t guarded = 0u; guarded < 2u; guarded++)
		{
			RgRgsLms state = {{16000, 16000, 16000, 16000}, {0, 0, -8192, 16384}};
			RgRgsLms replay;
			size_t position = 24u;
			size_t size = guarded != 0u ?
			    rg_rgs_encode_frame_guarded(pcm, 1u, 44100u, FRAMES, &state, encoded, &options) :
			    rg_rgs_encode_frame_planar_mixed(pcm, 1u, 44100u, FRAMES, &state, encoded, &options);
			uint32_t flags = encoded[0] >> 4u;
			CHECK(size >= 24u && size <= sizeof(encoded));
			if (size < 24u || size > sizeof(encoded)) return;
			CHECK(flags == 4u || flags == 5u || flags == 6u);
			if (flags != 4u && flags != 5u && flags != 6u) return;
			if (flags == 5u) position += MAP_BYTES;
			for (uint32_t tap = 0u; tap < 4u; tap++)
			{
				size_t offset = 8u + tap * 2u;
				int h = encoded[offset] | ((int)encoded[offset + 1u] << 8u);
				int w = encoded[offset + 8u] | ((int)encoded[offset + 9u] << 8u);
				replay.history[tap] = h < 32768 ? h : h - 65536;
				replay.weights[tap] = w < 32768 ? w : w - 65536;
			}
			for (uint32_t s = 0u; s < SLICES; s++)
			{
				uint32_t width = flags == 6u || (flags == 5u &&
				    (encoded[24u + s / 8u] & (1u << (s % 8u))) != 0u) ? 2u : 3u;
				uint32_t payload_bytes = width == 2u ? 6u : 8u;
				uint64_t packed = 0u;
				uint32_t scale;
				CHECK(position + payload_bytes <= size);
				if (position + payload_bytes > size) return;
				for (uint32_t b = 0u; b < payload_bytes; b++)
					packed |= (uint64_t)encoded[position++] << (b * 8u);
				scale = (uint32_t)(packed & 15u);
				packed >>= 4u;
				for (uint32_t i = 0u; i < 20u; i++)
				{
					uint32_t code = (uint32_t)(packed & ((1u << width) - 1u));
					int residual = width == 2u ? rg_rgs_dequant2_tab[scale][code] : rg_rgs_dequant_tab[scale][code];
					int decoded = test_reference_sample(&replay, residual);
					int64_t difference = (int64_t)pcm[s * 20u + i] - decoded;
					uint32_t absolute = (uint32_t)(difference < 0 ? -difference : difference);
					error[guarded] += (uint64_t)(difference * difference);
					if (absolute > peak[guarded]) peak[guarded] = absolute;
					if ((decoded == -32768 || decoded == 32767) && absolute > 4096u)
						unexpected_clipping[guarded] = 1;
					packed >>= width;
				}
			}
			CHECK(position == size);
		}
		CHECK(!unexpected_clipping[0]);
		CHECK(error[0] < energy / 100u);
		CHECK(error[0] < (uint64_t)FRAMES * 448u * 448u);
		CHECK(peak[0] >= 16384u);
		CHECK(error[1] < error[0]);
		CHECK(peak[1] < peak[0]);
	}
}

static void test_mono_frame_case(uint32_t samples, uint32_t mode,
                                  uint32_t scale, uint32_t pattern)
{
	/* These fixtures bypass the encoder, but enter every decoder through its
	 * public API. They therefore cover frame dispatch as well as the kernel.
	 * Two frames allow the maximum-length frame followed by each short tail. */
	uint8_t encoded[RG_RGS_HEADER_SIZE + 2u *
	    (RG_RGS_FRAME_SIZE(1u, RG_RGS_SLICES_PER_FRAME) +
	     RG_RGS_PLANAR_MODE_BYTES(RG_RGS_SLICES_PER_FRAME))];
	int16_t expected[RG_RGS_MAX_FRAME_SAMPLES + 20u];
	int16_t output[RG_RGS_MAX_FRAME_SAMPLES + 22u];
	uint32_t random = 0x9e3779b9u ^ samples ^ (mode << 16u) ^ (scale << 20u) ^ pattern;
	uint32_t start;
	size_t size = RG_RGS_HEADER_SIZE;
	RgRgsInfo info;
	RgRgsDecoder decoder;
	uint32_t decoded = 0u;
	uint32_t frames = 0u;
	int valid;
	CHECK(samples > 0u && samples <= RG_RGS_MAX_FRAME_SAMPLES + 20u);
	if (samples == 0u || samples > RG_RGS_MAX_FRAME_SAMPLES + 20u) return;
	memset(encoded, 0, sizeof(encoded));
	memcpy(encoded, "rgs!", 4u);
	test_store_le(encoded + 4u, samples, 4u);
	encoded[8] = RG_RGS_VERSION;
	encoded[9] = RG_RGS_FILE_FLAGS;
	for (start = 0u; start < samples;)
	{
		RgRgsLms state;
		uint32_t length = samples - start;
		uint32_t slices;
		uint32_t mode_bytes;
		uint32_t tap;
		uint32_t s;
		uint8_t* frame = encoded + size;
		size_t p = RG_RGS_FRAME_HEADER_SIZE;
		size_t map_start;
		if (length > RG_RGS_MAX_FRAME_SAMPLES) length = RG_RGS_MAX_FRAME_SAMPLES;
		slices = (length + 19u) / 20u;
		mode_bytes = mode == 2u ? (slices + 7u) / 8u : 0u;
		frame[0] = (uint8_t)(1u | ((RG_RGS_FRAME_FLAG_PLANAR |
		    (mode == 1u ? RG_RGS_FRAME_FLAG_ALL_2BIT :
		     mode == 2u ? RG_RGS_FRAME_FLAG_MIXED : 0u)) << 4u));
		test_store_le(frame + 1u, 44100u, 3u);
		test_store_le(frame + 4u, length, 2u);
		for (tap = 0u; tap < 4u; tap++)
		{
			if (pattern == 0u)
			{
				static const int histories[4] = {INT16_MIN, INT16_MAX, -1, 0};
				state.history[tap] = histories[tap];
				state.weights[tap] = (tap & 1u) != 0u ? INT16_MIN : INT16_MAX;
			}
			else if (pattern == 1u)
			{
				static const int histories[4] = {-30000, 20000, -123, 8191};
				static const int weights[4] = {1, -2, 3, -4};
				state.history[tap] = histories[tap];
				state.weights[tap] = weights[tap];
			}
			else if (pattern == 2u)
			{
				state.history[tap] = (int)(test_prng(&random) >> 16u) - 32768;
				state.weights[tap] = (int)(test_prng(&random) >> 16u) - 32768;
			}
			else
			{
				state.history[tap] = pattern == 3u ? INT16_MAX : INT16_MIN;
				state.weights[tap] = INT16_MAX;
			}
			test_store_le(frame + p, (uint16_t)state.history[tap], 2u);
			p += 2u;
		}
		for (tap = 0u; tap < 4u; tap++)
		{
			test_store_le(frame + p, (uint16_t)state.weights[tap], 2u);
			p += 2u;
		}
		map_start = p;
		p += mode_bytes;
		for (s = 0u; s < slices; s++)
		{
			const uint32_t width = mode == 1u || (mode == 2u && (s & 1u) != 0u) ? 2u : 3u;
			uint64_t packed = scale;
			uint32_t i;
			if (mode == 2u && width == 2u) frame[map_start + (s >> 3u)] |= (uint8_t)(1u << (s & 7u));
			for (i = 0u; i < 20u; i++)
			{
				const uint32_t code = pattern >= 3u ?
				    (width == 3u ? 6u : 2u) + (pattern == 4u) :
				    test_prng(&random) >> (32u - width);
				packed |= (uint64_t)code << (4u + i * width);
				if (s * 20u + i < length)
				{
					const int residual = width == 3u ? rg_rgs_dequant_tab[scale][code] : rg_rgs_dequant2_tab[scale][code];
					expected[start + s * 20u + i] = test_reference_sample(&state, residual);
				}
			}
			test_store_le(frame + p, packed, width == 3u ? 8u : 6u);
			p += width == 3u ? 8u : 6u;
		}
		test_store_le(frame + 6u, p, 2u);
		size += p;
		start += length;
	}
	memset(output, 0x5a, sizeof(output));
	valid = rg_rgs_decode_s16(encoded, size, output + 1u, samples, &info) == samples;
	CHECK(valid);
	if (!valid) return; /* Trusted decoding requires this checked preflight. */
	CHECK(info.channels == 1u && info.samplerate == 44100u && info.samples == samples);
	CHECK(memcmp(output + 1u, expected, samples * sizeof(int16_t)) == 0);
	CHECK(output[0] == 0x5a5a && output[samples + 1u] == 0x5a5a);
	memset(output, 0x5a, sizeof(output));
	CHECK(rg_rgs_decode_trusted_s16(encoded, size, output + 1u, samples, NULL) == samples);
	CHECK(memcmp(output + 1u, expected, samples * sizeof(int16_t)) == 0);
	CHECK(output[0] == 0x5a5a && output[samples + 1u] == 0x5a5a);
	memset(output, 0x5a, sizeof(output));
	valid = rg_rgs_decoder_init(&decoder, encoded, size, NULL);
	CHECK(valid);
	if (!valid) return;
	while (decoded < samples)
	{
		const uint32_t required = samples - decoded > RG_RGS_MAX_FRAME_SAMPLES ?
		    RG_RGS_MAX_FRAME_SAMPLES : samples - decoded;
		CHECK(rg_rgs_decoder_next_s16(&decoder, output + 1u + decoded, required, &frames) == RG_RGS_DECODE_FRAME);
		CHECK(frames == required);
		if (frames != required) return;
		decoded += frames;
	}
	CHECK(rg_rgs_decoder_next_s16(&decoder, output + 1u, samples, &frames) == RG_RGS_DECODE_END);
	CHECK(frames == 0u);
	CHECK(memcmp(output + 1u, expected, samples * sizeof(int16_t)) == 0);
	CHECK(output[0] == 0x5a5a && output[samples + 1u] == 0x5a5a);
	/* A partial final slice still occupies a complete encoded slice. */
	CHECK(rg_rgs_decode_s16(encoded, size - 1u, output + 1u, samples, NULL) == 0u);
}

static void test_mono_frame_reference(void)
{
	uint32_t mode;
	uint32_t scale;
	uint32_t tail;
	uint32_t pattern;
	for (mode = 0u; mode < 3u; mode++)
	{
		for (scale = 0u; scale < 16u; scale++)
		{
			for (tail = 1u; tail <= 20u; tail++)
			{
				for (pattern = 0u; pattern < 3u; pattern++)
				{
					/* Cross both a slice boundary and a mixed-map byte boundary.
					 * Distinct histories catch a wrong unrolled rotation even
					 * when all samples of an extreme fixture would clip. */
					test_mono_frame_case((tail & 1u ? 160u : 20u) + tail, mode, scale, pattern);
				}
			}
		}
		for (pattern = 0u; pattern < 5u; pattern++)
		{
			/* Exercise large evolved weights, all unroll remainders near the
			 * maximum frame size, and independently serialized frame resets. */
			for (tail = 0u; tail < 4u; tail++)
			{
				test_mono_frame_case(RG_RGS_MAX_FRAME_SAMPLES - tail, mode, 15u, pattern);
			}
			for (tail = 1u; tail <= 20u; tail++)
			{
				test_mono_frame_case(RG_RGS_MAX_FRAME_SAMPLES + tail, mode, 15u, pattern);
			}
		}
	}
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
	/* A metadata-only prefix is no longer a successful header read. Every
	 * truncation inside the first frame must fail without publishing metadata. */
	for (; cut < RG_RGS_HEADER_SIZE + rg_rgs_read_u16le(encoded + 18u); cut++)
	{
		RgRgsInfo info = {99u, 98u, 97u};
		CHECK(!rg_rgs_read_header(encoded, cut, &info));
		CHECK(info.channels == 99u && info.samplerate == 98u && info.samples == 97u);
	}
	CHECK(rg_rgs_read_header(encoded, RG_RGS_HEADER_SIZE + rg_rgs_read_u16le(encoded + 18u), NULL));
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
	test_floor_div_pow2();
	test_clamp_boundaries();
	test_constants_and_defaults();
	test_invalid_descriptors();
	test_legacy_v1_golden();
	test_roundtrips();
	test_quality_and_bitrate_hint();
	test_encoder_transient_recovery();
	test_encoder_corpus();
	test_bound_limits();
	test_slice_reference();
	test_full_frame_lms_limits();
	test_encoder_serialized_state();
	test_encoder_weight_normalization();
	test_encoder_channel_isolation();
	test_encoder_peak_recovery();
	test_mono_frame_reference();
	test_rejections();
	printf("rg_rgs: %d checks, %d failures\n", tests_run, tests_failed);
	return tests_failed == 0 ? 0 : 1;
}
