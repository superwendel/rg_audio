#ifndef RG_RGS_H
#define RG_RGS_H

/*
 * rg_rgs - RGS (Reverse Gravity Signal) PCM16 audio codec
 *
 * Public v1 is a little-endian, frame-oriented, QOA-derived format. Encoding
 * and all decoding are allocation-free. Input rates above 44100 Hz must be
 * resampled by the caller before encoding.
 *
 * This implementation derives its LMS predictor, quantizer tables, and slice
 * coding approach from Quite OK Audio (QOA):
 *
 * MIT License
 *
 * Copyright (c) 2023 Dominic Szablewski
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * RGS modifications copyright (c) Steven Wendel.
 */

#include "rg_defs.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/** @name Public format limits
 * @{ */
/** Current and only accepted RGS wire-format version. */
#define RG_RGS_VERSION 1u
/** Encoded file header size in bytes. */
#define RG_RGS_HEADER_SIZE 12u
/** Encoded frame header size in bytes. */
#define RG_RGS_FRAME_HEADER_SIZE 8u
/** Maximum supported channel count. */
#define RG_RGS_MAX_CHANNELS 8u
/** Maximum accepted input sample rate. */
#define RG_RGS_MAX_SAMPLERATE 44100u
/** Normative maximum rate stored in an RGS v1 stream. */
#define RG_RGS_MAX_STORED_SAMPLERATE 44100u
/** Maximum decoded sample frames returned by one streaming call. */
#define RG_RGS_MAX_FRAME_SAMPLES 5120u
/** @} */

#define RG_RGS_FILE_FLAG_VBR 0x1u
#define RG_RGS_FILE_FLAG_ALL_2BIT_FRAMES 0x2u
#define RG_RGS_FILE_FLAG_PLANAR_FRAMES 0x4u
#define RG_RGS_FILE_FLAGS \
	(RG_RGS_FILE_FLAG_VBR | RG_RGS_FILE_FLAG_ALL_2BIT_FRAMES | RG_RGS_FILE_FLAG_PLANAR_FRAMES)
#define RG_RGS_FRAME_FLAG_MIXED 0x1u
#define RG_RGS_FRAME_FLAG_ALL_2BIT 0x2u
#define RG_RGS_FRAME_FLAG_PLANAR 0x4u
#define RG_RGS_SLICE_LEN 20u
#define RG_RGS_SLICES_PER_FRAME 256u
#define RG_RGS_LMS_LEN 4u
#define RG_RGS_FRAME_SIZE(channels, slices) \
	(RG_RGS_FRAME_HEADER_SIZE + RG_RGS_LMS_LEN * 4u * (channels) + 8u * (slices) * (channels))
#define RG_RGS_PLANAR_MODE_BYTES(slices) (((slices) + 7u) / 8u)

/** Metadata stored in an RGS stream. Sample count is per channel. */
typedef struct RgRgsInfo
{
	uint32_t channels;
	uint32_t samplerate;
	uint32_t samples;
} RgRgsInfo;

/** Encoder quality/size tradeoff. */
typedef enum RgRgsQuality
{
	RG_RGS_QUALITY_HIGH = 0,
	RG_RGS_QUALITY_MEDIUM = 1,
	RG_RGS_QUALITY_LOW = 2
} RgRgsQuality;

/** Encoder configuration. A zero bitrate hint leaves quality unchanged. */
typedef struct RgRgsEncodeOptions
{
	RgRgsQuality quality;
	uint32_t target_kbps;
} RgRgsEncodeOptions;

/** Result of one allocation-free streaming decode operation. */
typedef enum RgRgsDecodeStatus
{
	RG_RGS_DECODE_INVALID = -1,
	RG_RGS_DECODE_END = 0,
	RG_RGS_DECODE_FRAME = 1,
	RG_RGS_DECODE_OUTPUT_TOO_SMALL = 2
} RgRgsDecodeStatus;

/**
 * Borrowing streaming decoder state.
 *
 * The encoded buffer supplied at initialization must remain immutable and
 * alive until this decoder is no longer used.
 */
typedef struct RgRgsDecoder
{
	const uint8_t* data;
	size_t size;
	size_t offset;
	uint32_t decoded_frames;
	RgRgsInfo info;
	int failed;
} RgRgsDecoder;

/* Private codec state types. */
typedef struct RgRgsLms
{
	int history[RG_RGS_LMS_LEN];
	int weights[RG_RGS_LMS_LEN];
} RgRgsLms;

typedef struct RgRgsSliceCandidate
{
	uint64_t bits;
	uint64_t rank;
	RgRgsLms lms;
} RgRgsSliceCandidate;

/** Return the medium-quality, no-bitrate-hint encoder options. */
RGINLINE RgRgsEncodeOptions rg_rgs_default_options(void);

/**
 * Return a safe encoded byte capacity for the input description.
 *
 * @return Required capacity, or zero for an invalid description/overflow.
 */
RGINLINE size_t rg_rgs_encode_bound(uint32_t samples, uint32_t channels, uint32_t samplerate);

/** Encode interleaved PCM16 at 1..44100 Hz using default options. No allocation. */
RGINLINE size_t rg_rgs_encode_s16(const int16_t* pcm,
                                  uint32_t samples,
                                  uint32_t channels,
                                  uint32_t samplerate,
                                  void* dst,
                                  size_t dst_size);

/** Encode interleaved PCM16 at 1..44100 Hz using explicit options. No allocation. */
RGINLINE size_t rg_rgs_encode_s16_ex(const int16_t* pcm,
                                     uint32_t samples,
                                     uint32_t channels,
                                     uint32_t samplerate,
                                     void* dst,
                                     size_t dst_size,
                                     const RgRgsEncodeOptions* options);

/** Read and validate the v1 file header and complete first-frame structure. */
RGINLINE int rg_rgs_read_header(const void* src, size_t src_size, RgRgsInfo* out_info);

/**
 * Validate and decode an entire RGS stream.
 *
 * @param dst_samples Destination capacity in total int16_t values.
 * @return Total int16_t values written, or zero on failure.
 */
RGINLINE size_t rg_rgs_decode_s16(const void* src,
                                  size_t src_size,
                                  int16_t* dst,
                                  size_t dst_samples,
                                  RgRgsInfo* out_info);

/**
 * Decode a complete stream that has already passed checked decoding.
 *
 * Malformed or attacker-controlled bytes must use rg_rgs_decode_s16 instead.
 */
RGINLINE size_t rg_rgs_decode_trusted_s16(const void* src,
                                          size_t src_size,
                                          int16_t* dst,
                                          size_t dst_samples,
                                          RgRgsInfo* out_info);

/** Initialize a decoder that borrows the complete encoded buffer. */
RGINLINE int rg_rgs_decoder_init(RgRgsDecoder* decoder,
                                 const void* src,
                                 size_t src_size,
                                 RgRgsInfo* out_info);

/** Revalidate the header, clear poison state, and rewind to the first frame. */
RGINLINE void rg_rgs_decoder_reset(RgRgsDecoder* decoder);

/**
 * Validate and decode the next complete frame without allocation.
 *
 * @param dst_samples Destination capacity in total int16_t values.
 * @param out_frames Receives required/decoded sample frames per channel.
 */
RGINLINE RgRgsDecodeStatus rg_rgs_decoder_next_s16(RgRgsDecoder* decoder,
                                                   int16_t* dst,
                                                   size_t dst_samples,
                                                   uint32_t* out_frames);

static const int rg_rgs_quant_tab[17] = {
    7, 7, 7, 5, 5, 3, 3, 1,
    0,
    0, 2, 2, 4, 4, 6, 6, 6};

static const int rg_rgs_scalefactor_tab[16] = {
    1, 7, 21, 45, 84, 138, 211, 304,
    421, 562, 731, 928, 1157, 1419, 1715, 2048};

static const int rg_rgs_reciprocal_tab[16] = {
    65536, 9363, 3121, 1457, 781, 475, 311, 216,
    156, 117, 90, 71, 57, 47, 39, 32};

static const int rg_rgs_dequant_tab[16][8] = {
    {1, -1, 3, -3, 5, -5, 7, -7},
    {5, -5, 18, -18, 32, -32, 49, -49},
    {16, -16, 53, -53, 95, -95, 147, -147},
    {34, -34, 113, -113, 203, -203, 315, -315},
    {63, -63, 210, -210, 378, -378, 588, -588},
    {104, -104, 345, -345, 621, -621, 966, -966},
    {158, -158, 528, -528, 950, -950, 1477, -1477},
    {228, -228, 760, -760, 1368, -1368, 2128, -2128},
    {316, -316, 1053, -1053, 1895, -1895, 2947, -2947},
    {422, -422, 1405, -1405, 2529, -2529, 3934, -3934},
    {548, -548, 1828, -1828, 3290, -3290, 5117, -5117},
    {696, -696, 2320, -2320, 4176, -4176, 6496, -6496},
    {868, -868, 2893, -2893, 5207, -5207, 8099, -8099},
    {1064, -1064, 3548, -3548, 6386, -6386, 9933, -9933},
    {1286, -1286, 4288, -4288, 7718, -7718, 12005, -12005},
    {1536, -1536, 5120, -5120, 9216, -9216, 14336, -14336}};

static const int rg_rgs_dequant2_tab[16][4] = {
    {1, -1, 5, -5},
    {5, -5, 32, -32},
    {16, -16, 95, -95},
    {34, -34, 203, -203},
    {63, -63, 378, -378},
    {104, -104, 621, -621},
    {158, -158, 950, -950},
    {228, -228, 1368, -1368},
    {316, -316, 1895, -1895},
    {422, -422, 2529, -2529},
    {548, -548, 3290, -3290},
    {696, -696, 4176, -4176},
    {868, -868, 5207, -5207},
    {1064, -1064, 6386, -6386},
    {1286, -1286, 7718, -7718},
    {1536, -1536, 9216, -9216}};

RGINLINE uint16_t rg_rgs_read_u16le(const uint8_t* p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}

RGINLINE uint32_t rg_rgs_read_u24le(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u);
}

