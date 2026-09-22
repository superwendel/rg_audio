// Allocation-free RGS streaming benchmark.
//
// The decoder-worker and callback-copy timings are reported separately. This
// benchmark is informational; it intentionally has no performance pass gate.
//
// Usage:
//   bench_rgs_stream [--iters N] [--callback-frames N] [optional-file.rgs ...]

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "../src/rg_rgs.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define BENCH_RING_SLOTS 4u

typedef struct BenchSlot
{
	int16_t* pcm;
	uint32_t frames;
	uint32_t cursor;
	int ready;
} BenchSlot;

static volatile uint64_t stream_sink = 0u;

static double stream_now_ms(void)
{
#if defined(_WIN32)
	static LARGE_INTEGER frequency;
	LARGE_INTEGER counter;
	if (frequency.QuadPart == 0)
	{
		(void)QueryPerformanceFrequency(&frequency);
	}
	(void)QueryPerformanceCounter(&counter);
	return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
#else
	struct timespec value;
	(void)clock_gettime(CLOCK_MONOTONIC, &value);
	return (double)value.tv_sec * 1000.0 + (double)value.tv_nsec / 1000000.0;
#endif
}

static int stream_read_file(const char* path, uint8_t** out_data, size_t* out_size)
{
	FILE* file = fopen(path, "rb");
	long length;
	uint8_t* data;
	if (file == NULL || fseek(file, 0, SEEK_END) != 0)
	{
		if (file != NULL)
		{
			(void)fclose(file);
		}
		return 0;
	}
	length = ftell(file);
	if (length <= 0 || fseek(file, 0, SEEK_SET) != 0)
	{
		(void)fclose(file);
		return 0;
	}
	data = (uint8_t*)malloc((size_t)length);
	if (data == NULL)
	{
		(void)fclose(file);
		return 0;
	}
	if (fread(data, 1u, (size_t)length, file) != (size_t)length || fclose(file) != 0)
	{
		free(data);
		return 0;
	}
	*out_data = data;
	*out_size = (size_t)length;
	return 1;
}

static int stream_make_generated(uint8_t** out_data, size_t* out_size)
{
	const uint32_t channels = 2u;
	const uint32_t samplerate = 44100u;
	const uint32_t frames = samplerate * 3u + 17u;
	const size_t values = (size_t)frames * channels;
	const size_t bound = rg_rgs_encode_bound(frames, channels, samplerate);
	RgRgsEncodeOptions options = rg_rgs_default_options();
	int16_t* pcm;
	uint8_t* encoded;
	uint32_t frame;
	size_t written;

	if (bound == 0u)
	{
		return 0;
	}
	pcm = (int16_t*)malloc(values * sizeof(int16_t));
	encoded = (uint8_t*)malloc(bound);
	if (pcm == NULL || encoded == NULL)
	{
		free(encoded);
		free(pcm);
		return 0;
	}
	for (frame = 0u; frame < frames; ++frame)
	{
		const uint32_t phase = frame % 1024u;
		const int32_t saw = (int32_t)phase * 48 - 24576;
		const uint32_t noise_word = frame * 1664525u + 1013904223u;
		const int32_t noise = (int32_t)((noise_word >> 17u) & 0x7fffu) - 16384;
		pcm[(size_t)frame * 2u] = (int16_t)(saw + noise / 8);
		pcm[(size_t)frame * 2u + 1u] = (int16_t)(-saw + noise / 10);
	}
	written = rg_rgs_encode_s16_ex(
	    pcm, frames, channels, samplerate, encoded, bound, &options);
	free(pcm);
	if (written == 0u)
	{
		free(encoded);
		return 0;
	}
	*out_data = encoded;
	*out_size = written;
	return 1;
}

