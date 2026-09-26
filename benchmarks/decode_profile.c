/* Decode-only profiling of already encoded corpus assets. No encoding, file
 * I/O, allocation, checksum scan, or prevalidation occurs inside the timed
 * batch. Checked RGS and QOA include header parsing on every whole-asset pass.
 * Parse-only intentionally uses private frame validation to isolate its cost. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef RG_RGS_HEADER
#define RG_RGS_HEADER "../src/rg_rgs.h"
#endif
#include RG_RGS_HEADER
#include "rg_time.h"
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

#define PROFILE_STRING_INNER(x) #x
#define PROFILE_STRING(x) PROFILE_STRING_INNER(x)
#if defined(__clang__)
#define PROFILE_COMPILER "clang " __clang_version__
#elif defined(_MSC_FULL_VER)
#define PROFILE_COMPILER "MSVC " PROFILE_STRING(_MSC_FULL_VER)
#elif defined(__GNUC__)
#define PROFILE_COMPILER "GCC " __VERSION__
#else
#define PROFILE_COMPILER "unknown"
#endif

typedef enum ProfileMode { MODE_CHECKED, MODE_TRUSTED, MODE_PARSE, MODE_QOA } ProfileMode;
typedef struct Profile {
    unsigned char* bytes;
    size_t size;
    int16_t* pcm;
    size_t values;
    RgRgsInfo info;
    ProfileMode mode;
    uint32_t frame_count;
} Profile;
static volatile uint64_t profile_sink;
static void compiler_barrier(void) {
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#elif defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" ::: "memory");
#endif
}

static uint64_t checksum(const void* data, size_t size) {
    const unsigned char* bytes = (const unsigned char*)data;
    uint64_t value = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) { value ^= bytes[i]; value *= UINT64_C(1099511628211); }
    return value;
}

/* Validate every frame, declared sample total and exact end of stream. The
 * public header parser also validates frame one; duplicate work is retained
 * here to match the checked public decode entrypoint's parsing behavior. */
static int parse_all(Profile* profile) {
    RgRgsInfo info;
    size_t position = RG_RGS_HEADER_SIZE;
    uint32_t decoded = 0, frames = 0;
    if (!rg_rgs_read_header(profile->bytes, profile->size, &info)) return 0;
    while (decoded < info.samples) {
        RgRgsFrameMeta frame;
        if (position > profile->size || !rg_rgs_parse_frame(profile->bytes + position,
                profile->size - position, &info, info.samples - decoded, &frame)) return 0;
        if (!frame.samples || frame.samples > info.samples - decoded || frame.frame_size > profile->size - position) return 0;
        decoded += frame.samples; position += frame.frame_size; ++frames;
    }
    if (position != profile->size || decoded != info.samples) return 0;
    profile->info = info; profile->frame_count = frames;
    return 1;
}

static int decode_qoa(Profile* profile) {
    qoa_desc description = {0};
    unsigned int position, decoded = 0, frames = 0;
    if (profile->size > INT_MAX) return 0;
    position = qoa_decode_header(profile->bytes, (int)profile->size, &description);
    if (!position || description.samples != profile->info.samples ||
        description.channels != profile->info.channels || description.samplerate != profile->info.samplerate) return 0;
    while (decoded < description.samples) {
        unsigned int count = 0, used;
        if ((size_t)position >= profile->size) return 0;
        /* The upstream frame API takes no destination capacity. Reject an
         * oversized final frame before it can write beyond the output. */
        if (profile->size - position < 8u ||
            (((unsigned int)profile->bytes[position + 4u] << 8u) | profile->bytes[position + 5u]) > description.samples - decoded) return 0;
        used = qoa_decode_frame(profile->bytes + position, (unsigned int)(profile->size - position),
            &description, profile->pcm + (size_t)decoded * description.channels, &count);
        if (!used || !count || count > description.samples - decoded || used > profile->size - position) return 0;
        position += used; decoded += count; ++frames;
    }
    if ((size_t)position != profile->size) return 0;
    profile->frame_count = frames;
    return 1;
}

static int operation(Profile* profile) {
    size_t values;
    switch (profile->mode) {
        case MODE_PARSE: return parse_all(profile);
        case MODE_QOA: return decode_qoa(profile);
        case MODE_TRUSTED:
            values = rg_rgs_decode_trusted_s16(profile->bytes, profile->size, profile->pcm, profile->values, NULL);
            break;
        default:
            values = rg_rgs_decode_s16(profile->bytes, profile->size, profile->pcm, profile->values, NULL);
            break;
    }
    return values == profile->values;
}

static int load(const char* path, Profile* profile) {
    FILE* file = fopen(path, "rb"); long length; size_t read; int closed;
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return 0; }
    profile->size = (size_t)length; profile->bytes = (unsigned char*)malloc(profile->size);
    if (!profile->bytes) { fclose(file); return 0; }
    read = fread(profile->bytes, 1, profile->size, file); closed = fclose(file);
    return read == profile->size && !closed;
}