RGINLINE uint32_t rg_rgs_read_u32le(const uint8_t* p)
{
	return (uint32_t)p[0] |
	       ((uint32_t)p[1] << 8u) |
	       ((uint32_t)p[2] << 16u) |
	       ((uint32_t)p[3] << 24u);
}

RGINLINE uint64_t rg_rgs_read_u64le(const uint8_t* p)
{
	return (uint64_t)p[0] |
	       ((uint64_t)p[1] << 8u) |
	       ((uint64_t)p[2] << 16u) |
	       ((uint64_t)p[3] << 24u) |
	       ((uint64_t)p[4] << 32u) |
	       ((uint64_t)p[5] << 40u) |
	       ((uint64_t)p[6] << 48u) |
	       ((uint64_t)p[7] << 56u);
}

RGINLINE uint64_t rg_rgs_read_u48le(const uint8_t* p)
{
	return (uint64_t)p[0] |
	       ((uint64_t)p[1] << 8u) |
	       ((uint64_t)p[2] << 16u) |
	       ((uint64_t)p[3] << 24u) |
	       ((uint64_t)p[4] << 32u) |
	       ((uint64_t)p[5] << 40u);
}

RGINLINE void rg_rgs_write_u16le(uint8_t* p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8u) & 0xffu);
}

RGINLINE void rg_rgs_write_u24le(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8u) & 0xffu);
	p[2] = (uint8_t)((v >> 16u) & 0xffu);
}

RGINLINE void rg_rgs_write_u32le(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8u) & 0xffu);
	p[2] = (uint8_t)((v >> 16u) & 0xffu);
	p[3] = (uint8_t)((v >> 24u) & 0xffu);
}

RGINLINE void rg_rgs_write_u64le(uint8_t* p, uint64_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8u) & 0xffu);
	p[2] = (uint8_t)((v >> 16u) & 0xffu);
	p[3] = (uint8_t)((v >> 24u) & 0xffu);
	p[4] = (uint8_t)((v >> 32u) & 0xffu);
	p[5] = (uint8_t)((v >> 40u) & 0xffu);
	p[6] = (uint8_t)((v >> 48u) & 0xffu);
	p[7] = (uint8_t)((v >> 56u) & 0xffu);
}

RGINLINE void rg_rgs_write_u48le(uint8_t* p, uint64_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8u) & 0xffu);
	p[2] = (uint8_t)((v >> 16u) & 0xffu);
	p[3] = (uint8_t)((v >> 24u) & 0xffu);
	p[4] = (uint8_t)((v >> 32u) & 0xffu);
	p[5] = (uint8_t)((v >> 40u) & 0xffu);
}

RGINLINE int rg_rgs_sign_extend_u16(uint16_t value)
{
	return value < 0x8000u ? (int)value : (int)value - 0x10000;
}

RGINLINE int rg_rgs_clamp(int v, int min_v, int max_v)
{
	if (v < min_v)
	{
		return min_v;
	}
	if (v > max_v)
	{
		return max_v;
	}
	return v;
}

RGINLINE int rg_rgs_clamp_s16(int v)
{
	/* The common in-range case needs one comparison. Add after converting
	 * to unsigned so even INT_MIN/INT_MAX cannot overflow signed arithmetic. */
	if ((unsigned int)v + 32768u > 65535u)
	{
		return v < 0 ? -32768 : 32767;
	}
	return v;
}

RGINLINE int64_t rg_rgs_floor_div_pow2_i64(int64_t value, uint32_t shift)
{
	/* Defined for shifts 0..63, including INT64_MIN. Bias the signed range
	 * into unsigned order, divide there, then remove the divided bias.
	 * For a positive shift the quotient fits int64_t before conversion.
	 * This avoids a sample-dependent branch and negative signed shifts. */
	if (shift == 0u)
	{
		return value;
	}
	return (int64_t)(((uint64_t)value ^ (UINT64_C(1) << 63u)) >> shift) -
	       (INT64_C(1) << (63u - shift));
}

RGINLINE int rg_rgs_lms_predict(const RgRgsLms* lms)
{
	int64_t prediction = 0;
	for (uint32_t i = 0u; i < RG_RGS_LMS_LEN; i++)
	{
		prediction += (int64_t)lms->weights[i] * (int64_t)lms->history[i];
	}
	return (int)rg_rgs_floor_div_pow2_i64(prediction, 13u);
}

RGINLINE void rg_rgs_lms_update(RgRgsLms* lms, int sample, int residual)
{
	int delta = (int)rg_rgs_floor_div_pow2_i64((int64_t)residual, 4u);
	for (uint32_t i = 0u; i < RG_RGS_LMS_LEN; i++)
	{
		lms->weights[i] += lms->history[i] < 0 ? -delta : delta;
	}
	for (uint32_t i = 0u; i < RG_RGS_LMS_LEN - 1u; i++)
	{
		lms->history[i] = lms->history[i + 1u];
	}
	lms->history[RG_RGS_LMS_LEN - 1u] = sample;
}

RGINLINE int rg_rgs_div(int v, int scalefactor)
{
	int reciprocal = rg_rgs_reciprocal_tab[scalefactor];
	int n = (int)rg_rgs_floor_div_pow2_i64((int64_t)v * reciprocal + ((int64_t)1 << 15), 16u);
	n = n + ((v > 0) - (v < 0)) - ((n > 0) - (n < 0));
	return n;
}

RGINLINE int rg_rgs_valid_desc(uint32_t samples, uint32_t channels, uint32_t samplerate)
{
	return samples != 0u &&
	       channels != 0u &&
	       channels <= RG_RGS_MAX_CHANNELS &&
	       samplerate != 0u &&
	       samplerate <= RG_RGS_MAX_SAMPLERATE;
}