static int stream_bench_decoder(const uint8_t* data,
                                size_t size,
                                const RgRgsInfo* info,
                                uint32_t iterations,
                                double* out_ms)
{
	RgRgsDecoder decoder;
	int16_t* frame_pcm;
	uint32_t iteration;
	double elapsed = 0.0;
	if (!rg_rgs_decoder_init(&decoder, data, size, NULL))
	{
		return 0;
	}
	frame_pcm = (int16_t*)malloc(
	    (size_t)RG_RGS_MAX_FRAME_SAMPLES * info->channels * sizeof(int16_t));
	if (frame_pcm == NULL)
	{
		return 0;
	}
	for (iteration = 0u; iteration < iterations; ++iteration)
	{
		uint32_t decoded_frames = 0u;
		rg_rgs_decoder_reset(&decoder);
		for (;;)
		{
			uint32_t frame_count = 0u;
			RgRgsDecodeStatus status;
			double started = stream_now_ms();
			status = rg_rgs_decoder_next_s16(&decoder,
			                                 frame_pcm,
			                                 (size_t)RG_RGS_MAX_FRAME_SAMPLES * info->channels,
			                                 &frame_count);
			elapsed += stream_now_ms() - started;
			if (status == RG_RGS_DECODE_FRAME)
			{
				decoded_frames += frame_count;
				stream_sink += (uint64_t)(uint16_t)frame_pcm[(size_t)(frame_count - 1u) * info->channels];
			}
			else if (status == RG_RGS_DECODE_END)
			{
				break;
			}
			else
			{
				free(frame_pcm);
				return 0;
			}
		}
		if (decoded_frames != info->samples)
		{
			free(frame_pcm);
			return 0;
		}
	}
	free(frame_pcm);
	*out_ms = elapsed;
	return 1;
}

