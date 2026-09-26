/* Native, in-memory asset benchmark. Disk I/O and output validation are untimed.
 * RGS/QOA reuse caller-owned output; libsndfile includes its stream setup/close.
 * Define RG_RGS_HEADER to select a frozen baseline header at build time. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#define _CRT_SECURE_NO_WARNINGS
#ifndef RG_RGS_HEADER
#define RG_RGS_HEADER "../src/rg_rgs.h"
#endif
#include RG_RGS_HEADER
#include "rg_time.h"
#include "../tools/rgs_audio_prepare.h"
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
#include <sndfile.h>
#include <soxr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <math.h>

typedef struct MemoryFile { unsigned char* bytes; size_t size, capacity, position; int error; } MemoryFile;
typedef struct Audio { short* samples; uint32_t frames, channels, rate; } Audio;
typedef struct Context {
    const char* codec;
    RgRgsEncodeOptions options;
    Audio input, output;
    MemoryFile encoded;
} Context;
static volatile unsigned long long sink;
static unsigned long long checksum(const void* data, size_t size) {
    const unsigned char* bytes = (const unsigned char*)data;
    unsigned long long value = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) { value ^= bytes[i]; value *= 1099511628211ull; }
    return value;
}
static double now_ms(void) {
    return rg_time_ms();
}
static sf_count_t mem_length(void* user) { return (sf_count_t)((MemoryFile*)user)->size; }
static sf_count_t mem_tell(void* user) { return (sf_count_t)((MemoryFile*)user)->position; }
static sf_count_t mem_seek(sf_count_t offset, int whence, void* user) {
    MemoryFile* file = (MemoryFile*)user;
    sf_count_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (sf_count_t)file->position : (sf_count_t)file->size;
    if ((offset < 0 && offset < -base) || (offset > 0 && offset > (sf_count_t)file->capacity - base)) return -1;
    file->position = (size_t)(base + offset); return (sf_count_t)file->position;
}
static sf_count_t mem_read(void* ptr, sf_count_t count, void* user) {
    MemoryFile* file = (MemoryFile*)user;
    size_t available = file->position < file->size ? file->size - file->position : 0;
    size_t take = count < 0 ? 0 : (size_t)count;
    if (take > available) take = available;
    memcpy(ptr, file->bytes + file->position, take); file->position += take; return (sf_count_t)take;
}
static sf_count_t mem_write(const void* ptr, sf_count_t count, void* user) {
    MemoryFile* file = (MemoryFile*)user;
    size_t take = count < 0 ? 0 : (size_t)count;
    if (take > file->capacity - file->position) { file->error = 1; return 0; }
    memcpy(file->bytes + file->position, ptr, take); file->position += take;
    if (file->position > file->size) file->size = file->position;
    return (sf_count_t)take;
}
static SF_VIRTUAL_IO memory_io = {mem_length, mem_seek, mem_read, mem_write, mem_tell};
static int load_audio(const char* path, Audio* audio) {
    SF_INFO info = {0}; SNDFILE* file = sf_open(path, SFM_READ, &info);
    if (!file) { fprintf(stderr, "%s: %s\n", path, sf_strerror(NULL)); return 0; }
    if (info.frames <= 0 || info.frames > UINT32_MAX || info.channels < 1 || info.channels > (int)RG_RGS_MAX_CHANNELS || info.samplerate <= 0 ||
        (uint64_t)info.frames * (unsigned)info.channels > SIZE_MAX / sizeof(short)) { sf_close(file); return 0; }
    audio->frames = (uint32_t)info.frames; audio->channels = (uint32_t)info.channels; audio->rate = (uint32_t)info.samplerate;
    audio->samples = (short*)malloc((size_t)audio->frames * audio->channels * sizeof(short));
    if (!audio->samples) { sf_close(file); return 0; }
    if (sf_readf_short(file, audio->samples, info.frames) != info.frames) { sf_close(file); free(audio->samples); audio->samples = NULL; return 0; }
    return sf_close(file) == 0;
}
static int save_audio(const char* path, const Audio* audio) {
    SF_INFO info = {0}; SNDFILE* file; sf_count_t written;
    info.channels = (int)audio->channels; info.samplerate = (int)audio->rate; info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    file = sf_open(path, SFM_WRITE, &info); if (!file) return 0;
    written = sf_writef_short(file, audio->samples, audio->frames);
    return sf_close(file) == 0 && written == audio->frames;
}
static int encode(Context* c) {
    c->encoded.position = c->encoded.size = 0; c->encoded.error = 0;
    if (!strcmp(c->codec, "rgs")) {
        c->encoded.size = rg_rgs_encode_s16_ex(c->input.samples, c->input.frames, c->input.channels, c->input.rate,
            c->encoded.bytes, c->encoded.capacity, &c->options);
        return c->encoded.size != 0;
    }
    if (!strcmp(c->codec, "qoa")) {
        qoa_desc q = {0}; unsigned int at = 0, position;
        q.channels = c->input.channels; q.samples = c->input.frames; q.samplerate = c->input.rate;
        for (unsigned int ch = 0; ch < q.channels; ++ch) { q.lms[ch].weights[2] = -(1 << 13); q.lms[ch].weights[3] = 1 << 14; }
        position = qoa_encode_header(&q, c->encoded.bytes);
        while (at < q.samples) {
            unsigned int count = q.samples - at; if (count > QOA_FRAME_LEN) count = QOA_FRAME_LEN;
            position += qoa_encode_frame(c->input.samples + (size_t)at * q.channels, &q, count, c->encoded.bytes + position);
            at += count;
        }
        c->encoded.size = position; return position != 0;
    }
    {
        SF_INFO info = {0}; SNDFILE* file; sf_count_t written; int closed;
        info.channels = (int)c->input.channels; info.samplerate = (int)c->input.rate;
        info.format = SF_FORMAT_WAV | (!strcmp(c->codec, "ima") ? SF_FORMAT_IMA_ADPCM : SF_FORMAT_PCM_16);
        file = sf_open_virtual(&memory_io, SFM_WRITE, &info, &c->encoded); if (!file) return 0;
        written = sf_writef_short(file, c->input.samples, c->input.frames); closed = sf_close(file);
        return written == c->input.frames && !closed && !c->encoded.error;
    }
}
static int decode(Context* c) {
    if (!strcmp(c->codec, "rgs")) {
        RgRgsInfo info;
        size_t written = rg_rgs_decode_s16(c->encoded.bytes, c->encoded.size, c->output.samples,
            (size_t)c->input.frames * c->input.channels, &info);
        if (!written) return 0;
        c->output.frames = info.samples; c->output.channels = info.channels; c->output.rate = info.samplerate;
        return written == (size_t)info.samples * info.channels;
    }
    if (!strcmp(c->codec, "qoa")) {
        qoa_desc q = {0}; unsigned int at = 0, position;
        if (c->encoded.size > INT_MAX) return 0;
        position = qoa_decode_header(c->encoded.bytes, (int)c->encoded.size, &q); if (!position) return 0;
        while (at < q.samples) {
            unsigned int count = 0;
            unsigned int used = qoa_decode_frame(c->encoded.bytes + position, (unsigned int)(c->encoded.size - position), &q,
                c->output.samples + (size_t)at * q.channels, &count);
            if (!used || !count || count > q.samples - at) return 0;
            position += used; at += count;
        }
        c->output.frames = q.samples; c->output.channels = q.channels; c->output.rate = q.samplerate;
        return position == c->encoded.size;
    }
    {
        SF_INFO info = {0}; SNDFILE* file; sf_count_t count; int closed;
        c->encoded.position = 0;
        file = sf_open_virtual(&memory_io, SFM_READ, &info, &c->encoded); if (!file) return 0;
        count = sf_readf_short(file, c->output.samples, c->input.frames); closed = sf_close(file);
        /* IMA WAV's last block may contain padding; compare only the source timeline. */
        c->output.frames = c->input.frames; c->output.channels = (uint32_t)info.channels; c->output.rate = (uint32_t)info.samplerate;
        return count == c->input.frames && !closed;
    }
}
/* Validate all public RGS decode paths outside every timed region. The checked
 * warmup has already accepted this exact immutable buffer before trusted use. */
