#include <stddef.h>
#include <stdint.h>

#include "rg_rgs.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	RgRgsInfo info;
	RgRgsDecoder decoder;
	int16_t frame[RG_RGS_MAX_FRAME_SAMPLES * RG_RGS_MAX_CHANNELS];
	uint32_t frames;
	uint32_t iterations = 0u;

	(void)rg_rgs_read_header(data, size, &info);
	if (rg_rgs_decoder_init(&decoder, data, size, &info))
	{
		for (;;)
		{
			RgRgsDecodeStatus status = rg_rgs_decoder_next_s16(
			    &decoder,
			    frame,
			    sizeof(frame) / sizeof(frame[0]),
			    &frames);
			iterations++;
			if (status != RG_RGS_DECODE_FRAME || iterations > 1048576u)
			{
				break;
			}
		}
		rg_rgs_decoder_reset(&decoder);
	}

	if (size <= 1024u)
	{
		int16_t small[4096];
		(void)rg_rgs_decode_s16(data, size, small, sizeof(small) / sizeof(small[0]), NULL);
	}
	return 0;
}