static int stream_bench_ring(const uint8_t* data,
                             size_t size,
                             const RgRgsInfo* info,
                             uint32_t iterations,
                             uint32_t callback_frames,
                             double* out_decode_ms,
                             double* out_copy_ms,
                             uint64_t* out_callbacks)
{
	RgRgsDecoder decoder;
	BenchSlot slots[BENCH_RING_SLOTS];
	int16_t* slot_storage;
	int16_t* callback_pcm;
	uint32_t iteration;
	double decode_ms = 0.0;
	double copy_ms = 0.0;
	uint64_t callbacks = 0u;
	uint32_t slot_index;

	if (!rg_rgs_decoder_init(&decoder, data, size, NULL) || callback_frames == 0u ||
	    (size_t)callback_frames > SIZE_MAX / (info->channels * sizeof(int16_t)))
	{
		return 0;
	}
	slot_storage = (int16_t*)malloc((size_t)BENCH_RING_SLOTS *
	                                RG_RGS_MAX_FRAME_SAMPLES * info->channels * sizeof(int16_t));
	callback_pcm = (int16_t*)malloc(
	    (size_t)callback_frames * info->channels * sizeof(int16_t));
	if (slot_storage == NULL || callback_pcm == NULL)
	{
		free(callback_pcm);
		free(slot_storage);
		return 0;
	}
	for (slot_index = 0u; slot_index < BENCH_RING_SLOTS; ++slot_index)
	{
		slots[slot_index].pcm = slot_storage +
		                        (size_t)slot_index * RG_RGS_MAX_FRAME_SAMPLES * info->channels;
		slots[slot_index].frames = 0u;
		slots[slot_index].cursor = 0u;
		slots[slot_index].ready = 0;
	}

	for (iteration = 0u; iteration < iterations; ++iteration)
	{
		uint32_t producer = 0u;
		uint32_t consumer = 0u;
		uint32_t ready_count = 0u;
		uint32_t consumed = 0u;
		int producer_at_end = 0;
		rg_rgs_decoder_reset(&decoder);
		for (slot_index = 0u; slot_index < BENCH_RING_SLOTS; ++slot_index)
		{
			slots[slot_index].ready = 0;
			slots[slot_index].frames = 0u;
			slots[slot_index].cursor = 0u;
		}

		while (consumed < info->samples)
		{
			while (ready_count < BENCH_RING_SLOTS && !producer_at_end)
			{
				uint32_t frame_count = 0u;
				RgRgsDecodeStatus status;
				double started = stream_now_ms();
				status = rg_rgs_decoder_next_s16(&decoder,
				                                 slots[producer].pcm,
				                                 (size_t)RG_RGS_MAX_FRAME_SAMPLES * info->channels,
				                                 &frame_count);
				decode_ms += stream_now_ms() - started;
				if (status == RG_RGS_DECODE_FRAME)
				{
					slots[producer].frames = frame_count;
					slots[producer].cursor = 0u;
					slots[producer].ready = 1;
					producer = (producer + 1u) % BENCH_RING_SLOTS;
					++ready_count;
				}
				else if (status == RG_RGS_DECODE_END)
				{
					producer_at_end = 1;
				}
				else
				{
					free(callback_pcm);
					free(slot_storage);
					return 0;
				}
			}

			{
				uint32_t request = info->samples - consumed;
				uint32_t copied = 0u;
				double started;
				if (request > callback_frames)
				{
					request = callback_frames;
				}
				started = stream_now_ms();
				while (copied < request && ready_count != 0u)
				{
					BenchSlot* slot = &slots[consumer];
					uint32_t available = slot->frames - slot->cursor;
					uint32_t take = request - copied;
					if (!slot->ready)
					{
						free(callback_pcm);
						free(slot_storage);
						return 0;
					}
					if (take > available)
					{
						take = available;
					}
					memcpy(callback_pcm + (size_t)copied * info->channels,
					       slot->pcm + (size_t)slot->cursor * info->channels,
					       (size_t)take * info->channels * sizeof(int16_t));
					copied += take;
					slot->cursor += take;
					if (slot->cursor == slot->frames)
					{
						slot->ready = 0;
						--ready_count;
						consumer = (consumer + 1u) % BENCH_RING_SLOTS;
					}
				}
				copy_ms += stream_now_ms() - started;
				if (copied == 0u)
				{
					free(callback_pcm);
					free(slot_storage);
					return 0;
				}
				consumed += copied;
				++callbacks;
				stream_sink += (uint64_t)(uint16_t)callback_pcm[(size_t)(copied - 1u) * info->channels];
			}
		}

		if (!producer_at_end)
		{
			uint32_t frame_count = 0u;
			RgRgsDecodeStatus status;
			double started = stream_now_ms();
			status = rg_rgs_decoder_next_s16(&decoder,
			                                 slots[producer].pcm,
			                                 (size_t)RG_RGS_MAX_FRAME_SAMPLES * info->channels,
			                                 &frame_count);
			decode_ms += stream_now_ms() - started;
			if (status != RG_RGS_DECODE_END || frame_count != 0u)
			{
				free(callback_pcm);
				free(slot_storage);
				return 0;
			}
		}
	}

	free(callback_pcm);
	free(slot_storage);
	*out_decode_ms = decode_ms;
	*out_copy_ms = copy_ms;
	*out_callbacks = callbacks;
	return 1;
}