static int validate_rgs_decoders(const Context* c) {
    const size_t values = (size_t)c->output.frames * c->output.channels;
    short* scratch = (short*)malloc(values * sizeof(short));
    RgRgsInfo info; RgRgsDecoder decoder; size_t position = 0; int ok = 0;
    if (!scratch) return 0;
    if (rg_rgs_decode_trusted_s16(c->encoded.bytes, c->encoded.size, scratch, values, &info) != values ||
        info.samples != c->output.frames || info.channels != c->output.channels || info.samplerate != c->output.rate ||
        memcmp(scratch, c->output.samples, values * sizeof(short))) goto done;
    if (!rg_rgs_decoder_init(&decoder, c->encoded.bytes, c->encoded.size, NULL)) goto done;
    for (;;) {
        uint32_t frames = 0;
        RgRgsDecodeStatus status = rg_rgs_decoder_next_s16(&decoder, scratch + position, values - position, &frames);
        if (status == RG_RGS_DECODE_END) {
            if (frames || position != values) goto done;
            break;
        }
        if (status != RG_RGS_DECODE_FRAME || !frames ||
            (size_t)frames * c->output.channels > values - position) goto done;
        position += (size_t)frames * c->output.channels;
    }
    ok = memcmp(scratch, c->output.samples, values * sizeof(short)) == 0;
done:
    free(scratch);
    return ok;
}
static double measure(Context* c, int is_encode, double minimum_ms, unsigned int* count) {
    double start = now_ms(), elapsed; *count = 0;
    do {
        int result = is_encode ? encode(c) : decode(c); if (!result) return -1;
        ++*count;
        sink += is_encode ? (unsigned long long)c->encoded.size : (unsigned short)c->output.samples[(size_t)c->output.frames * c->output.channels - 1];
        elapsed = now_ms() - start;
    } while (elapsed < minimum_ms && *count < 1000000u);
    return elapsed / *count;
}
static double measure_frame_api(Context* c, int first_only, double minimum_ms, unsigned int* output_frames) {
    double start = now_ms(), elapsed; unsigned int count = 0;
    if (strcmp(c->codec, "rgs") && strcmp(c->codec, "qoa")) return -1;
    do {
        if (!strcmp(c->codec, "rgs")) {
            RgRgsDecoder decoder; uint32_t frames = 0, total = 0;
            if (!rg_rgs_decoder_init(&decoder, c->encoded.bytes, c->encoded.size, NULL)) return -1;
            for (;;) {
                RgRgsDecodeStatus status = rg_rgs_decoder_next_s16(&decoder, c->output.samples,
                    (size_t)c->input.frames * c->input.channels, &frames);
                if (status == RG_RGS_DECODE_END) break;
                if (status != RG_RGS_DECODE_FRAME) return -1;
                total += frames;
                sink += (unsigned short)c->output.samples[(size_t)frames * c->input.channels - 1];
                if (first_only) break;
            }
            if (!first_only && total != c->output.frames) return -1;
            *output_frames = total;
        } else {
            qoa_desc q = {0}; unsigned int frames = 0;
            unsigned int position = qoa_decode_header(c->encoded.bytes, (int)c->encoded.size, &q);
            if (!position || !qoa_decode_frame(c->encoded.bytes + position, (unsigned int)(c->encoded.size - position), &q, c->output.samples, &frames)) return -1;
            sink += (unsigned short)c->output.samples[(size_t)frames * q.channels - 1];
            *output_frames = frames;
        }
        ++count; elapsed = now_ms() - start;
    } while (elapsed < minimum_ms && count < 1000000u);
    return elapsed / count;
}
static const char* argument(int argc, char** argv, const char* key, const char* fallback) {
    for (int i = 1; i + 1 < argc; ++i) if (!strcmp(argv[i], key)) return argv[i + 1];
    return fallback;
}
int main(int argc, char** argv) {
    Context c = {0}; const char* input = argument(argc, argv, "--input", NULL);
    const char* encoded_path = argument(argc, argv, "--encoded", NULL);
    const char* decoded_path = argument(argc, argv, "--decoded", NULL);
    const char* prepare_path = argument(argc, argv, "--prepare-output", NULL);
    const char* quality = argument(argc, argv, "--quality", "medium");
    const char* min_text = argument(argc, argv, "--min-ms", "10"); char* min_end;
    double minimum_ms = strtod(min_text, &min_end), encode_ms, decode_ms;
    double first_ms = -1, stream_ms = -1; unsigned int first_frames = 0, stream_frames = 0;
    char first_json[64] = "null", stream_json[64] = "null", state_json[64] = "null", ring_json[64] = "null";
    unsigned int encode_count, decode_count; int result = 1;
    c.codec = argument(argc, argv, "--codec", "rgs"); c.options = rg_rgs_default_options();
    if (!strcmp(quality, "high")) c.options.quality = RG_RGS_QUALITY_HIGH;
    else if (!strcmp(quality, "low")) c.options.quality = RG_RGS_QUALITY_LOW;
    else if (strcmp(quality, "medium")) { fprintf(stderr, "Invalid quality\n"); return 2; }
    rg_time_init();
    if (!input || min_end == min_text || *min_end || !isfinite(minimum_ms) || minimum_ms < 0 || minimum_ms > 60000 ||
        (strcmp(c.codec, "rgs") && strcmp(c.codec, "qoa") && strcmp(c.codec, "pcm") && strcmp(c.codec, "ima"))) {
        fprintf(stderr, "Usage: codec_adapter --input file.wav --codec rgs|qoa|pcm|ima [--quality high|medium|low] [--min-ms 10] [--encoded file] [--decoded file.wav]\n"); return 2;
    }
    if (!load_audio(input, &c.input)) goto cleanup;
    if (prepare_path) {
        RgsPreparedAudio prepared = {0};
        const char* error = rgs_audio_prepare_s16(c.input.samples, c.input.frames, c.input.channels, c.input.rate, &prepared);
        if (error) { fprintf(stderr, "%s\n", error); goto cleanup; }
        c.output.samples = prepared.pcm; c.output.frames = prepared.frames; c.output.channels = prepared.channels; c.output.rate = prepared.samplerate;
        if (!save_audio(prepare_path, &c.output)) goto cleanup;
        printf("{\"source_frames\":%u,\"source_rate\":%u,\"frames\":%u,\"rate\":%u,\"channels\":%u,\"soxr_version\":\"%s\"}\n",
            c.input.frames, c.input.rate, c.output.frames, c.output.rate, c.output.channels, soxr_version());
        result = 0; goto cleanup;
    }
    if ((size_t)c.input.frames * c.input.channels > (SIZE_MAX - 65536u) / (2u * sizeof(short))) goto cleanup;
    c.encoded.capacity = (size_t)c.input.frames * c.input.channels * 2u * sizeof(short) + 65536u;
    c.encoded.bytes = (unsigned char*)malloc(c.encoded.capacity);
    c.output.samples = (short*)malloc((size_t)c.input.frames * c.input.channels * sizeof(short));
    if (!c.encoded.bytes || !c.output.samples) goto cleanup;
    /* One untimed full operation primes code and buffers for every trial. */
    if (!encode(&c) || !decode(&c)) goto cleanup;
    if (!strcmp(c.codec, "rgs") && !validate_rgs_decoders(&c)) goto cleanup;
    encode_ms = measure(&c, 1, minimum_ms, &encode_count); if (encode_ms < 0) goto cleanup;
    decode_ms = measure(&c, 0, minimum_ms, &decode_count); if (decode_ms < 0) goto cleanup;
    if (!strcmp(c.codec, "rgs") || !strcmp(c.codec, "qoa")) {
        first_ms = measure_frame_api(&c, 1, minimum_ms, &first_frames); if (first_ms < 0) goto cleanup;
        snprintf(first_json, sizeof(first_json), "%.9f", first_ms);
        snprintf(state_json, sizeof(state_json), "%zu", !strcmp(c.codec, "rgs") ? sizeof(RgRgsDecoder) : sizeof(qoa_desc));
        snprintf(ring_json, sizeof(ring_json), "%zu", (size_t)4u * RG_RGS_MAX_FRAME_SAMPLES * c.input.channels * sizeof(short));
    }
    if (!strcmp(c.codec, "rgs")) {
        stream_ms = measure_frame_api(&c, 0, minimum_ms, &stream_frames); if (stream_ms < 0) goto cleanup;
        snprintf(stream_json, sizeof(stream_json), "%.9f", stream_ms);
    }
    /* Frame microbenchmarks reuse the output buffer; restore the complete output for validation/export. */
    if (!decode(&c)) goto cleanup;
    if (c.output.channels != c.input.channels || (!strcmp(c.codec, "pcm") && memcmp(c.output.samples, c.input.samples,
        (size_t)c.input.frames * c.input.channels * sizeof(short)))) goto cleanup;
    if (encoded_path) {
        FILE* file = fopen(encoded_path, "wb"); size_t written; int closed;
        if (!file) goto cleanup;
        written = fwrite(c.encoded.bytes, 1, c.encoded.size, file); closed = fclose(file);
        if (written != c.encoded.size || closed) goto cleanup;
    }
    if (decoded_path && !save_audio(decoded_path, &c.output)) goto cleanup;
    printf("{\"codec\":\"%s\",\"quality\":\"%s\",\"source_frames\":%u,\"source_rate\":%u,\"channels\":%u,\"stored_frames\":%u,\"stored_rate\":%u,\"encoded_bytes\":%zu,\"encode_ms\":%.9f,\"decode_ms\":%.9f,\"encode_batch\":%u,\"decode_batch\":%u,\"warmups\":1,\"output_buffer_bytes\":%zu,\"init_first_frame_ms\":%s,\"first_frame_samples\":%u,\"stream_decode_ms\":%s,\"decoder_state_bytes\":%s,\"four_slot_pcm_bytes\":%s,\"internal_peak_allocation_bytes\":null,\"encoded_fnv64\":\"%016llx\",\"decoded_fnv64\":\"%016llx\",\"sndfile_version\":\"%s\",\"soxr_version\":\"%s\",\"decode_apis_agree\":%s,\"sink\":%llu}\n",
        c.codec, quality, c.input.frames, c.input.rate, c.input.channels, c.output.frames, c.output.rate, c.encoded.size,
        encode_ms, decode_ms, encode_count, decode_count, (size_t)c.input.frames * c.input.channels * sizeof(short), first_json, first_frames, stream_json, state_json, ring_json,
        checksum(c.encoded.bytes, c.encoded.size), checksum(c.output.samples, (size_t)c.output.frames * c.output.channels * sizeof(short)), sf_version_string(), soxr_version(), !strcmp(c.codec, "rgs") ? "true" : "null", sink);
    result = 0;
cleanup:
    if (result) fprintf(stderr, "Codec operation failed: codec=%s input=%s (%s)\n", c.codec, input ? input : "", sf_strerror(NULL));
    free(c.output.samples); free(c.encoded.bytes); free(c.input.samples); return result;
}