static void json_string(const char* value) {
    putchar('"');
    for (; *value; ++value) {
        unsigned char ch = (unsigned char)*value;
        if (ch == '"' || ch == '\\') { putchar('\\'); putchar(ch); }
        else if (ch < 32) printf("\\u%04x", (unsigned int)ch);
        else putchar(ch);
    }
    putchar('"');
}

int main(int argc, char** argv) {
    Profile profile = {0}; const char* path = NULL; const char* mode = "checked";
    double minimum_ms = 10, elapsed; uint32_t batch = 0; int result = 1;
    uint64_t checked_checksum = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--input") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "--min-ms") && i + 1 < argc) {
            char* end; const char* text = argv[++i]; errno = 0; minimum_ms = strtod(text, &end);
            if (errno || end == text || *end || !isfinite(minimum_ms) || minimum_ms < 0 || minimum_ms > 60000) return 2;
        } else { fprintf(stderr, "Invalid argument: %s\n", argv[i]); return 2; }
    }
    if (!strcmp(mode, "checked")) profile.mode = MODE_CHECKED;
    else if (!strcmp(mode, "trusted")) profile.mode = MODE_TRUSTED;
    else if (!strcmp(mode, "parse")) profile.mode = MODE_PARSE;
    else if (!strcmp(mode, "qoa")) profile.mode = MODE_QOA;
    else { fprintf(stderr, "Unknown mode\n"); return 2; }
    if (!path) { fprintf(stderr, "Usage: decode_profile --input file.rgs|qoa [--mode checked|trusted|parse|qoa] [--min-ms 10]\n"); return 2; }
    rg_time_init();
    if (!load(path, &profile)) goto cleanup;
    if (profile.mode == MODE_QOA) {
        qoa_desc description = {0};
        if (profile.size > INT_MAX || !qoa_decode_header(profile.bytes, (int)profile.size, &description) ||
            !description.samples || !description.channels || description.channels > RG_RGS_MAX_CHANNELS) goto cleanup;
        profile.info.samples = description.samples; profile.info.channels = description.channels; profile.info.samplerate = description.samplerate;
    } else if (!parse_all(&profile)) goto cleanup;
    if ((uint64_t)profile.info.samples * profile.info.channels > SIZE_MAX / sizeof(int16_t)) goto cleanup;
    profile.values = (size_t)profile.info.samples * profile.info.channels;
    /* Parse mode allocates and writes no PCM output. Trusted mode is allowed
     * only after the entire immutable RGS stream was checked above. */
    if (profile.mode != MODE_PARSE) {
        profile.pcm = (int16_t*)malloc(profile.values * sizeof(int16_t));
        if (!profile.pcm) goto cleanup;
    }
    if (profile.mode == MODE_TRUSTED) {
        /* Honor the public trusted-decoder contract with a successful full
         * checked decode of this exact immutable buffer, not only parsing. */
        if (rg_rgs_decode_s16(profile.bytes, profile.size, profile.pcm, profile.values, NULL) != profile.values) goto cleanup;
        checked_checksum = checksum(profile.pcm, profile.values * sizeof(int16_t));
    }
    if (!operation(&profile)) goto cleanup; /* Untimed warmup. */
    if (profile.mode == MODE_TRUSTED && checked_checksum != checksum(profile.pcm, profile.values * sizeof(int16_t))) goto cleanup;
    {
        u64 started = rg_time_ticks();
        do {
            compiler_barrier();
            if (!operation(&profile)) goto cleanup;
            profile_sink += profile.mode == MODE_PARSE ? profile.frame_count : (uint16_t)profile.pcm[profile.values - 1];
            ++batch; elapsed = rg_time_ticks_to_ms(rg_time_ticks() - started);
        } while (elapsed < minimum_ms && batch < 1000000u);
    }
    printf("{\"mode\":\"%s\",\"elapsed_ms\":%.9f,\"mean_ms\":%.9f,\"batch\":%u,\"warmups\":1,\"checked_decode_preflight\":%s,\"frames\":%u,\"codec_frames\":%u,\"channels\":%u,\"rate\":%u,\"encoded_bytes\":%zu,\"output_buffer_bytes\":%zu,\"decoded_fnv64\":",
        mode, elapsed, elapsed / batch, batch, profile.mode == MODE_TRUSTED ? "true" : "false", profile.info.samples, profile.frame_count, profile.info.channels,
        profile.info.samplerate, profile.size, profile.mode == MODE_PARSE ? 0 : profile.values * sizeof(int16_t));
    if (profile.mode == MODE_PARSE) printf("null");
    else printf("\"%016llx\"", (unsigned long long)checksum(profile.pcm, profile.values * sizeof(int16_t)));
    printf(",\"encoded_fnv64\":\"%016llx\",\"compiler\":", (unsigned long long)checksum(profile.bytes, profile.size));
    json_string(PROFILE_COMPILER);
    printf(",\"sink\":%llu}\n", (unsigned long long)profile_sink);
    result = 0;
cleanup:
    if (result) fprintf(stderr, "Profile failed or stream invalid: %s (%s)\n", path, mode);
    free(profile.pcm); free(profile.bytes); return result;
}
