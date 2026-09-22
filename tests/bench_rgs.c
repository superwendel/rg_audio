// RGS codec benchmark. Measurements are informational and never gate tests.
//
// Usage:
//   bench_rgs [--iters N] [--quality high|medium|low]
//             [--target-kbps N] [optional-pcm16.wav ...]

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "../src/rg_rgs.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4245 4701)
#endif
#define QOA_IMPLEMENTATION
#define QOA_NO_STDIO
#include "../third_party/qoa/qoa.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef struct BenchAudio
{
	const char* name;
	int16_t* pcm;
	uint32_t frames;
	uint32_t channels;
	uint32_t samplerate;
} BenchAudio;

static volatile uint64_t bench_sink = 0u;

static double bench_now_ms(void)
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

static uint16_t bench_read_u16le(const uint8_t* bytes)
{
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u));
}

static uint32_t bench_read_u32le(const uint8_t* bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
	       ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static int bench_read_file(const char* path, uint8_t** out_data, size_t* out_size)
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

static int bench_load_pcm16_wav(const char* path, BenchAudio* out_audio)
{
	uint8_t* file_data = NULL;
	size_t file_size = 0u;
	size_t cursor = 12u;
	uint16_t format = 0u;
	uint16_t channels = 0u;
	uint16_t bits = 0u;
	uint32_t samplerate = 0u;
	const uint8_t* pcm_bytes = NULL;
	size_t pcm_size = 0u;
	size_t values;
	size_t i;

	if (!bench_read_file(path, &file_data, &file_size) || file_size < 12u ||
	    memcmp(file_data, "RIFF", 4u) != 0 || memcmp(file_data + 8u, "WAVE", 4u) != 0)
	{
		free(file_data);
		return 0;
	}
	while (cursor <= file_size && file_size - cursor >= 8u)
	{
		const uint8_t* id = file_data + cursor;
		const uint32_t chunk_size = bench_read_u32le(file_data + cursor + 4u);
		cursor += 8u;
		if ((size_t)chunk_size > file_size - cursor)
		{
			free(file_data);
			return 0;
		}
		if (memcmp(id, "fmt ", 4u) == 0 && chunk_size >= 16u)
		{
			format = bench_read_u16le(file_data + cursor);
			channels = bench_read_u16le(file_data + cursor + 2u);
			samplerate = bench_read_u32le(file_data + cursor + 4u);
			bits = bench_read_u16le(file_data + cursor + 14u);
		}
		else if (memcmp(id, "data", 4u) == 0 && pcm_bytes == NULL)
		{
			pcm_bytes = file_data + cursor;
			pcm_size = (size_t)chunk_size;
		}
		if ((size_t)chunk_size + ((size_t)chunk_size & 1u) > file_size - cursor)
		{
			free(file_data);
			return 0;
		}
		cursor += (size_t)chunk_size + ((size_t)chunk_size & 1u);
	}
	if (format != 1u || channels == 0u || channels > RG_RGS_MAX_CHANNELS ||
	    samplerate == 0u || bits != 16u || pcm_bytes == NULL || pcm_size == 0u ||
	    (pcm_size % ((size_t)channels * sizeof(int16_t))) != 0u ||
	    pcm_size / ((size_t)channels * sizeof(int16_t)) > UINT32_MAX)
	{
		free(file_data);
		return 0;
	}
	out_audio->pcm = (int16_t*)malloc(pcm_size);
	if (out_audio->pcm == NULL)
	{
		free(file_data);
		return 0;
	}
	values = pcm_size / sizeof(int16_t);
	for (i = 0u; i < values; ++i)
	{
		out_audio->pcm[i] = (int16_t)bench_read_u16le(pcm_bytes + i * 2u);
	}
	out_audio->name = path;
	out_audio->frames = (uint32_t)(values / channels);
	out_audio->channels = channels;
	out_audio->samplerate = samplerate;
	free(file_data);
	return 1;
}

static int16_t bench_sample(double value)
{
	if (value > 1.0)
	{
		value = 1.0;
	}
	else if (value < -1.0)
	{
		value = -1.0;
	}
	return (int16_t)(value * 32767.0);
}

static int bench_generate(BenchAudio* audio, unsigned int pattern)
{
	const uint32_t samplerate = 44100u;
	const uint32_t frames = samplerate * 2u + 137u;
	const uint32_t channels = pattern == 1u ? 1u : 2u;
	uint32_t frame;
	uint32_t channel;
	const char* names[3] = {"generated-tonal", "generated-voice", "generated-transients"};
	audio->pcm = (int16_t*)malloc((size_t)frames * channels * sizeof(int16_t));
	if (audio->pcm == NULL)
	{
		return 0;
	}
	for (frame = 0u; frame < frames; ++frame)
	{
		const double t = (double)frame / (double)samplerate;
		for (channel = 0u; channel < channels; ++channel)
		{
			double value;
			if (pattern == 0u)
			{
				value = 0.48 * sin(6.283185307179586 * (220.0 + 3.0 * channel) * t) +
				        0.22 * sin(6.283185307179586 * 880.0 * t + channel * 0.7) +
				        0.08 * sin(6.283185307179586 * 3520.0 * t);
			}
			else if (pattern == 1u)
			{
				const double syllable = 0.35 + 0.65 * fabs(sin(6.283185307179586 * 3.7 * t));
				value = syllable * (0.5 * sin(6.283185307179586 * 118.0 * t) +
				                    0.18 * sin(6.283185307179586 * 713.0 * t) +
				                    0.09 * sin(6.283185307179586 * 1901.0 * t));
			}
			else
			{
				const uint32_t beat = frame % (samplerate / 5u);
				const double impulse = beat < 96u ? (1.0 - (double)beat / 96.0) * 0.8 : 0.0;
				const uint32_t noise_word = frame * 1664525u + channel * 1013904223u;
				const double noise = ((double)((noise_word >> 8u) & 0xffffu) / 32767.5 - 1.0) * 0.08;
				value = impulse + noise + 0.16 * sin(6.283185307179586 * (55.0 + channel * 27.0) * t);
			}
			audio->pcm[(size_t)frame * channels + channel] = bench_sample(value);
		}
	}
	audio->name = names[pattern];
	audio->frames = frames;
	audio->channels = channels;
	audio->samplerate = samplerate;
	return 1;
}

static const char* bench_quality_name(RgRgsQuality quality)
{
	switch (quality)
	{
		case RG_RGS_QUALITY_HIGH:
			return "high";
		case RG_RGS_QUALITY_MEDIUM:
			return "medium";
		case RG_RGS_QUALITY_LOW:
			return "low";
		default:
			return "invalid";
	}
}

static double bench_snr_db(const int16_t* reference, const int16_t* decoded, size_t count)
{
	double signal = 0.0;
	double error = 0.0;
	size_t i;
	for (i = 0u; i < count; ++i)
	{
		const double sample = (double)reference[i];
		const double difference = sample - (double)decoded[i];
		signal += sample * sample;
		error += difference * difference;
	}
	if (error == 0.0)
	{
		return 999.0;
	}
	return signal > 0.0 ? 10.0 * log10(signal / error) : 0.0;
}

static int bench_audio(const BenchAudio* audio,
                       const RgRgsEncodeOptions* options,
                       uint32_t iterations)
{
	const size_t input_values = (size_t)audio->frames * audio->channels;
	const size_t input_bytes = input_values * sizeof(int16_t);
	const size_t rgs_bound = rg_rgs_encode_bound(audio->frames, audio->channels, audio->samplerate);
	uint8_t* rgs_data = NULL;
	size_t rgs_size = 0u;
	void* qoa_data = NULL;
	unsigned int qoa_size = 0u;
	RgRgsInfo rgs_info = {0};
	int16_t* rgs_pcm = NULL;
	short* qoa_pcm = NULL;
	double rgs_encode_ms = 0.0;
	double qoa_encode_ms = 0.0;
	double rgs_decode_ms = 0.0;
	double qoa_decode_ms = 0.0;
	uint32_t i;
	int ok = 0;

	if (rgs_bound == 0u)
	{
		fprintf(stderr, "%s: input cannot be represented by RGS.\n", audio->name);
		return 0;
	}
	rgs_data = (uint8_t*)malloc(rgs_bound);
	if (rgs_data == NULL)
	{
		return 0;
	}

	for (i = 0u; i < iterations; ++i)
	{
		double started = bench_now_ms();
		rgs_size = rg_rgs_encode_s16_ex(audio->pcm,
		                                audio->frames,
		                                audio->channels,
		                                audio->samplerate,
		                                rgs_data,
		                                rgs_bound,
		                                options);
		rgs_encode_ms += bench_now_ms() - started;
		if (rgs_size == 0u)
		{
			goto cleanup;
		}
	}
	for (i = 0u; i < iterations; ++i)
	{
		qoa_desc description;
		double started;
		free(qoa_data);
		qoa_data = NULL;
		memset(&description, 0, sizeof(description));
		description.channels = audio->channels;
		description.samplerate = audio->samplerate;
		description.samples = audio->frames;
		started = bench_now_ms();
		qoa_data = qoa_encode((const short*)audio->pcm, &description, &qoa_size);
		qoa_encode_ms += bench_now_ms() - started;
		if (qoa_data == NULL || qoa_size == 0u || qoa_size > (unsigned int)INT_MAX)
		{
			goto cleanup;
		}
	}
	if (!rg_rgs_read_header(rgs_data, rgs_size, &rgs_info))
	{
		goto cleanup;
	}
	rgs_pcm = (int16_t*)malloc((size_t)rgs_info.samples * rgs_info.channels * sizeof(int16_t));
	if (rgs_pcm == NULL)
	{
		goto cleanup;
	}
	for (i = 0u; i < iterations; ++i)
	{
		const size_t capacity = (size_t)rgs_info.samples * rgs_info.channels;
		double started = bench_now_ms();
		const size_t written = rg_rgs_decode_s16(rgs_data, rgs_size, rgs_pcm, capacity, NULL);
		rgs_decode_ms += bench_now_ms() - started;
		if (written != capacity)
		{
			goto cleanup;
		}
	}
	for (i = 0u; i < iterations; ++i)
	{
		qoa_desc decoded_description;
		double started;
		free(qoa_pcm);
		qoa_pcm = NULL;
		memset(&decoded_description, 0, sizeof(decoded_description));
		started = bench_now_ms();
		qoa_pcm = qoa_decode((const unsigned char*)qoa_data, (int)qoa_size, &decoded_description);
		qoa_decode_ms += bench_now_ms() - started;
		if (qoa_pcm == NULL || decoded_description.samples != audio->frames ||
		    decoded_description.channels != audio->channels)
		{
			goto cleanup;
		}
	}

	bench_sink += (uint64_t)(uint16_t)rgs_pcm[(size_t)rgs_info.samples * rgs_info.channels - 1u];
	bench_sink += (uint64_t)(uint16_t)qoa_pcm[input_values - 1u];
	printf("\n%s: %u Hz, %u ch, %u frames\n",
	       audio->name,
	       audio->samplerate,
	       audio->channels,
	       audio->frames);
	printf("  RGS  %8zu bytes  %7.1f kbps  encode %8.1f MiB/s (%7.3f ms)  decode %8.1f MiB/s (%7.3f ms)\n",
	       rgs_size,
	       (double)rgs_size * 8.0 * (double)rgs_info.samplerate /
	           ((double)rgs_info.samples * 1000.0),
	       (double)input_bytes * iterations / (1024.0 * 1024.0) /
	           (rgs_encode_ms > 0.0 ? rgs_encode_ms / 1000.0 : 0.000001),
	       rgs_encode_ms / iterations,
	       (double)((size_t)rgs_info.samples * rgs_info.channels * sizeof(int16_t)) * iterations /
	           (1024.0 * 1024.0) / (rgs_decode_ms > 0.0 ? rgs_decode_ms / 1000.0 : 0.000001),
	       rgs_decode_ms / iterations);
	printf("  QOA  %8u bytes  %7.1f kbps  encode %8.1f MiB/s (%7.3f ms)  decode %8.1f MiB/s (%7.3f ms)\n",
	       qoa_size,
	       (double)qoa_size * 8.0 * (double)audio->samplerate /
	           ((double)audio->frames * 1000.0),
	       (double)input_bytes * iterations / (1024.0 * 1024.0) /
	           (qoa_encode_ms > 0.0 ? qoa_encode_ms / 1000.0 : 0.000001),
	       qoa_encode_ms / iterations,
	       (double)input_bytes * iterations / (1024.0 * 1024.0) /
	           (qoa_decode_ms > 0.0 ? qoa_decode_ms / 1000.0 : 0.000001),
	       qoa_decode_ms / iterations);
	if (rgs_info.samplerate == audio->samplerate && rgs_info.samples == audio->frames)
	{
		printf("  SNR  RGS %.2f dB, QOA %.2f dB; RGS/QOA size %.1f%%\n",
		       bench_snr_db(audio->pcm, rgs_pcm, input_values),
		       bench_snr_db(audio->pcm, (const int16_t*)qoa_pcm, input_values),
		       (double)rgs_size * 100.0 / (double)qoa_size);
	}
	else
	{
		printf("  RGS stored at %u Hz/%u frames; source-rate SNR omitted. RGS/QOA size %.1f%%\n",
		       rgs_info.samplerate,
		       rgs_info.samples,
		       (double)rgs_size * 100.0 / (double)qoa_size);
	}
	ok = 1;

cleanup:
	free(qoa_pcm);
	free(rgs_pcm);
	free(qoa_data);
	free(rgs_data);
	return ok;
}

static int bench_parse_u32(const char* text, uint32_t* out_value)
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

static int bench_parse_quality(const char* text, RgRgsQuality* quality)
{
	if (strcmp(text, "high") == 0)
	{
		*quality = RG_RGS_QUALITY_HIGH;
		return 1;
	}
	if (strcmp(text, "medium") == 0)
	{
		*quality = RG_RGS_QUALITY_MEDIUM;
		return 1;
	}
	if (strcmp(text, "low") == 0)
	{
		*quality = RG_RGS_QUALITY_LOW;
		return 1;
	}
	return 0;
}

static void bench_usage(const char* executable)
{
	printf("Usage: %s [--iters N] [--quality high|medium|low] [--target-kbps N] [pcm16.wav ...]\n",
	       executable);
}

int main(int argc, char** argv)
{
	RgRgsEncodeOptions options = rg_rgs_default_options();
	uint32_t iterations = 3u;
	int file_count = 0;
	int ok = 1;
	int i;

	for (i = 1; i < argc; ++i)
	{
		if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc)
		{
			if (!bench_parse_u32(argv[++i], &iterations) || iterations == 0u)
			{
				bench_usage(argv[0]);
				return 2;
			}
		}
		else if (strcmp(argv[i], "--quality") == 0 && i + 1 < argc)
		{
			if (!bench_parse_quality(argv[++i], &options.quality))
			{
				bench_usage(argv[0]);
				return 2;
			}
		}
		else if (strcmp(argv[i], "--target-kbps") == 0 && i + 1 < argc)
		{
			if (!bench_parse_u32(argv[++i], &options.target_kbps))
			{
				bench_usage(argv[0]);
				return 2;
			}
		}
		else if (strcmp(argv[i], "--help") == 0)
		{
			bench_usage(argv[0]);
			return 0;
		}
		else if (argv[i][0] == '-')
		{
			bench_usage(argv[0]);
			return 2;
		}
		else
		{
			++file_count;
		}
	}

	printf("RGS v%u benchmark: quality=%s target=%u kbps iterations=%u\n",
	       (unsigned int)RG_RGS_VERSION,
	       bench_quality_name(options.quality),
	       options.target_kbps,
	       iterations);
	if (file_count == 0)
	{
		unsigned int pattern;
		for (pattern = 0u; pattern < 3u; ++pattern)
		{
			BenchAudio audio;
			memset(&audio, 0, sizeof(audio));
			if (!bench_generate(&audio, pattern) || !bench_audio(&audio, &options, iterations))
			{
				fprintf(stderr, "Generated benchmark %u failed.\n", pattern);
				ok = 0;
			}
			free(audio.pcm);
		}
	}
	else
	{
		for (i = 1; i < argc; ++i)
		{
			if ((strcmp(argv[i], "--iters") == 0 || strcmp(argv[i], "--quality") == 0 ||
			     strcmp(argv[i], "--target-kbps") == 0) &&
			    i + 1 < argc)
			{
				++i;
				continue;
			}
			if (argv[i][0] != '-')
			{
				BenchAudio audio;
				memset(&audio, 0, sizeof(audio));
				if (!bench_load_pcm16_wav(argv[i], &audio))
				{
					fprintf(stderr, "%s: only uncompressed PCM16 WAV is supported.\n", argv[i]);
					ok = 0;
					continue;
				}
				if (!bench_audio(&audio, &options, iterations))
				{
					fprintf(stderr, "%s: benchmark failed.\n", argv[i]);
					ok = 0;
				}
				free(audio.pcm);
			}
		}
	}
	printf("\nBenchmark sink: %llu\n", (unsigned long long)bench_sink);
	return ok ? 0 : 1;
}