RGINLINE RgRgsEncodeOptions rg_rgs_default_options(void)
{
	RgRgsEncodeOptions options;
	options.quality = RG_RGS_QUALITY_MEDIUM;
	options.target_kbps = 0u;
	return options;
}

RGINLINE int rg_rgs_options_valid(const RgRgsEncodeOptions* options)
{
	return options == NULL ||
	       ((int)options->quality >= (int)RG_RGS_QUALITY_HIGH &&
	        (int)options->quality <= (int)RG_RGS_QUALITY_LOW);
}

RGINLINE RgRgsEncodeOptions rg_rgs_resolve_options(const RgRgsEncodeOptions* options)
{
	RgRgsEncodeOptions resolved = rg_rgs_default_options();
	if (options != NULL)
	{
		resolved = *options;
	}
	if (resolved.target_kbps != 0u)
	{
		if (resolved.target_kbps <= 160u)
		{
			resolved.quality = RG_RGS_QUALITY_LOW;
		}
		else if (resolved.target_kbps <= 224u && resolved.quality == RG_RGS_QUALITY_HIGH)
		{
			resolved.quality = RG_RGS_QUALITY_MEDIUM;
		}
	}
	return resolved;
}

RGINLINE int rg_rgs_promoted_ratio_percent(RgRgsQuality quality)
{
	if (quality == RG_RGS_QUALITY_LOW)
	{
		return 360;
	}
	if (quality == RG_RGS_QUALITY_MEDIUM)
	{
		return 240;
	}
	return 100;
}

RGINLINE uint64_t rg_rgs_promoted_abs_error(RgRgsQuality quality, uint32_t slice_len)
{
	uint64_t per_sample = 0u;
	if (quality == RG_RGS_QUALITY_LOW)
	{
		per_sample = 1024u * 1024u;
	}
	else if (quality == RG_RGS_QUALITY_MEDIUM)
	{
		per_sample = 448u * 448u;
	}
	return per_sample * slice_len;
}

RGINLINE int rg_rgs_accept_2bit(uint64_t rank2,
                                uint64_t rank3,
                                uint32_t slice_len,
                                const RgRgsEncodeOptions* options)
{
	RgRgsEncodeOptions resolved = rg_rgs_resolve_options(options);
	uint64_t scaled;
	uint64_t limit;
	if (resolved.quality == RG_RGS_QUALITY_HIGH)
	{
		return 0;
	}
	scaled = rank3 * (uint64_t)rg_rgs_promoted_ratio_percent(resolved.quality);
	limit = scaled / 100u + rg_rgs_promoted_abs_error(resolved.quality, slice_len);
	return rank2 <= limit;
}

RGINLINE uint64_t rg_rgs_weights_penalty_sq(const RgRgsLms* lms)
{
	int64_t sum = 0;
	int64_t penalty;
	for (uint32_t i = 0u; i < RG_RGS_LMS_LEN; i++)
	{
		sum += (int64_t)lms->weights[i] * (int64_t)lms->weights[i];
	}
	penalty = sum / ((int64_t)1 << 18) - 0x8ff;
	if (penalty <= 0)
	{
		return 0u;
	}
	return (uint64_t)penalty * (uint64_t)penalty;
}

RGINLINE int rg_rgs_encode_sample_2bit(int sample,
                                       int predicted,
                                       int scalefactor,
                                       int* out_quantized,
                                       int* out_dequantized,
                                       int* out_reconstructed)
{
	int best_q = 0;
	int best_dequantized = rg_rgs_dequant2_tab[scalefactor][0];
	int best_reconstructed = rg_rgs_clamp_s16(predicted + best_dequantized);
	int64_t best_error = (int64_t)sample - (int64_t)best_reconstructed;
	uint64_t best_error_sq = (uint64_t)(best_error * best_error);

	for (int q = 1; q < 4; q++)
	{
		int dequantized = rg_rgs_dequant2_tab[scalefactor][q];
		int reconstructed = rg_rgs_clamp_s16(predicted + dequantized);
		int64_t error = (int64_t)sample - (int64_t)reconstructed;
		uint64_t error_sq = (uint64_t)(error * error);
		if (error_sq < best_error_sq)
		{
			best_q = q;
			best_dequantized = dequantized;
			best_reconstructed = reconstructed;
			best_error_sq = error_sq;
		}
	}

	*out_quantized = best_q;
	*out_dequantized = best_dequantized;
	*out_reconstructed = best_reconstructed;
	return 1;
}

RGINLINE RgRgsSliceCandidate rg_rgs_encode_slice3(const int16_t* sample_data,
                                                  uint32_t channels,
                                                  uint32_t sample_index,
                                                  uint32_t slice_len,
                                                  uint32_t c,
                                                  const RgRgsLms* lms,
                                                  int prev_scalefactor)
{
	RgRgsSliceCandidate best;
	best.rank = (uint64_t)-1;
	best.bits = 0u;
	best.lms = *lms;

	for (int sfi = 0; sfi < 16; sfi++)
	{
		int scalefactor = (sfi + prev_scalefactor) & 15;
		RgRgsLms trial_lms = *lms;
		uint64_t slice = (uint64_t)scalefactor;
		uint64_t current_rank = 0u;
		uint32_t bitpos = 4u;

		for (uint32_t si = 0u; si < slice_len; si++)
		{
			int sample = sample_data[(sample_index + si) * channels + c];
			int predicted = rg_rgs_lms_predict(&trial_lms);
			int residual = sample - predicted;
			int scaled = rg_rgs_div(residual, scalefactor);
			int clamped = rg_rgs_clamp(scaled, -8, 8);
			int quantized = rg_rgs_quant_tab[clamped + 8];
			int dequantized = rg_rgs_dequant_tab[scalefactor][quantized];
			int reconstructed = rg_rgs_clamp_s16(predicted + dequantized);
			int64_t error;
			uint64_t error_sq;

			error = (int64_t)sample - (int64_t)reconstructed;
			error_sq = (uint64_t)(error * error);
			current_rank += error_sq + rg_rgs_weights_penalty_sq(&trial_lms);
			if (current_rank > best.rank)
			{
				break;
			}

			slice |= ((uint64_t)quantized & 7u) << bitpos;
			bitpos += 3u;
			rg_rgs_lms_update(&trial_lms, reconstructed, dequantized);
		}

		if (current_rank < best.rank)
		{
			best.rank = current_rank;
			best.bits = slice;
			best.lms = trial_lms;
		}
	}

	return best;
}

RGINLINE RgRgsSliceCandidate rg_rgs_encode_slice2(const int16_t* sample_data,
                                                  uint32_t channels,
                                                  uint32_t sample_index,
                                                  uint32_t slice_len,
                                                  uint32_t c,
                                                  const RgRgsLms* lms,
                                                  int prev_scalefactor)
{
	RgRgsSliceCandidate best;
	best.rank = (uint64_t)-1;
	best.bits = 0u;
	best.lms = *lms;

	for (int sfi = 0; sfi < 16; sfi++)
	{
		int scalefactor = (sfi + prev_scalefactor) & 15;
		RgRgsLms trial_lms = *lms;
		uint64_t slice = (uint64_t)scalefactor;
		uint64_t current_rank = 0u;
		uint32_t bitpos = 4u;

		for (uint32_t si = 0u; si < slice_len; si++)
		{
			int sample = sample_data[(sample_index + si) * channels + c];
			int predicted = rg_rgs_lms_predict(&trial_lms);
			int quantized = 0;
			int dequantized = 0;
			int reconstructed = 0;
			int64_t error;
			uint64_t error_sq;

			rg_rgs_encode_sample_2bit(sample,
			                          predicted,
			                          scalefactor,
			                          &quantized,
			                          &dequantized,
			                          &reconstructed);
			error = (int64_t)sample - (int64_t)reconstructed;
			error_sq = (uint64_t)(error * error);
			current_rank += error_sq + rg_rgs_weights_penalty_sq(&trial_lms);
			if (current_rank > best.rank)
			{
				break;
			}

			slice |= ((uint64_t)quantized & 3u) << bitpos;
			bitpos += 2u;
			rg_rgs_lms_update(&trial_lms, reconstructed, dequantized);
		}

		if (current_rank < best.rank)
		{
			best.rank = current_rank;
			best.bits = slice;
			best.lms = trial_lms;
		}
	}

	return best;
}

