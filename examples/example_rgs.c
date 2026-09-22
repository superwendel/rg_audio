#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "rg_rgs.h"

int main(void)
{
	enum
	{
		FRAMES = 800,
		CHANNELS = 2
	};
	int16_t pcm[FRAMES * CHANNELS];
	uint32_t frame;
	uint32_t channel;
	size_t bound;
	uint8_t* encoded;
	size_t encoded_size;
	int16_t decoded[FRAMES * CHANNELS];
	RgRgsInfo info;

	for (frame = 0u; frame < FRAMES; frame++)
	{
		/* A dependency-free triangle tone, interleaved as stereo PCM16. */
		int32_t phase = (int32_t)(frame % 200u);
		int16_t sample = (int16_t)((phase < 100 ? phase : 200 - phase) * 240 - 12000);
		for (channel = 0u; channel < CHANNELS; channel++)
		{
			pcm[(size_t)frame * CHANNELS + channel] = sample;
		}
	}

	bound = rg_rgs_encode_bound(FRAMES, CHANNELS, 44100u);
	encoded = (uint8_t*)malloc(bound);
	if (encoded == NULL)
	{
		return 1;
	}
	encoded_size = rg_rgs_encode_s16(pcm, FRAMES, CHANNELS, 44100u, encoded, bound);
	if (encoded_size == 0u ||
	    rg_rgs_decode_s16(encoded,
	                      encoded_size,
	                      decoded,
	                      sizeof(decoded) / sizeof(decoded[0]),
	                      &info) == 0u)
	{
		free(encoded);
		return 1;
	}

	printf("RGS v%u: %u channels, %u Hz, %u frames, %zu bytes\n",
	       (unsigned)RG_RGS_VERSION,
	       (unsigned)info.channels,
	       (unsigned)info.samplerate,
	       (unsigned)info.samples,
	       encoded_size);
	free(encoded);
	return 0;
}
