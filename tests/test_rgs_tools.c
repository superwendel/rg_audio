#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "../src/rg_rgs.h"

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

static int write_test_wav(const char* path)
{
	enum
	{
		CHANNELS = 2,
		FRAMES = 257,
		SAMPLERATE = 48000
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
	write_u32le(file, SAMPLERATE);
	write_u32le(file, SAMPLERATE * CHANNELS * (uint32_t)sizeof(int16_t));
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

int main(int argc, char** argv)
{
	char input_path[96];
	char output_path[96];
	char invalid_path[96];
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
	(void)remove(input_path);
	(void)remove(output_path);
	(void)remove(invalid_path);

	CHECK(write_test_wav(input_path));
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
		CHECK(info.samples > 0u);
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