RGINLINE size_t rg_rgs_encode_frame_planar_fixed(const int16_t* sample_data,
                                                 uint32_t channels,
                                                 uint32_t samplerate,
                                                 uint32_t frame_len,
                                                 RgRgsLms* lms,
                                                 uint8_t* bytes)
{
	uint32_t slices = (frame_len + RG_RGS_SLICE_LEN - 1u) / RG_RGS_SLICE_LEN;
	size_t frame_size = RG_RGS_FRAME_SIZE(channels, slices);
	size_t p = 0u;
	int prev_scalefactor[RG_RGS_MAX_CHANNELS] = {0};

	bytes[p++] = (uint8_t)(channels | (RG_RGS_FRAME_FLAG_PLANAR << 4u));
	rg_rgs_write_u24le(bytes + p, samplerate);
	p += 3u;
	rg_rgs_write_u16le(bytes + p, (uint16_t)frame_len);
	p += 2u;
	rg_rgs_write_u16le(bytes + p, (uint16_t)frame_size);
	p += 2u;

	for (uint32_t c = 0u; c < channels; c++)
	{
		for (uint32_t i = 0u; i < RG_RGS_LMS_LEN; i++)
		{
			rg_rgs_write_u16le(bytes + p, (uint16_t)((uint32_t)lms[c].history[i] & 0xffffu));
			p += 2u;
		}
		for (uint32_t i = 0u; i < RG_RGS_LMS_LEN; i++)
		{
			rg_rgs_write_u16le(bytes + p, (uint16_t)((uint32_t)lms[c].weights[i] & 0xffffu));
			p += 2u;
		}
	}

	for (uint32_t c = 0u; c < channels; c++)
	{
		for (uint32_t sample_index = 0u; sample_index < frame_len; sample_index += RG_RGS_SLICE_LEN)
		{
			uint32_t slice_len = frame_len - sample_index;
			if (slice_len > RG_RGS_SLICE_LEN)
			{
				slice_len = RG_RGS_SLICE_LEN;
			}

			RgRgsSliceCandidate slice3 = rg_rgs_encode_slice3(sample_data,
			                                                  channels,
			                                                  sample_index,
			                                                  slice_len,
			                                                  c,
			                                                  &lms[c],
			                                                  prev_scalefactor[c]);
			rg_rgs_write_u64le(bytes + p, slice3.bits);
			p += 8u;
			lms[c] = slice3.lms;
			prev_scalefactor[c] = (int)(slice3.bits & 0xfu);
		}
	}

	return p;
}

RGINLINE size_t rg_rgs_encode_frame_planar_mixed(const int16_t* sample_data,
                                                 uint32_t channels,
                                                 uint32_t samplerate,
                                                 uint32_t frame_len,
                                                 RgRgsLms* lms,
                                                 uint8_t* bytes,
                                                 const RgRgsEncodeOptions* options)
{
	uint32_t slices = (frame_len + RG_RGS_SLICE_LEN - 1u) / RG_RGS_SLICE_LEN;
	uint32_t mode_bytes = RG_RGS_PLANAR_MODE_BYTES(slices);
	size_t fixed_size = RG_RGS_FRAME_SIZE(channels, slices);
	size_t p = 0u;
	size_t mode_map_start;
	RgRgsLms initial_lms[RG_RGS_MAX_CHANNELS];
	int prev_scalefactor[RG_RGS_MAX_CHANNELS] = {0};
	size_t two_bit_count = 0u;

	if (rg_rgs_resolve_options(options).quality == RG_RGS_QUALITY_HIGH)
	{
		return rg_rgs_encode_frame_planar_fixed(sample_data, channels, samplerate, frame_len, lms, bytes);
	}

	memcpy(initial_lms, lms, sizeof(RgRgsLms) * channels);

	bytes[p++] = (uint8_t)(channels | ((RG_RGS_FRAME_FLAG_PLANAR | RG_RGS_FRAME_FLAG_MIXED) << 4u));
	rg_rgs_write_u24le(bytes + p, samplerate);
	p += 3u;
	rg_rgs_write_u16le(bytes + p, (uint16_t)frame_len);
	p += 2u;
	p += 2u;

	for (uint32_t c = 0u; c < channels; c++)
	{
		for (uint32_t i = 0u; i < RG_RGS_LMS_LEN; i++)
		{
			rg_rgs_write_u16le(bytes + p, (uint16_t)((uint32_t)lms[c].history[i] & 0xffffu));
			p += 2u;
		}
		for (uint32_t i = 0u; i < RG_RGS_LMS_LEN; i++)
		{
			rg_rgs_write_u16le(bytes + p, (uint16_t)((uint32_t)lms[c].weights[i] & 0xffffu));
			p += 2u;
		}
	}

	mode_map_start = p;
	memset(bytes + p, 0, (size_t)channels * mode_bytes);
	p += (size_t)channels * mode_bytes;

	for (uint32_t c = 0u; c < channels; c++)
	{
		for (uint32_t s = 0u; s < slices; s++)
		{
			uint32_t sample_index = s * RG_RGS_SLICE_LEN;
			uint32_t slice_len = frame_len - sample_index;
			if (slice_len > RG_RGS_SLICE_LEN)
			{
				slice_len = RG_RGS_SLICE_LEN;
			}

			RgRgsSliceCandidate slice3 = rg_rgs_encode_slice3(sample_data,
			                                                  channels,
			                                                  sample_index,
			                                                  slice_len,
			                                                  c,
			                                                  &lms[c],
			                                                  prev_scalefactor[c]);
			RgRgsSliceCandidate slice2 = rg_rgs_encode_slice2(sample_data,
			                                                  channels,
			                                                  sample_index,
			                                                  slice_len,
			                                                  c,
			                                                  &lms[c],
			                                                  prev_scalefactor[c]);
			if (rg_rgs_accept_2bit(slice2.rank, slice3.rank, slice_len, options))
			{
				bytes[mode_map_start + (size_t)c * mode_bytes + (s >> 3u)] |= (uint8_t)(1u << (s & 7u));
				rg_rgs_write_u48le(bytes + p, slice2.bits);
				p += 6u;
				lms[c] = slice2.lms;
				prev_scalefactor[c] = (int)(slice2.bits & 0xfu);
				two_bit_count++;
			}
			else
			{
				rg_rgs_write_u64le(bytes + p, slice3.bits);
				p += 8u;
				lms[c] = slice3.lms;
				prev_scalefactor[c] = (int)(slice3.bits & 0xfu);
			}
		}
	}

	if (two_bit_count == 0u || two_bit_count == (size_t)slices * channels)
	{
		/* The chosen payload and final LMS state already match the fixed-width
		 * representation. Only the now-redundant mode maps need removing. */
		size_t map_size = (size_t)channels * mode_bytes;
		memmove(bytes + mode_map_start, bytes + mode_map_start + map_size,
		        p - mode_map_start - map_size);
		p -= map_size;
		bytes[0] = (uint8_t)(channels | ((RG_RGS_FRAME_FLAG_PLANAR |
		    (two_bit_count != 0u ? RG_RGS_FRAME_FLAG_ALL_2BIT : 0u)) << 4u));
	}
	else if (p >= fixed_size)
	{
		memcpy(lms, initial_lms, sizeof(RgRgsLms) * channels);
		return rg_rgs_encode_frame_planar_fixed(sample_data, channels, samplerate, frame_len, lms, bytes);
	}

	rg_rgs_write_u16le(bytes + 6u, (uint16_t)p);
	return p;
}

