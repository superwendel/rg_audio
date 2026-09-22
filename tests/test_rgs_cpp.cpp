#include <cstdint>
#include <cstdio>
#include <vector>

#include "rg_rgs.h"
#include "rg_rgs.h"

int main()
{
	constexpr std::uint32_t frames = 97u;
	constexpr std::uint32_t channels = 2u;
	std::vector<std::int16_t> pcm(static_cast<std::size_t>(frames) * channels);
	for (std::size_t i = 0; i < pcm.size(); i++)
	{
		pcm[i] = static_cast<std::int16_t>(static_cast<std::int32_t>((i * 3571u) & 32767u) - 16384);
	}

	const std::size_t bound = rg_rgs_encode_bound(frames, channels, 44100u);
	std::vector<std::uint8_t> encoded(bound);
	const std::size_t encoded_size =
	    rg_rgs_encode_s16(pcm.data(), frames, channels, 44100u, encoded.data(), encoded.size());
	if (encoded_size == 0u)
	{
		std::fputs("C++ encode failed\n", stderr);
		return 1;
	}

	RgRgsInfo info{};
	std::vector<std::int16_t> decoded(pcm.size());
	if (rg_rgs_decode_s16(encoded.data(), encoded_size, decoded.data(), decoded.size(), &info) != decoded.size() ||
	    info.channels != channels || info.samples != frames || info.samplerate != 44100u)
	{
		std::fputs("C++ decode failed\n", stderr);
		return 1;
	}

	RgRgsDecoder decoder{};
	std::uint32_t decoded_frames = 0u;
	if (!rg_rgs_decoder_init(&decoder, encoded.data(), encoded_size, nullptr) ||
	    rg_rgs_decoder_next_s16(&decoder, decoded.data(), decoded.size(), &decoded_frames) != RG_RGS_DECODE_FRAME ||
	    decoded_frames != frames)
	{
		std::fputs("C++ stream decode failed\n", stderr);
		return 1;
	}

	std::puts("rg_rgs C++: ok");
	return 0;
}
