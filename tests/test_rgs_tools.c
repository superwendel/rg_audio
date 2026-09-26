#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#define SDL_MAIN_HANDLED
#include "../tools/rgs_tool_io.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;

#define CHECK(condition)                                                         \
	do                                                                           \
	{                                                                            \
		if (!(condition))                                                        \
		{                                                                        \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			++failures;                                                          \
		}                                                                        \
	} while (0)

static void write_u16le(FILE* file, uint16_t value)
{
	uint8_t bytes[2];
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8u);
	(void)fwrite(bytes, 1u, sizeof(bytes), file);
}

static void write_u32le(FILE* file, uint32_t value)
{
	uint8_t bytes[4];
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8u);
	bytes[2] = (uint8_t)(value >> 16u);
	bytes[3] = (uint8_t)(value >> 24u);
	(void)fwrite(bytes, 1u, sizeof(bytes), file);
}

static int write_test_wav(const char* path, uint32_t samplerate)
{
	enum
	{
		CHANNELS = 2,
		FRAMES = 257
	};
	const uint32_t data_size = FRAMES * CHANNELS * (uint32_t)sizeof(int16_t);
	FILE* file = fopen(path, "wb");
	uint32_t frame;
	if (file == NULL)
	{
		return 0;
	}
	(void)fwrite("RIFF", 1u, 4u, file);
	write_u32le(file, 36u + data_size);
	(void)fwrite("WAVEfmt ", 1u, 8u, file);
	write_u32le(file, 16u);
	write_u16le(file, 1u);
	write_u16le(file, CHANNELS);
	write_u32le(file, samplerate);
	write_u32le(file, samplerate * CHANNELS * (uint32_t)sizeof(int16_t));
	write_u16le(file, CHANNELS * (uint16_t)sizeof(int16_t));
	write_u16le(file, 16u);
	(void)fwrite("data", 1u, 4u, file);
	write_u32le(file, data_size);
	for (frame = 0u; frame < FRAMES; ++frame)
	{
		const int16_t left = (int16_t)((int32_t)(frame * 193u) - 24000);
		const int16_t right = (int16_t)(24000 - (int32_t)(frame * 149u));
		write_u16le(file, (uint16_t)left);
		write_u16le(file, (uint16_t)right);
	}
	return fclose(file) == 0;
}

static int write_pcm8_wav(const char* path)
{
	const uint8_t pcm[] = {0u, 255u, 128u, 128u, 64u, 192u, 1u, 254u};
	FILE* file = fopen(path, "wb");
	int ok;
	if (file == NULL)
	{
		return 0;
	}
	(void)fwrite("RIFF", 1u, 4u, file);
	write_u32le(file, 36u + (uint32_t)sizeof(pcm));
	(void)fwrite("WAVEfmt ", 1u, 8u, file);
	write_u32le(file, 16u);
	write_u16le(file, 1u);
	write_u16le(file, 2u);
	write_u32le(file, 22050u);
	write_u32le(file, 44100u);
	write_u16le(file, 2u);
	write_u16le(file, 8u);
	(void)fwrite("data", 1u, 4u, file);
	write_u32le(file, (uint32_t)sizeof(pcm));
	ok = fwrite(pcm, 1u, sizeof(pcm), file) == sizeof(pcm);
	if (fclose(file) != 0)
	{
		ok = 0;
	}
	return ok;
}

static void test_shared_wav_loader(const char* path)
{
	const int16_t expected_pcm8[] = {-32768, 32512, 0, 0, -16384, 16384, -32512, 32256};
	RgsPreparedAudio prepared = {0};
	RgsWavSource source = {0};
	uint32_t frame;
	int loaded;
	CHECK(write_test_wav(path, 22050u));
	loaded = rgs_tool_load_wav(path, &prepared, &source);
	CHECK(loaded);
	if (loaded)
	{
		CHECK(source.frames == 257u && source.channels == 2u && source.samplerate == 22050u);
		CHECK(prepared.frames == 257u && prepared.channels == 2u && prepared.samplerate == 22050u);
		if (prepared.pcm != NULL && prepared.frames == 257u && prepared.channels == 2u)
		{
			for (frame = 0u; frame < 257u; ++frame)
			{
				CHECK(prepared.pcm[frame * 2u] == (int16_t)((int32_t)(frame * 193u) - 24000));
				CHECK(prepared.pcm[frame * 2u + 1u] == (int16_t)(24000 - (int32_t)(frame * 149u)));
			}
		}
		else
		{
			CHECK(0);
		}
		free(prepared.pcm);
	}
	/* SDL's unsigned PCM8 conversion must preserve channel order and sign. */
	CHECK(write_pcm8_wav(path));
	loaded = rgs_tool_load_wav(path, &prepared, NULL);
	CHECK(loaded);
	if (loaded)
	{
		CHECK(prepared.frames == 4u && prepared.channels == 2u && prepared.samplerate == 22050u);
		if (prepared.pcm != NULL && prepared.frames == 4u && prepared.channels == 2u)
		{
			CHECK(memcmp(prepared.pcm, expected_pcm8, sizeof(expected_pcm8)) == 0);
		}
		else
		{
			CHECK(0);
		}
		free(prepared.pcm);
	}
	/* Source metadata describes the input even when preparation resamples it. */
	CHECK(write_test_wav(path, 48000u));
	loaded = rgs_tool_load_wav(path, &prepared, &source);
	CHECK(loaded);
	if (loaded)
	{
		CHECK(source.frames == 257u && source.channels == 2u && source.samplerate == 48000u);
		CHECK(prepared.frames == 236u && prepared.channels == 2u && prepared.samplerate == 44100u);
		free(prepared.pcm);
	}
}