RGINLINE void rg_rgs_decode_packed_slice(uint64_t slice,
                                         uint32_t bits_per_sample,
                                         RgRgsLms* lms,
                                         int16_t* dst,
                                         uint32_t channels,
                                         uint32_t c,
                                         uint32_t sample_index,
                                         uint32_t slice_len)
{
	int scalefactor = (int)(slice & 0xfu);
	int16_t* out = dst + (size_t)sample_index * channels + c;
#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64)
	/* Keep the four taps in local wide scalars for the whole slice. Besides
	 * avoiding repeated state loads/stores, this avoids sign-extending each
	 * tap before every wide multiply. Frame headers reset the weights, so
	 * their magnitude stays <= 32768 + 5120 * 896 = 4620288. The wide dot
	 * product remains necessary for arbitrary valid serialized states. */
	int64_t h0 = lms->history[0];
	int64_t h1 = lms->history[1];
	int64_t h2 = lms->history[2];
	int64_t h3 = lms->history[3];
	int64_t w0 = lms->weights[0];
	int64_t w1 = lms->weights[1];
	int64_t w2 = lms->weights[2];
	int64_t w3 = lms->weights[3];
	slice >>= 4u;
	/* Select the table and constant shift once per slice, outside the LMS
	 * dependency chain. Both paths use the same defined wide arithmetic. */
	if (bits_per_sample == 3u)
	{
		const int* dequant = rg_rgs_dequant_tab[scalefactor];
		for (uint32_t si = 0u; si < slice_len; si++)
		{
			int64_t prediction = w0 * h0 + w1 * h1 + w2 * h2 + w3 * h3;
			int predicted = (int)rg_rgs_floor_div_pow2_i64(prediction, 13u);
			int dequantized = dequant[slice & 7u];
			int reconstructed = rg_rgs_clamp_s16(predicted + dequantized);
			int delta = (int)rg_rgs_floor_div_pow2_i64(dequantized, 4u);
			out[(size_t)si * channels] = (int16_t)reconstructed;
			slice >>= 3u;
			w0 += h0 < 0 ? -delta : delta;
			w1 += h1 < 0 ? -delta : delta;
			w2 += h2 < 0 ? -delta : delta;
			w3 += h3 < 0 ? -delta : delta;
			h0 = h1;
			h1 = h2;
			h2 = h3;
			h3 = reconstructed;
		}
	}
	else
	{
		const int* dequant = rg_rgs_dequant2_tab[scalefactor];
		for (uint32_t si = 0u; si < slice_len; si++)
		{
			int64_t prediction = w0 * h0 + w1 * h1 + w2 * h2 + w3 * h3;
			int predicted = (int)rg_rgs_floor_div_pow2_i64(prediction, 13u);
			int dequantized = dequant[slice & 3u];
			int reconstructed = rg_rgs_clamp_s16(predicted + dequantized);
			int delta = (int)rg_rgs_floor_div_pow2_i64(dequantized, 4u);
			out[(size_t)si * channels] = (int16_t)reconstructed;
			slice >>= 2u;
			w0 += h0 < 0 ? -delta : delta;
			w1 += h1 < 0 ? -delta : delta;
			w2 += h2 < 0 ? -delta : delta;
			w3 += h3 < 0 ? -delta : delta;
			h0 = h1;
			h1 = h2;
			h2 = h3;
			h3 = reconstructed;
		}
	}
	lms->history[0] = (int)h0;
	lms->history[1] = (int)h1;
	lms->history[2] = (int)h2;
	lms->history[3] = (int)h3;
	lms->weights[0] = (int)w0;
	lms->weights[1] = (int)w1;
	lms->weights[2] = (int)w2;
	lms->weights[3] = (int)w3;
#else
	/* Keep the array form on other compilers: LLVM can vectorize all four
	 * weight updates together, while explicit wide scalars prevent that.
	 * Both kernels retain wide products and exactly the same LMS updates. */
	slice >>= 4u;
	if (bits_per_sample == 3u)
	{
		const int* dequant = rg_rgs_dequant_tab[scalefactor];
		for (uint32_t si = 0u; si < slice_len; si++)
		{
			int predicted = rg_rgs_lms_predict(lms);
			int dequantized = dequant[slice & 7u];
			int reconstructed = rg_rgs_clamp_s16(predicted + dequantized);
			out[(size_t)si * channels] = (int16_t)reconstructed;
			slice >>= 3u;
			rg_rgs_lms_update(lms, reconstructed, dequantized);
		}
	}
	else
	{
		const int* dequant = rg_rgs_dequant2_tab[scalefactor];
		for (uint32_t si = 0u; si < slice_len; si++)
		{
			int predicted = rg_rgs_lms_predict(lms);
			int dequantized = dequant[slice & 3u];
			int reconstructed = rg_rgs_clamp_s16(predicted + dequantized);
			out[(size_t)si * channels] = (int16_t)reconstructed;
			slice >>= 2u;
			rg_rgs_lms_update(lms, reconstructed, dequantized);
		}
	}
#endif
}

typedef struct RgRgsFrameMeta
{
	uint32_t flags;
	uint32_t samples;
	uint32_t slices;
	uint32_t mode_bytes;
	uint32_t frame_size;
	size_t payload_offset;
} RgRgsFrameMeta;

RGINLINE int rg_rgs_valid_frame_flags(uint32_t flags)
{
	return flags == RG_RGS_FRAME_FLAG_PLANAR ||
	       flags == (RG_RGS_FRAME_FLAG_PLANAR | RG_RGS_FRAME_FLAG_MIXED) ||
	       flags == (RG_RGS_FRAME_FLAG_PLANAR | RG_RGS_FRAME_FLAG_ALL_2BIT);
}

RGINLINE int rg_rgs_parse_frame(const uint8_t* bytes,
                                size_t size,
                                const RgRgsInfo* info,
                                uint32_t remaining,
                                RgRgsFrameMeta* out_meta)
{
	uint32_t channels;
	uint32_t flags;
	uint32_t samplerate;
	uint32_t samples;
	uint32_t frame_size;
	uint32_t slices;
	uint32_t expected_samples;
	size_t header_size;
	size_t expected_size;
	uint32_t mode_bytes = 0u;

	if (bytes == NULL || info == NULL || out_meta == NULL || size < RG_RGS_FRAME_HEADER_SIZE)
	{
		return 0;
	}

	channels = bytes[0] & 0x0fu;
	flags = bytes[0] >> 4u;
	samplerate = rg_rgs_read_u24le(bytes + 1u);
	samples = rg_rgs_read_u16le(bytes + 4u);
	frame_size = rg_rgs_read_u16le(bytes + 6u);
	expected_samples = remaining > RG_RGS_MAX_FRAME_SAMPLES ? RG_RGS_MAX_FRAME_SAMPLES : remaining;

	if (channels != info->channels ||
	    samplerate != info->samplerate ||
	    !rg_rgs_valid_frame_flags(flags) ||
	    samples == 0u ||
	    samples != expected_samples ||
	    frame_size > size)
	{
		return 0;
	}

	slices = (samples + RG_RGS_SLICE_LEN - 1u) / RG_RGS_SLICE_LEN;
	header_size = RG_RGS_FRAME_HEADER_SIZE + (size_t)RG_RGS_LMS_LEN * 4u * channels;
	if (frame_size < header_size)
	{
		return 0;
	}

	if (flags == RG_RGS_FRAME_FLAG_PLANAR)
	{
		expected_size = header_size + 8u * (size_t)slices * channels;
	}
	else if (flags == (RG_RGS_FRAME_FLAG_PLANAR | RG_RGS_FRAME_FLAG_ALL_2BIT))
	{
		expected_size = header_size + 6u * (size_t)slices * channels;
		if (expected_size != frame_size)
		{
			return 0;
		}
		for (size_t i = 0u; i < (size_t)slices * channels; i++)
		{
			if ((bytes[header_size + i * 6u + 5u] & 0xf0u) != 0u)
			{
				return 0;
			}
		}
	}
	else
	{
		uint32_t valid_last_bits;
		uint8_t unused_mask;
		size_t payload_offset;

		mode_bytes = RG_RGS_PLANAR_MODE_BYTES(slices);
		expected_size = header_size + (size_t)mode_bytes * channels;
		if (expected_size > frame_size)
		{
			return 0;
		}

		valid_last_bits = slices & 7u;
		unused_mask = valid_last_bits == 0u ? 0u : (uint8_t)(0xffu << valid_last_bits);
		payload_offset = expected_size;
		for (uint32_t c = 0u; c < channels; c++)
		{
			const uint8_t* map = bytes + header_size + (size_t)c * mode_bytes;
			if (unused_mask != 0u && (map[mode_bytes - 1u] & unused_mask) != 0u)
			{
				return 0;
			}
			for (uint32_t s = 0u; s < slices; s++)
			{
				uint32_t mode = (map[s >> 3u] >> (s & 7u)) & 1u;
				size_t slice_bytes = mode != 0u ? 6u : 8u;
				if (payload_offset + slice_bytes > frame_size)
				{
					return 0;
				}
				if (mode != 0u && (bytes[payload_offset + 5u] & 0xf0u) != 0u)
				{
					return 0;
				}
				payload_offset += slice_bytes;
			}
		}
		expected_size = payload_offset;
	}

	if (expected_size != frame_size)
	{
		return 0;
	}

	out_meta->flags = flags;
	out_meta->samples = samples;
	out_meta->slices = slices;
	out_meta->mode_bytes = mode_bytes;
	out_meta->frame_size = frame_size;
	out_meta->payload_offset = header_size;
	return 1;
}

