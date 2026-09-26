/* Offline preparation checks: unchanged PCM, flushed duration, repeatability,
 * channel isolation, and suppression of content above the stored Nyquist. */
#include "../tools/rgs_audio_prepare.h"

#include <math.h>
#include <stdio.h>

static int failures;
#define CHECK(condition) \
	do { if (!(condition)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

static void test_unchanged(void)
{
	const uint32_t rates[] = {1u, 22050u, 44100u};
	int16_t pcm[17u * 8u];
	uint32_t i;
	for (i = 0u; i < 17u * 8u; ++i) pcm[i] = (int16_t)((int)i * 431 - 29000);
	for (i = 0u; i < sizeof(rates) / sizeof(rates[0]); ++i)
	{
		RgsPreparedAudio out = {0};
		CHECK(rgs_audio_prepare_s16(pcm, 17u, 8u, rates[i], &out) == NULL);
		CHECK(out.frames == 17u && out.channels == 8u && out.samplerate == rates[i]);
		CHECK(out.pcm != pcm);
		if (out.pcm != NULL) CHECK(memcmp(pcm, out.pcm, sizeof(pcm)) == 0);
		free(out.pcm);
	}
}

static void test_flush_and_channels(void)
{
	const uint32_t rates[] = {48000u, 96000u, 192000u};
	const uint32_t channels[] = {1u, 2u, 8u};
	uint32_t rate_index;
	uint32_t channel_index;
	for (rate_index = 0u; rate_index < sizeof(rates) / sizeof(rates[0]); ++rate_index)
	for (channel_index = 0u; channel_index < sizeof(channels) / sizeof(channels[0]); ++channel_index)
	{
		const uint32_t rate = rates[rate_index];
		const uint32_t count = channels[channel_index];
		int16_t pcm[257u * 8u] = {0};
		RgsPreparedAudio a = {0};
		RgsPreparedAudio b = {0};
		uint32_t frame;
		uint32_t channel;
		int tail_present = 0;
		/* All energy is near EOF; failure to flush loses this entire signal. */
		pcm[253u * count] = 24000;
		CHECK(rgs_audio_prepare_s16(pcm, 257u, count, rate, &a) == NULL);
		CHECK(rgs_audio_prepare_s16(pcm, 257u, count, rate, &b) == NULL);
		CHECK(a.frames == (uint32_t)(((uint64_t)257u * 44100u + rate / 2u) / rate));
		CHECK(a.frames == b.frames && a.channels == count && a.samplerate == 44100u);
		if (a.pcm != NULL && b.pcm != NULL)
		{
			CHECK(memcmp(a.pcm, b.pcm, (size_t)a.frames * count * sizeof(int16_t)) == 0);
			for (frame = 0u; frame < a.frames; ++frame)
			{
				if (frame >= a.frames / 2u && a.pcm[(size_t)frame * count] != 0)
					tail_present = 1;
				for (channel = 1u; channel < count; ++channel)
					CHECK(a.pcm[(size_t)frame * count + channel] == 0);
			}
			CHECK(tail_present);
		}
		free(a.pcm);
		free(b.pcm);
	}
}

static double tone_rms(double frequency, uint32_t samplerate)
{
	const uint32_t frames = samplerate;
	int16_t* pcm = (int16_t*)malloc((size_t)frames * sizeof(int16_t));
	RgsPreparedAudio out = {0};
	double energy = 0.0;
	uint32_t i;
	CHECK(pcm != NULL);
	if (pcm == NULL) return 0.0;
	for (i = 0u; i < frames; ++i)
		pcm[i] = (int16_t)(12000.0 * sin(6.2831853071795864769 * frequency * i / frames));
	CHECK(rgs_audio_prepare_s16(pcm, frames, 1u, samplerate, &out) == NULL);
	CHECK(out.frames == 44100u);
	if (out.pcm != NULL && out.frames > 2048u)
	{
		for (i = 1024u; i < out.frames - 1024u; ++i)
			energy += (double)out.pcm[i] * out.pcm[i];
		energy = sqrt(energy / (out.frames - 2048u));
	}
	free(out.pcm);
	free(pcm);
	return energy;
}

static void test_passband_and_sweep(void)
{
	const double frequencies[] = {1000.0, 8000.0, 10000.0, 18000.0};
	int16_t* pcm = (int16_t*)malloc(48000u * sizeof(int16_t));
	RgsPreparedAudio out = {0};
	double error = 0.0;
	size_t i;
	for (i = 0u; i < sizeof(frequencies) / sizeof(frequencies[0]); ++i)
	{
		const double rms = tone_rms(frequencies[i], 48000u);
		/* A 12000-amplitude tone has RMS ~8485. Preserve useful game-audio
		 * treble; the previous box-filter path substantially attenuated it. */
		CHECK(rms > 8400.0 && rms < 8550.0);
	}
	CHECK(pcm != NULL);
	if (pcm == NULL) return;
	for (i = 0u; i < 48000u; ++i)
	{
		const double t = (double)i / 48000.0;
		pcm[i] = (int16_t)(12000.0 * sin(6.2831853071795864769 * (100.0 * t + 8950.0 * t * t)));
	}
	CHECK(rgs_audio_prepare_s16(pcm, 48000u, 1u, 48000u, &out) == NULL);
	CHECK(out.frames == 44100u);
	if (out.pcm != NULL && out.frames == 44100u)
	{
		for (i = 1024u; i < out.frames - 1024u; ++i)
		{
			const double t = (double)i / 44100.0;
			const double expected = 12000.0 * sin(6.2831853071795864769 * (100.0 * t + 8950.0 * t * t));
			const double difference = out.pcm[i] - expected;
			error += difference * difference;
		}
		/* Comparing at the original time origin also detects untrimmed
		 * filter delay, sample-count drift, or a shifted output timeline. */
		CHECK(sqrt(error / (44100u - 2048u)) < 8.0);
	}
	free(out.pcm);
	free(pcm);
}

static void test_alignment_and_clipping(void)
{
	int16_t pcm[4800u] = {0};
	RgsPreparedAudio out = {0};
	uint32_t frame;
	uint32_t peak_frame = 0u;
	int peak = 0;
	int sign;
	pcm[2400] = 24000;
	CHECK(rgs_audio_prepare_s16(pcm, 4800u, 1u, 48000u, &out) == NULL);
	if (out.pcm != NULL)
	{
		for (frame = 0u; frame < out.frames; ++frame)
		{
			const int magnitude = abs((int)out.pcm[frame]);
			if (magnitude > peak) { peak = magnitude; peak_frame = frame; }
		}
		CHECK(peak_frame == 2205u && peak > 16000);
	}
	free(out.pcm);
	/* Full-scale DC has filter overshoot at the endpoints. Integer output
	 * must saturate rather than wrap sign; its settled level stays exact. */
	for (sign = -1; sign <= 1; sign += 2)
	{
		const int16_t level = sign < 0 ? INT16_MIN : INT16_MAX;
		int reached_limit = 0;
		for (frame = 0u; frame < 4800u; ++frame) pcm[frame] = level;
		CHECK(rgs_audio_prepare_s16(pcm, 4800u, 1u, 48000u, &out) == NULL);
		if (out.pcm != NULL)
		{
			for (frame = 0u; frame < out.frames; ++frame)
			{
				CHECK((int)out.pcm[frame] * sign >= 0);
				if (out.pcm[frame] == level) reached_limit = 1;
				if (frame > 1500u && frame < 2900u) CHECK(abs((int)out.pcm[frame] - level) <= 1);
			}
			CHECK(reached_limit);
		}
		free(out.pcm);
	}
}

static void test_failures(void)
{
	int16_t pcm[8] = {0};
	RgsPreparedAudio out = {0};
	const uint32_t descriptions[][3] = {
		{0u, 1u, 44100u}, {1u, 0u, 44100u}, {1u, 9u, 44100u},
		{1u, 1u, 0u}, {1u, 1u, UINT32_MAX}, {1u, 1u, 192000u}};
	size_t i;
	for (i = 0u; i < sizeof(descriptions) / sizeof(descriptions[0]); ++i)
	{
		out.pcm = pcm;
		out.frames = out.channels = out.samplerate = 99u;
		CHECK(rgs_audio_prepare_s16(pcm, descriptions[i][0], descriptions[i][1], descriptions[i][2], &out) != NULL);
		CHECK(out.pcm == NULL && out.frames == 0u && out.channels == 0u && out.samplerate == 0u);
	}
	out.pcm = pcm;
	out.frames = out.channels = out.samplerate = 99u;
	CHECK(rgs_audio_prepare_s16(NULL, 1u, 1u, 44100u, &out) != NULL);
	CHECK(out.pcm == NULL && out.frames == 0u && out.channels == 0u && out.samplerate == 0u);
	CHECK(rgs_audio_prepare_s16(pcm, 1u, 1u, 44100u, NULL) != NULL);
}

int main(void)
{
	test_unchanged();
	test_flush_and_channels();
	test_failures();
	test_passband_and_sweep();
	test_alignment_and_clipping();
	CHECK(tone_rms(30000.0, 96000u) < 4.0);
	if (failures != 0) return 1;
	puts("VHQ PCM preparation tests passed.");
	return 0;
}