static int read_file(const char* path, uint8_t** out_data, size_t* out_size)
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

static int run_converter(const char* executable,
                         const char* input,
                         const char* output,
                         const char* arguments)
{
	const size_t capacity = strlen(executable) + strlen(input) + strlen(output) +
	                        strlen(arguments) + 20u;
	char* command = (char*)malloc(capacity);
	int status;
	if (command == NULL)
	{
		return 0;
	}
#if defined(_WIN32)
	/* cmd.exe needs an outer quote when the command itself begins quoted. */
	(void)snprintf(command,
	               capacity,
	               "\"\"%s\" \"%s\" \"%s\" %s\"",
	               executable,
	               input,
	               output,
	               arguments);
#else
	(void)snprintf(command,
	               capacity,
	               "\"%s\" \"%s\" \"%s\" %s",
	               executable,
	               input,
	               output,
	               arguments);
#endif
	status = system(command);
	free(command);
	return status == 0;
}

static void test_failed_conversion(const char* executable, const char* input, const char* output)
{
	const char malformed[] = "This is not a WAV file";
	const uint8_t sentinel[] = {0x72u, 0x67u, 0x73u, 0x00u, 0xffu, 0x21u};
	int16_t unused_pcm = 0;
	RgsPreparedAudio prepared = {&unused_pcm, 99u, 99u, 99u};
	RgsWavSource source = {99u, 99u, 99u};
	uint8_t* actual = NULL;
	size_t actual_size = 0u;
	CHECK(SDL_SaveFile(input, malformed, sizeof(malformed)));
	CHECK(SDL_SaveFile(output, sentinel, sizeof(sentinel)));
	SDL_ClearError();
	CHECK(!rgs_tool_load_wav(input, &prepared, &source));
	CHECK(prepared.pcm == NULL && prepared.frames == 0u &&
	      prepared.channels == 0u && prepared.samplerate == 0u);
	CHECK(source.frames == 0u && source.channels == 0u && source.samplerate == 0u);
	CHECK(SDL_GetError()[0] != '\0');
	CHECK(!run_converter(executable, input, output, "--quality medium"));
	CHECK(read_file(output, &actual, &actual_size));
	if (actual != NULL)
	{
		CHECK(actual_size == sizeof(sentinel));
		if (actual_size == sizeof(sentinel))
		{
			CHECK(memcmp(actual, sentinel, sizeof(sentinel)) == 0);
		}
		free(actual);
	}
}

typedef struct TemporaryFileCheck
{
	const char* destination;
	unsigned count;
} TemporaryFileCheck;

static SDL_EnumerationResult SDLCALL count_temporary_files(void* userdata,
                                                          const char* dirname,
                                                          const char* filename)
{
	TemporaryFileCheck* check = (TemporaryFileCheck*)userdata;
	(void)dirname;
	if (strncmp(filename, check->destination, strlen(check->destination)) == 0 &&
	    strcmp(filename, check->destination) != 0)
	{
		++check->count;
	}
	return SDL_ENUM_CONTINUE;
}

static void test_failed_rename(const char* directory)
{
	const uint8_t data[] = {1u, 2u, 3u, 4u};
	TemporaryFileCheck temporary = {directory, 0u};
	SDL_PathInfo info = {0};
	const int created = SDL_CreateDirectory(directory);
	CHECK(created);
	if (!created)
	{
		return;
	}
	SDL_ClearError();
	CHECK(!rgs_tool_write_atomic(directory, data, sizeof(data)));
	CHECK(SDL_GetError()[0] != '\0');
	CHECK(SDL_GetPathInfo(directory, &info));
	CHECK(info.type == SDL_PATHTYPE_DIRECTORY);
	CHECK(SDL_EnumerateDirectory(".", count_temporary_files, &temporary));
	CHECK(temporary.count == 0u);
	CHECK(SDL_RemovePath(directory));
}