#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64)
/* Overwrite the oldest physical history value after each sample. Passing
 * the histories in rotated order restores their logical order after four
 * samples, avoiding three history moves per sample in the main loop.
 * Prediction, weight updates, and floor division keep the same wide,
 * defined arithmetic and update order as the scalar decoder above. */
#define RG_RGS_MONO_STEP(H0, H1, H2, H3, WIDTH, MASK)                                           \
	do                                                                                        \
	{                                                                                         \
		int64_t prediction = w0 * (H0) + w1 * (H1) + w2 * (H2) + w3 * (H3);                   \
		int residual = dequant[slice & (MASK)];                                                \
		int reconstructed = rg_rgs_clamp_s16(                                                 \
		    (int)rg_rgs_floor_div_pow2_i64(prediction, 13u) + residual);                        \
		int64_t delta = (int)rg_rgs_floor_div_pow2_i64(residual, 4u);                           \
		*out++ = (int16_t)reconstructed;                                                       \
		slice >>= (WIDTH);                                                                    \
		w0 += (H0) < 0 ? -delta : delta;                                                       \
		w1 += (H1) < 0 ? -delta : delta;                                                       \
		w2 += (H2) < 0 ? -delta : delta;                                                       \
		w3 += (H3) < 0 ? -delta : delta;                                                       \
		(H0) = reconstructed;                                                                \
	} while (0)

RGINLINE void rg_rgs_decode_packed_slice_mono(uint64_t slice,
                                              uint32_t bits_per_sample,
                                              RgRgsLms* lms,
                                              int16_t* dst,
                                              uint32_t sample_index,
                                              uint32_t slice_len)
{
	int scalefactor = (int)(slice & 15u);
	int16_t* out = dst + sample_index;
	int64_t h0 = lms->history[0];
	int64_t h1 = lms->history[1];
	int64_t h2 = lms->history[2];
	int64_t h3 = lms->history[3];
	int64_t w0 = lms->weights[0];
	int64_t w1 = lms->weights[1];
	int64_t w2 = lms->weights[2];
	int64_t w3 = lms->weights[3];
	uint32_t si = 0;
	slice >>= 4u;
	if (bits_per_sample == 3u)
	{
		const int* dequant = rg_rgs_dequant_tab[scalefactor];
		for (; si + 4u <= slice_len; si += 4u)
		{
			RG_RGS_MONO_STEP(h0, h1, h2, h3, 3u, 7u);
			RG_RGS_MONO_STEP(h1, h2, h3, h0, 3u, 7u);
			RG_RGS_MONO_STEP(h2, h3, h0, h1, 3u, 7u);
			RG_RGS_MONO_STEP(h3, h0, h1, h2, 3u, 7u);
		}
		for (; si < slice_len; ++si)
		{
			int64_t tail;
			RG_RGS_MONO_STEP(h0, h1, h2, h3, 3u, 7u);
			tail = h0;
			h0 = h1;
			h1 = h2;
			h2 = h3;
			h3 = tail;
		}
	}
	else
	{
		const int* dequant = rg_rgs_dequant2_tab[scalefactor];
		for (; si + 4u <= slice_len; si += 4u)
		{
			RG_RGS_MONO_STEP(h0, h1, h2, h3, 2u, 3u);
			RG_RGS_MONO_STEP(h1, h2, h3, h0, 2u, 3u);
			RG_RGS_MONO_STEP(h2, h3, h0, h1, 2u, 3u);
			RG_RGS_MONO_STEP(h3, h0, h1, h2, 2u, 3u);
		}
		for (; si < slice_len; ++si)
		{
			int64_t tail;
			RG_RGS_MONO_STEP(h0, h1, h2, h3, 2u, 3u);
			tail = h0;
			h0 = h1;
			h1 = h2;
			h2 = h3;
			h3 = tail;
		}
	}
	lms->history[0] = (int)h0;
	lms->history[1] = (int)h1;
	lms->history[2] = (int)h2;
	lms->history[3] = (int)h3;
	lms->weights[0] = (int)w0;
	lms->weights[1] = (int)w1;
	lms->weights[2] = (int)w2;
	lms->weights[3] = (int)w3;
}

#undef RG_RGS_MONO_STEP

/* Keep mono and multichannel frame bodies out of their dispatch wrapper.
 * Inlining that dispatch into the multichannel body increases register
 * pressure and introduces extra spills in its per-sample loop on MSVC. */
static RG_NOINLINE int rg_rgs_decode_frame_mono(const uint8_t* bytes,
                                               const RgRgsFrameMeta* meta,
                                               int16_t* dst)
{
	RgRgsLms lms[1];
	size_t p = RG_RGS_FRAME_HEADER_SIZE;

	for (uint32_t c = 0u; c < 1u; c++)
	{
		for (uint32_t i = 0u; i < RG_RGS_LMS_LEN; i++)
		{
			lms[c].history[i] = rg_rgs_sign_extend_u16(rg_rgs_read_u16le(bytes + p));
			p += 2u;
		}
		for (uint32_t i = 0u; i < RG_RGS_LMS_LEN; i++)
		{
			lms[c].weights[i] = rg_rgs_sign_extend_u16(rg_rgs_read_u16le(bytes + p));
			p += 2u;
		}
	}

	{
		const uint8_t* mode_maps = NULL;
		if (meta->flags == (RG_RGS_FRAME_FLAG_PLANAR | RG_RGS_FRAME_FLAG_MIXED))
		{
			mode_maps = bytes + p;
			p += (size_t)meta->mode_bytes * 1u;
		}

		for (uint32_t c = 0u; c < 1u; c++)
		{
			for (uint32_t s = 0u; s < meta->slices; s++)
			{
				uint32_t sample_index = s * RG_RGS_SLICE_LEN;
				uint32_t slice_len = meta->samples - sample_index;
				uint32_t bits_per_sample = 3u;
				uint64_t slice;
				if (slice_len > RG_RGS_SLICE_LEN)
				{
					slice_len = RG_RGS_SLICE_LEN;
				}
				if (meta->flags == (RG_RGS_FRAME_FLAG_PLANAR | RG_RGS_FRAME_FLAG_ALL_2BIT) ||
				    (mode_maps != NULL &&
				     ((mode_maps[(size_t)c * meta->mode_bytes + (s >> 3u)] >> (s & 7u)) & 1u) != 0u))
				{
					bits_per_sample = 2u;
				}
				if (bits_per_sample == 2u)
				{
					slice = rg_rgs_read_u48le(bytes + p);
					p += 6u;
				}
				else
				{
					slice = rg_rgs_read_u64le(bytes + p);
					p += 8u;
				}
				rg_rgs_decode_packed_slice_mono(slice,
				                                bits_per_sample,
				                                &lms[c],
				                                dst,
				                                sample_index,
				                                slice_len);
			}
		}
	}

	return p == meta->frame_size;
}