static int stream_bench_one(const char* name,
                            const uint8_t* data,
                            size_t size,
                            uint32_t iterations,
                            uint32_t callback_frames)
{
	RgRgsDecoder decoder;
	RgRgsInfo info = {0};
	double direct_ms;
	double ring_decode_ms;
	double copy_ms;
	uint64_t callbacks;
	double duration_seconds;
	double total_pcm_mib;

	if (!rg_rgs_decoder_init(&decoder, data, size, &info))
	{
		fprintf(stderr, "%s: invalid RGS v1 stream.\n", name);
		return 0;
	}
	if (!stream_bench_decoder(data, size, &info, iterations, &direct_ms) ||
	    !stream_bench_ring(data,
	                       size,
	                       &info,
	                       iterations,
	                       callback_frames,
	                       &ring_decode_ms,
	                       &copy_ms,
	                       &callbacks))
	{
		fprintf(stderr, "%s: streaming benchmark failed.\n", name);
		return 0;
	}
	duration_seconds = (double)info.samples / (double)info.samplerate;
	total_pcm_mib = (double)info.samples * info.channels * sizeof(int16_t) * iterations /
	                (1024.0 * 1024.0);
	printf("\n%s\n", name);
	printf("  %u Hz, %u ch, %u frames, %.1f kbps, %.3f seconds\n",
	       info.samplerate,
	       info.channels,
	       info.samples,
	       (double)size * 8.0 / (duration_seconds * 1000.0),
	       duration_seconds);
	printf("  decoder loop:       %8.1f MiB/s  %8.1fx realtime\n",
	       total_pcm_mib / (direct_ms > 0.0 ? direct_ms / 1000.0 : 0.000001),
	       duration_seconds * iterations / (direct_ms > 0.0 ? direct_ms / 1000.0 : 0.000001));
	printf("  ring worker decode: %8.1f MiB/s  (%llu callbacks, four slots)\n",
	       total_pcm_mib / (ring_decode_ms > 0.0 ? ring_decode_ms / 1000.0 : 0.000001),
	       (unsigned long long)callbacks);
	printf("  callback ring copy: %8.1f MiB/s  %.3f us/callback at %u frames\n",
	       total_pcm_mib / (copy_ms > 0.0 ? copy_ms / 1000.0 : 0.000001),
	       (copy_ms * 1000.0) / (double)callbacks,
	       callback_frames);
	return 1;
}

static int stream_parse_u32(const char* text, uint32_t* out_value)
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

static void stream_usage(const char* executable)
{
	printf("Usage: %s [--iters N] [--callback-frames N] [file.rgs ...]\n", executable);
}

int main(int argc, char** argv)
{
	uint32_t iterations = 5u;
	uint32_t callback_frames = 512u;
	int file_count = 0;
	int ok = 1;
	int i;

	for (i = 1; i < argc; ++i)
	{
		if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc)
		{
			if (!stream_parse_u32(argv[++i], &iterations) || iterations == 0u)
			{
				stream_usage(argv[0]);
				return 2;
			}
		}
		else if (strcmp(argv[i], "--callback-frames") == 0 && i + 1 < argc)
		{
			if (!stream_parse_u32(argv[++i], &callback_frames) || callback_frames == 0u)
			{
				stream_usage(argv[0]);
				return 2;
			}
		}
		else if (strcmp(argv[i], "--help") == 0)
		{
			stream_usage(argv[0]);
			return 0;
		}
		else if (argv[i][0] == '-')
		{
			stream_usage(argv[0]);
			return 2;
		}
		else
		{
			++file_count;
		}
	}

	printf("RGS v%u streaming benchmark: iterations=%u callback=%u frames\n",
	       (unsigned int)RG_RGS_VERSION,
	       iterations,
	       callback_frames);
	if (file_count == 0)
	{
		uint8_t* data = NULL;
		size_t size = 0u;
		if (!stream_make_generated(&data, &size) ||
		    !stream_bench_one("generated-stream", data, size, iterations, callback_frames))
		{
			ok = 0;
		}
		free(data);
	}
	else
	{
		for (i = 1; i < argc; ++i)
		{
			if ((strcmp(argv[i], "--iters") == 0 || strcmp(argv[i], "--callback-frames") == 0) &&
			    i + 1 < argc)
			{
				++i;
				continue;
			}
			if (argv[i][0] != '-')
			{
				uint8_t* data = NULL;
				size_t size = 0u;
				if (!stream_read_file(argv[i], &data, &size) ||
				    !stream_bench_one(argv[i], data, size, iterations, callback_frames))
				{
					ok = 0;
				}
				free(data);
			}
		}
	}
	printf("\nBenchmark sink: %llu\n", (unsigned long long)stream_sink);
	return ok ? 0 : 1;
}