static void test_utf8_write_and_replace(const char* path)
{
	const uint8_t first[] = {0u, 1u, 255u};
	const uint8_t replacement[] = {0x72u, 0u, 0x67u, 0xffu, 0x21u};
	unsigned pass;
	for (pass = 0u; pass < 2u; ++pass)
	{
		const uint8_t* expected = pass == 0u ? first : replacement;
		const size_t expected_size = pass == 0u ? sizeof(first) : sizeof(replacement);
		size_t actual_size = 0u;
		void* actual;
		CHECK(rgs_tool_write_atomic(path, expected, expected_size));
		actual = SDL_LoadFile(path, &actual_size);
		CHECK(actual != NULL);
		CHECK(actual_size == expected_size);
		if (actual != NULL && actual_size == expected_size)
		{
			CHECK(memcmp(actual, expected, expected_size) == 0);
		}
		SDL_free(actual);
	}
	CHECK(SDL_RemovePath(path));
}

int main(int argc, char** argv)
{
	char input_path[96];
	char output_path[96];
	char invalid_path[96];
	char directory_path[96];
	char utf8_path[96];
	uint8_t* encoded = NULL;
	size_t encoded_size = 0u;
	RgRgsInfo info = {0};
	int16_t* decoded = NULL;
	size_t decoded_values;
	const unsigned long suffix = (unsigned long)clock();

	if (argc != 2)
	{
		fprintf(stderr, "Usage: %s path-to-rgs_convert\n", argv[0]);
		return 2;
	}
	(void)snprintf(input_path, sizeof(input_path), "rgs_tool_%lu_input.wav", suffix);
	(void)snprintf(output_path, sizeof(output_path), "rgs_tool_%lu_output.rgs", suffix);
	(void)snprintf(invalid_path, sizeof(invalid_path), "rgs_tool_%lu_output.rgsx", suffix);
	/* UTF-8 bytes are explicit so this test does not depend on source encoding. */
	(void)snprintf(directory_path, sizeof(directory_path), "rgs_tool_%lu_\xe9\x9f\xb3\xe9\xa2\x91_directory.rgs", suffix);
	(void)snprintf(utf8_path, sizeof(utf8_path), "rgs_tool_%lu_\xe9\x9f\xb3\xe9\xa2\x91.rgs", suffix);
	(void)remove(input_path);
	(void)remove(output_path);
	(void)remove(invalid_path);
	test_shared_wav_loader(input_path);
	test_failed_conversion(argv[1], input_path, output_path);
	test_failed_rename(directory_path);
	test_utf8_write_and_replace(utf8_path);

	CHECK(write_test_wav(input_path, 48000u));
	CHECK(run_converter(argv[1], input_path, output_path, "--quality high --target-kbps 180"));
	/* Replacing an existing destination exercises the atomic publish path. */
	CHECK(run_converter(argv[1], input_path, output_path, "--quality low --target-kbps 128"));
	CHECK(read_file(output_path, &encoded, &encoded_size));
	if (encoded != NULL)
	{
		CHECK(encoded_size >= RG_RGS_HEADER_SIZE);
		CHECK(encoded[8] == RG_RGS_VERSION);
		CHECK(rg_rgs_read_header(encoded, encoded_size, &info));
		CHECK(info.channels == 2u);
		CHECK(info.samplerate == RG_RGS_MAX_STORED_SAMPLERATE);
		CHECK(info.samples == 236u);
		decoded_values = (size_t)info.samples * info.channels;
		decoded = (int16_t*)malloc(decoded_values * sizeof(int16_t));
		CHECK(decoded != NULL);
		if (decoded != NULL)
		{
			CHECK(rg_rgs_decode_s16(encoded,
			                        encoded_size,
			                        decoded,
			                        decoded_values,
			                        NULL) == decoded_values);
		}
	}

	CHECK(!run_converter(argv[1], input_path, invalid_path, "--quality medium"));
	{
		FILE* should_not_exist = fopen(invalid_path, "rb");
		CHECK(should_not_exist == NULL);
		if (should_not_exist != NULL)
		{
			(void)fclose(should_not_exist);
		}
	}

	free(decoded);
	free(encoded);
	{
		const uint32_t rates[] = {22050u, 44100u, 96000u, 192000u};
		size_t rate_index;
		for (rate_index = 0u; rate_index < sizeof(rates) / sizeof(rates[0]); ++rate_index)
		{
			const uint32_t source_rate = rates[rate_index];
			const uint32_t stored_rate = source_rate > 44100u ? 44100u : source_rate;
			const uint32_t expected_frames = (uint32_t)(((uint64_t)257u * stored_rate + source_rate / 2u) / source_rate);
			encoded = NULL;
			CHECK(write_test_wav(input_path, source_rate));
			CHECK(run_converter(argv[1], input_path, output_path, "--quality medium"));
			CHECK(read_file(output_path, &encoded, &encoded_size));
			if (encoded != NULL)
			{
				CHECK(rg_rgs_read_header(encoded, encoded_size, &info));
				CHECK(info.samplerate == stored_rate && info.samples == expected_frames && info.channels == 2u);
				free(encoded);
			}
		}
	}
	(void)remove(invalid_path);
	(void)remove(output_path);
	(void)remove(input_path);
	if (failures != 0)
	{
		fprintf(stderr, "%d tool test(s) failed.\n", failures);
		return 1;
	}
	printf("rgs_convert integration tests passed.\n");
	return 0;
}
