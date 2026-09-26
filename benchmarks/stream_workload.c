/* Single-thread scheduling model, never a device-latency claim. Decode-ahead
 * worker and PCM ring/mix costs are timed separately. All buffers are allocated
 * before measurement; independent voices share only immutable encoded bytes. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#define _CRT_SECURE_NO_WARNINGS
#ifndef RG_RGS_HEADER
#define RG_RGS_HEADER "../src/rg_rgs.h"
#endif
#include RG_RGS_HEADER
#include "rg_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define SLOTS 4u
typedef struct Voice {
    RgRgsDecoder decoder;
    int16_t* pcm;
    uint32_t frames[SLOTS], cursor[SLOTS], producer, consumer, ready;
    uint64_t decoded, consumed, loops, starts;
    int ended;
} Voice;
static const char* arg(int argc, char** argv, const char* name, const char* fallback) {
    for (int i = 1; i + 1 < argc; ++i) if (!strcmp(argv[i], name)) return argv[i + 1];
    return fallback;
}
static int number(const char* text, uint32_t* output) {
    char* end; unsigned long parsed;
    if (!text || !*text || *text == '-') return 0;
    errno = 0; parsed = strtoul(text, &end, 10);
    if (errno || *end || !parsed || parsed > UINT32_MAX) return 0;
    *output = (uint32_t)parsed; return 1;
}
static void reset_voice(Voice* voice) {
    rg_rgs_decoder_reset(&voice->decoder);
    voice->producer = voice->consumer = voice->ready = 0;
    voice->ended = 0;
    memset(voice->frames, 0, sizeof(voice->frames));
    memset(voice->cursor, 0, sizeof(voice->cursor));
}
static int fill(Voice* voice, uint32_t channels, int loop) {
    while (voice->ready < SLOTS && !voice->ended) {
        uint32_t frames = 0;
        RgRgsDecodeStatus status = rg_rgs_decoder_next_s16(&voice->decoder,
            voice->pcm + (size_t)voice->producer * RG_RGS_MAX_FRAME_SAMPLES * channels,
            (size_t)RG_RGS_MAX_FRAME_SAMPLES * channels, &frames);
        if (status == RG_RGS_DECODE_FRAME) {
            voice->frames[voice->producer] = frames; voice->cursor[voice->producer] = 0;
            voice->producer = (voice->producer + 1u) % SLOTS; ++voice->ready;
            voice->decoded += frames;
        } else if (status == RG_RGS_DECODE_END) {
            if (loop) { rg_rgs_decoder_reset(&voice->decoder); ++voice->loops; }
            else voice->ended = 1;
        } else return 0;
    }
    return 1;
}
static int consume(Voice* voice, int32_t* output, uint32_t request, uint32_t channels) {
    uint32_t copied = 0;
    while (copied < request && voice->ready) {
        uint32_t slot = voice->consumer;
        uint32_t take = voice->frames[slot] - voice->cursor[slot];
        const int16_t* input = voice->pcm + ((size_t)slot * RG_RGS_MAX_FRAME_SAMPLES + voice->cursor[slot]) * channels;
        if (take > request - copied) take = request - copied;
        for (size_t sample = 0; sample < (size_t)take * channels; ++sample)
            output[(size_t)copied * channels + sample] += input[sample];
        voice->cursor[slot] += take; copied += take;
        if (voice->cursor[slot] == voice->frames[slot]) {
            voice->consumer = (slot + 1u) % SLOTS; --voice->ready;
        }
    }
    voice->consumed += copied;
    /* Natural EOF is silence; an unfilled callback before EOF is an underrun. */
    return copied == request || voice->ended;
}
static int compare_double(const void* left, const void* right) {
    double a = *(const double*)left, b = *(const double*)right;
    return (a > b) - (a < b);
}
static double quantile(double* values, uint32_t count, double proportion) {
    double location = (double)(count - 1u) * proportion;
    uint32_t low = (uint32_t)location, high = low + 1u < count ? low + 1u : low;
    return values[low] + (values[high] - values[low]) * (location - low);
}
int main(int argc, char** argv) {
    const char* path = arg(argc, argv, "--input", NULL);
    const char* scenario = arg(argc, argv, "--scenario", "steady");
    uint32_t voices_count, callback, blocks;
    unsigned char* encoded = NULL; Voice* voices = NULL; int16_t* storage = NULL;
    int32_t* mixed = NULL; double* worker = NULL; double* copy = NULL;
    size_t encoded_size = 0, ring_values; RgRgsInfo info; uint64_t checksum = 14695981039346656037ull;
    uint64_t decoded = 0, consumed = 0, loops = 0, starts = 0, misses = 0;
    double worker_total = 0, copy_total = 0, deadline_ms; int result = 1;
    rg_time_init();
    if (!path || !number(arg(argc, argv, "--voices", "1"), &voices_count) || voices_count > 128 ||
        !number(arg(argc, argv, "--callback-frames", "256"), &callback) || callback > 512 ||
        !number(arg(argc, argv, "--blocks", "1000"), &blocks) || blocks > 1000000 ||
        (strcmp(scenario, "steady") && strcmp(scenario, "start") && strcmp(scenario, "loop"))) {
        fprintf(stderr, "Usage: stream_workload --input file.rgs [--voices 1..128] [--callback-frames 1..512] [--blocks N] [--scenario steady|start|loop]\n"); return 2;
    }
    {
        FILE* file = fopen(path, "rb"); long length; size_t read; int closed;
        if (!file) goto cleanup;
        if (fseek(file, 0, SEEK_END) || (length = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET)) { fclose(file); goto cleanup; }
        encoded_size = (size_t)length; encoded = (unsigned char*)malloc(encoded_size);
        if (!encoded) { fclose(file); goto cleanup; }
        read = fread(encoded, 1, encoded_size, file); closed = fclose(file);
        if (read != encoded_size || closed || !rg_rgs_read_header(encoded, encoded_size, &info)) goto cleanup;
    }
    if (!strcmp(scenario, "steady")) {
        uint32_t asset_blocks = info.samples / callback + (info.samples % callback != 0);
        if (blocks > asset_blocks) blocks = asset_blocks;
    }
    /* Require one callback per asset so every refill can satisfy the next
     * callback even when one of the four slots contains only a partial tail. */
    if (!strcmp(scenario, "loop") && info.samples < callback) {
        fprintf(stderr, "Loop asset must contain at least one callback of samples\n"); goto cleanup;
    }
    ring_values = (size_t)SLOTS * RG_RGS_MAX_FRAME_SAMPLES * info.channels;
    voices = (Voice*)calloc(voices_count, sizeof(Voice));
    storage = (int16_t*)malloc(ring_values * voices_count * sizeof(int16_t));
    mixed = (int32_t*)malloc((size_t)callback * info.channels * sizeof(int32_t));
    worker = (double*)malloc((size_t)blocks * sizeof(double)); copy = (double*)malloc((size_t)blocks * sizeof(double));
    if (!voices || !storage || !mixed || !worker || !copy) goto cleanup;
    for (uint32_t v = 0; v < voices_count; ++v) {
        voices[v].pcm = storage + (size_t)v * ring_values;
        if (!rg_rgs_decoder_init(&voices[v].decoder, encoded, encoded_size, NULL) || !fill(&voices[v], info.channels, 0)) goto cleanup;
        /* Warm code/buffers, then reset all state before the measured start. */
        reset_voice(&voices[v]); voices[v].decoded = 0;
    }
    deadline_ms = (double)callback * 1000.0 / info.samplerate;
    for (uint32_t block = 0; block < blocks; ++block) {
        u64 tick = rg_time_ticks();
        for (uint32_t v = 0; v < voices_count; ++v) {
            if (!strcmp(scenario, "start") && block % 32u == 0) { reset_voice(&voices[v]); ++voices[v].starts; }
            if (!fill(&voices[v], info.channels, !strcmp(scenario, "loop"))) goto cleanup;
        }
        worker[block] = rg_time_ticks_to_ms(rg_time_ticks() - tick);
        tick = rg_time_ticks();
        memset(mixed, 0, (size_t)callback * info.channels * sizeof(int32_t));
        for (uint32_t v = 0; v < voices_count; ++v)
            if (!consume(&voices[v], mixed, callback, info.channels)) { fprintf(stderr, "Simulated ring underrun\n"); goto cleanup; }
        copy[block] = rg_time_ticks_to_ms(rg_time_ticks() - tick);
        worker_total += worker[block]; copy_total += copy[block];
        if (worker[block] + copy[block] > deadline_ms) ++misses;
        for (size_t sample = 0; sample < (size_t)callback * info.channels; ++sample) {
            checksum ^= (uint32_t)mixed[sample]; checksum *= 1099511628211ull;
        }
    }
    for (uint32_t v = 0; v < voices_count; ++v) {
        decoded += voices[v].decoded; consumed += voices[v].consumed;
        loops += voices[v].loops; starts += voices[v].starts;
    }
    qsort(worker, blocks, sizeof(double), compare_double); qsort(copy, blocks, sizeof(double), compare_double);
    printf("{\"scenario\":\"%s\",\"voices\":%u,\"callback_frames\":%u,\"blocks\":%u,\"rate\":%u,\"channels\":%u,\"worker_total_ms\":%.9f,\"copy_mix_total_ms\":%.9f,\"worker_p95_ms\":%.9f,\"worker_p99_ms\":%.9f,\"worker_max_ms\":%.9f,\"copy_mix_p95_ms\":%.9f,\"copy_mix_p99_ms\":%.9f,\"copy_mix_max_ms\":%.9f,\"simulated_budget_ms\":%.9f,\"simulated_budget_exceedances\":%llu,\"decoded_frames\":%llu,\"consumed_frames\":%llu,\"decode_ahead_loop_resets\":%llu,\"start_events\":%llu,\"ring_pcm_bytes\":%zu,\"voice_state_bytes\":%zu,\"shared_encoded_bytes\":%zu,\"mix_buffer_bytes\":%zu,\"timing_storage_bytes\":%zu,\"steady_state_allocations\":0,\"checksum\":\"%016llx\"}\n",
        scenario, voices_count, callback, blocks, info.samplerate, info.channels, worker_total, copy_total,
        quantile(worker, blocks, .95), quantile(worker, blocks, .99), worker[blocks - 1u],
        quantile(copy, blocks, .95), quantile(copy, blocks, .99), copy[blocks - 1u], deadline_ms,
        (unsigned long long)misses, (unsigned long long)decoded, (unsigned long long)consumed, (unsigned long long)loops, (unsigned long long)starts,
        ring_values * voices_count * sizeof(int16_t), (size_t)voices_count * sizeof(Voice), encoded_size,
        (size_t)callback * info.channels * sizeof(int32_t), (size_t)blocks * 2u * sizeof(double), (unsigned long long)checksum);
    result = 0;
cleanup:
    if (result) fprintf(stderr, "Stream workload failed\n");
    free(copy); free(worker); free(mixed); free(storage); free(voices); free(encoded); return result;
}