static RG_NOINLINE int rg_rgs_decode_frame_multi(const uint8_t* bytes,
#else
RGINLINE int rg_rgs_decode_frame(const uint8_t* bytes,
#endif
                                 const RgRgsInfo* info,
                                 const RgRgsFrameMeta* meta,
                                 int16_t* dst)
{
	RgRgsLms lms[RG_RGS_MAX_CHANNELS];
	size_t p = RG_RGS_FRAME_HEADER_SIZE;

	for (uint32_t c = 0u; c < info->channels; c++)
	{
		for (uint32_t i = 0u; i < RG_RGS_LMS_LEN; i++)
		{
			lms[c].history[i] = rg_rgs_sign_extend_u16(rg_rgs_read_u16le(bytes + p));
			p += 2u;
		}
		for (uint32_t i = 0u; i < RG_RGS_LMS_LEN; i++)
		{
			lms[c].weights[i] = rg_rgs_sign_extend_u16(rg_rgs_read_u16le(bytes + p));
			p += 2u;
		}
	}

	{
		const uint8_t* mode_maps = NULL;
		if (meta->flags == (RG_RGS_FRAME_FLAG_PLANAR | RG_RGS_FRAME_FLAG_MIXED))
		{
			mode_maps = bytes + p;
			p += (size_t)meta->mode_bytes * info->channels;
		}

		for (uint32_t c = 0u; c < info->channels; c++)
		{
			for (uint32_t s = 0u; s < meta->slices; s++)
			{
				uint32_t sample_index = s * RG_RGS_SLICE_LEN;
				uint32_t slice_len = meta->samples - sample_index;
				uint32_t bits_per_sample = 3u;
				uint64_t slice;
				if (slice_len > RG_RGS_SLICE_LEN)
				{
					slice_len = RG_RGS_SLICE_LEN;
				}
				if (meta->flags == (RG_RGS_FRAME_FLAG_PLANAR | RG_RGS_FRAME_FLAG_ALL_2BIT) ||
				    (mode_maps != NULL &&
				     ((mode_maps[(size_t)c * meta->mode_bytes + (s >> 3u)] >> (s & 7u)) & 1u) != 0u))
				{
					bits_per_sample = 2u;
				}
				if (bits_per_sample == 2u)
				{
					slice = rg_rgs_read_u48le(bytes + p);
					p += 6u;
				}
				else
				{
					slice = rg_rgs_read_u64le(bytes + p);
					p += 8u;
				}
				rg_rgs_decode_packed_slice(slice,
				                           bits_per_sample,
				                           &lms[c],
				                           dst,
				                           info->channels,
				                           c,
				                           sample_index,
				                           slice_len);
			}
		}
	}

	return p == meta->frame_size;
}

#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64)
RGINLINE int rg_rgs_decode_frame(const uint8_t* bytes,
                                 const RgRgsInfo* info,
                                 const RgRgsFrameMeta* meta,
                                 int16_t* dst)
{
	if (info->channels == 1u)
	{
		return rg_rgs_decode_frame_mono(bytes, meta, dst);
	}
	return rg_rgs_decode_frame_multi(bytes, info, meta, dst);
}
#endif

RGINLINE size_t rg_rgs_encode_bound(uint32_t samples, uint32_t channels, uint32_t samplerate)
{
	uint64_t frames;
	uint64_t slices;
	uint64_t size;

	if (!rg_rgs_valid_desc(samples, channels, samplerate) ||
	    (uint64_t)samples * channels > (uint64_t)(size_t)-1 / sizeof(int16_t))
	{
		return 0u;
	}

	frames = ((uint64_t)samples + RG_RGS_MAX_FRAME_SAMPLES - 1u) / RG_RGS_MAX_FRAME_SAMPLES;
	slices = ((uint64_t)samples + RG_RGS_SLICE_LEN - 1u) / RG_RGS_SLICE_LEN;
	/* Include temporary mixed-frame maps even when compaction later removes
	 * them. Full frames contain 256 slices, an exact multiple of eight. */
	size = RG_RGS_HEADER_SIZE +
	       frames * (RG_RGS_FRAME_HEADER_SIZE + RG_RGS_LMS_LEN * 4u * (uint64_t)channels) +
	       (slices * 8u + (slices + 7u) / 8u) * (uint64_t)channels;
	if (size > (uint64_t)(size_t)-1)
	{
		return 0u;
	}
	return (size_t)size;
}

RGINLINE size_t rg_rgs_encode_s16(const int16_t* pcm,
                                  uint32_t samples,
                                  uint32_t channels,
                                  uint32_t samplerate,
                                  void* dst,
                                  size_t dst_size)
{
	RgRgsEncodeOptions options = rg_rgs_default_options();
	return rg_rgs_encode_s16_ex(pcm, samples, channels, samplerate, dst, dst_size, &options);
}

RGINLINE size_t rg_rgs_encode_s16_ex(const int16_t* pcm,
                                     uint32_t samples,
                                     uint32_t channels,
                                     uint32_t samplerate,
                                     void* dst,
                                     size_t dst_size,
                                     const RgRgsEncodeOptions* options)
{
	RgRgsEncodeOptions resolved;
	size_t bound;
	RgRgsLms lms[RG_RGS_MAX_CHANNELS];
	uint8_t* bytes = (uint8_t*)dst;
	size_t p = 0u;

	if (pcm == NULL ||
	    dst == NULL ||
	    !rg_rgs_valid_desc(samples, channels, samplerate) ||
	    !rg_rgs_options_valid(options))
	{
		return 0u;
	}

	resolved = rg_rgs_resolve_options(options);
	bound = rg_rgs_encode_bound(samples, channels, samplerate);
	if (bound == 0u || dst_size < bound)
	{
		return 0u;
	}

	memset(lms, 0, sizeof(lms));
	for (uint32_t c = 0u; c < channels; c++)
	{
		lms[c].weights[2] = -(1 << 13);
		lms[c].weights[3] = 1 << 14;
	}

	bytes[p++] = (uint8_t)'r';
	bytes[p++] = (uint8_t)'g';
	bytes[p++] = (uint8_t)'s';
	bytes[p++] = (uint8_t)'!';
	rg_rgs_write_u32le(bytes + p, samples);
	p += 4u;
	bytes[p++] = (uint8_t)RG_RGS_VERSION;
	bytes[p++] = (uint8_t)RG_RGS_FILE_FLAGS;
	bytes[p++] = 0u;
	bytes[p++] = 0u;

	for (uint32_t sample_index = 0u; sample_index < samples;)
	{
		uint32_t frame_len = samples - sample_index;
		size_t written;
		if (frame_len > RG_RGS_MAX_FRAME_SAMPLES)
		{
			frame_len = RG_RGS_MAX_FRAME_SAMPLES;
		}
		written = rg_rgs_encode_frame_planar_mixed(
		    pcm + (size_t)sample_index * channels,
		    channels,
		    samplerate,
		    frame_len,
		    lms,
		    bytes + p,
		    &resolved);
		p += written;
		/* Advancing by the final partial frame cannot wrap at UINT32_MAX. */
		sample_index += frame_len;
	}
	return p;
}

RGINLINE int rg_rgs_read_header(const void* src, size_t src_size, RgRgsInfo* out_info)
{
	const uint8_t* bytes = (const uint8_t*)src;
	uint32_t samples;
	uint32_t channels;
	uint32_t samplerate;
	RgRgsInfo info;
	RgRgsFrameMeta first_frame;

	if (bytes == NULL || src_size < RG_RGS_HEADER_SIZE + RG_RGS_FRAME_HEADER_SIZE)
	{
		return 0;
	}
	if (bytes[0] != (uint8_t)'r' ||
	    bytes[1] != (uint8_t)'g' ||
	    bytes[2] != (uint8_t)'s' ||
	    bytes[3] != (uint8_t)'!' ||
	    bytes[8] != (uint8_t)RG_RGS_VERSION ||
	    bytes[9] != (uint8_t)RG_RGS_FILE_FLAGS ||
	    bytes[10] != 0u ||
	    bytes[11] != 0u)
	{
		return 0;
	}

	samples = rg_rgs_read_u32le(bytes + 4u);
	channels = bytes[12] & 0x0fu;
	samplerate = rg_rgs_read_u24le(bytes + 13u);

	if (!rg_rgs_valid_desc(samples, channels, samplerate) ||
	    samplerate > RG_RGS_MAX_STORED_SAMPLERATE)
	{
		return 0;
	}
	info.channels = channels;
	info.samplerate = samplerate;
	info.samples = samples;
	if (!rg_rgs_parse_frame(bytes + RG_RGS_HEADER_SIZE, src_size - RG_RGS_HEADER_SIZE,
	                        &info, samples, &first_frame))
	{
		return 0;
	}

	if (out_info != NULL)
	{
		*out_info = info;
	}
	return 1;
}

RGINLINE int rg_rgs_decoder_init(RgRgsDecoder* decoder,
                                 const void* src,
                                 size_t src_size,
                                 RgRgsInfo* out_info)
{
	RgRgsInfo info;
	if (decoder == NULL)
	{
		return 0;
	}

	memset(decoder, 0, sizeof(*decoder));
	decoder->data = (const uint8_t*)src;
	decoder->size = src_size;
	decoder->failed = 1;
	if (!rg_rgs_read_header(src, src_size, &info))
	{
		return 0;
	}

	decoder->offset = RG_RGS_HEADER_SIZE;
	decoder->info = info;
	decoder->failed = 0;
	if (out_info != NULL)
	{
		*out_info = info;
	}
	return 1;
}

RGINLINE void rg_rgs_decoder_reset(RgRgsDecoder* decoder)
{
	RgRgsInfo info;
	if (decoder == NULL)
	{
		return;
	}
	decoder->offset = RG_RGS_HEADER_SIZE;
	decoder->decoded_frames = 0u;
	decoder->failed = 1;
	if (rg_rgs_read_header(decoder->data, decoder->size, &info) &&
	    info.channels == decoder->info.channels &&
	    info.samplerate == decoder->info.samplerate &&
	    info.samples == decoder->info.samples)
	{
		decoder->failed = 0;
	}
}

RGINLINE RgRgsDecodeStatus rg_rgs_decoder_next_s16(RgRgsDecoder* decoder,
                                                   int16_t* dst,
                                                   size_t dst_samples,
                                                   uint32_t* out_frames)
{
	RgRgsFrameMeta meta;
	uint32_t remaining;
	size_t required;

	if (out_frames != NULL)
	{
		*out_frames = 0u;
	}
	if (decoder == NULL || out_frames == NULL)
	{
		if (decoder != NULL)
		{
			decoder->failed = 1;
		}
		return RG_RGS_DECODE_INVALID;
	}
	if (decoder->failed)
	{
		return RG_RGS_DECODE_INVALID;
	}
	if (decoder->decoded_frames == decoder->info.samples)
	{
		if (decoder->offset == decoder->size)
		{
			return RG_RGS_DECODE_END;
		}
		decoder->failed = 1;
		return RG_RGS_DECODE_INVALID;
	}
	if (decoder->decoded_frames > decoder->info.samples ||
	    decoder->offset > decoder->size)
	{
		decoder->failed = 1;
		return RG_RGS_DECODE_INVALID;
	}

	remaining = decoder->info.samples - decoder->decoded_frames;
	if (!rg_rgs_parse_frame(decoder->data + decoder->offset,
	                        decoder->size - decoder->offset,
	                        &decoder->info,
	                        remaining,
	                        &meta))
	{
		decoder->failed = 1;
		return RG_RGS_DECODE_INVALID;
	}

	required = (size_t)meta.samples * decoder->info.channels;
	if (dst_samples < required)
	{
		*out_frames = meta.samples;
		return RG_RGS_DECODE_OUTPUT_TOO_SMALL;
	}
	if (dst == NULL || !rg_rgs_decode_frame(decoder->data + decoder->offset,
	                                        &decoder->info,
	                                        &meta,
	                                        dst))
	{
		decoder->failed = 1;
		return RG_RGS_DECODE_INVALID;
	}

	decoder->offset += meta.frame_size;
	decoder->decoded_frames += meta.samples;
	*out_frames = meta.samples;
	return RG_RGS_DECODE_FRAME;
}

RGINLINE size_t rg_rgs_decode_s16(const void* src,
                                  size_t src_size,
                                  int16_t* dst,
                                  size_t dst_samples,
                                  RgRgsInfo* out_info)
{
	RgRgsDecoder decoder;
	RgRgsInfo info;
	size_t written = 0u;
	RgRgsDecodeStatus status;

	if (dst == NULL || !rg_rgs_decoder_init(&decoder, src, src_size, &info))
	{
		return 0u;
	}
	if ((uint64_t)info.samples * info.channels > (uint64_t)dst_samples)
	{
		return 0u;
	}

	do
	{
		uint32_t frames = 0u;
		status = rg_rgs_decoder_next_s16(&decoder,
		                                 dst + written,
		                                 dst_samples - written,
		                                 &frames);
		if (status == RG_RGS_DECODE_FRAME)
		{
			written += (size_t)frames * info.channels;
		}
	} while (status == RG_RGS_DECODE_FRAME);

	if (status != RG_RGS_DECODE_END ||
	    written != (size_t)info.samples * info.channels)
	{
		return 0u;
	}
	if (out_info != NULL)
	{
		*out_info = info;
	}
	return written;
}

RGINLINE size_t rg_rgs_decode_trusted_s16(const void* src,
                                          size_t src_size,
                                          int16_t* dst,
                                          size_t dst_samples,
                                          RgRgsInfo* out_info)
{
	const uint8_t* bytes = (const uint8_t*)src;
	RgRgsInfo info;
	size_t offset = RG_RGS_HEADER_SIZE;
	uint32_t decoded = 0u;

	if (dst == NULL || !rg_rgs_read_header(src, src_size, &info) ||
	    (uint64_t)info.samples * info.channels > (uint64_t)dst_samples)
	{
		return 0u;
	}

	while (decoded < info.samples)
	{
		const uint8_t* frame = bytes + offset;
		RgRgsFrameMeta meta;
		meta.flags = frame[0] >> 4u;
		meta.samples = rg_rgs_read_u16le(frame + 4u);
		meta.slices = (meta.samples + RG_RGS_SLICE_LEN - 1u) / RG_RGS_SLICE_LEN;
		meta.mode_bytes = meta.flags == (RG_RGS_FRAME_FLAG_PLANAR | RG_RGS_FRAME_FLAG_MIXED) ? RG_RGS_PLANAR_MODE_BYTES(meta.slices) : 0u;
		meta.frame_size = rg_rgs_read_u16le(frame + 6u);
		meta.payload_offset = RG_RGS_FRAME_HEADER_SIZE +
		                      (size_t)RG_RGS_LMS_LEN * 4u * info.channels;
		(void)rg_rgs_decode_frame(frame,
		                          &info,
		                          &meta,
		                          dst + (size_t)decoded * info.channels);
		offset += meta.frame_size;
		decoded += meta.samples;
	}

	if (out_info != NULL)
	{
		*out_info = info;
	}
	return (size_t)info.samples * info.channels;
}

#endif /* RG_RGS_H */
