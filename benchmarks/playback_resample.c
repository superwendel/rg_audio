/* Actual SDL3 AudioStream conversion cost, without an audio device. PCM loading
 * is untimed. SDL queueing/allocation, conversion, and draining are included;
 * this is not an allocation-free codec or end-to-end latency measurement. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#define _CRT_SECURE_NO_WARNINGS
#include "rg_time.h"
#include <SDL3/SDL.h>
#include <sndfile.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct Trial {
    double init_ms, put_ms, get_ms, flush_ms;
    uint64_t output_frames;
    uint32_t first_output_input_frames;
    uint64_t checksum;
} Trial;
static int integer(const char* text, uint32_t* value) {
    char* end; unsigned long parsed;
    if (!*text || *text == '-') return 0;
    errno = 0; parsed = strtoul(text, &end, 10);
    if (errno || *end || parsed == 0 || parsed > UINT32_MAX) return 0;
    *value = (uint32_t)parsed; return 1;
}
static int drain(SDL_AudioStream* stream, short* output, int capacity, uint32_t channels, Trial* trial) {
    for (;;) {
        u64 started = rg_time_ticks();
        int read = SDL_GetAudioStreamData(stream, output, capacity);
        trial->get_ms += rg_time_ticks_to_ms(rg_time_ticks() - started);
        if (read < 0 || read % (int)(channels * sizeof(short))) return 0;
        if (!read) return 1;
        trial->output_frames += (unsigned int)read / (channels * sizeof(short));
        for (size_t sample = 0; sample < (size_t)read / sizeof(short); ++sample) {
            trial->checksum ^= (uint16_t)output[sample]; trial->checksum *= UINT64_C(1099511628211);
        }
    }
}
static int run(const short* samples, uint32_t frames, uint32_t rate, uint32_t channels,
               uint32_t block, short* output, int capacity, Trial* trial) {
    SDL_AudioSpec source = {SDL_AUDIO_S16, (int)channels, (int)rate};
    SDL_AudioSpec target = {SDL_AUDIO_S16, (int)channels, 48000};
    u64 started = rg_time_ticks();
    SDL_AudioStream* stream = SDL_CreateAudioStream(&source, &target);
    trial->init_ms = rg_time_ticks_to_ms(rg_time_ticks() - started);
    trial->checksum = UINT64_C(14695981039346656037);
    if (!stream) return 0;
    for (uint32_t position = 0; position < frames;) {
        uint32_t take = frames - position; bool success;
        if (take > block) take = block;
        started = rg_time_ticks();
        success = SDL_PutAudioStreamData(stream, samples + (size_t)position * channels, (int)(take * channels * sizeof(short)));
        trial->put_ms += rg_time_ticks_to_ms(rg_time_ticks() - started);
        if (!success || !drain(stream, output, capacity, channels, trial)) { SDL_DestroyAudioStream(stream); return 0; }
        position += take;
        if (!trial->first_output_input_frames && trial->output_frames) trial->first_output_input_frames = position;
    }
    started = rg_time_ticks();
    {
        bool success = SDL_FlushAudioStream(stream);
        trial->flush_ms = rg_time_ticks_to_ms(rg_time_ticks() - started);
        if (!success || !drain(stream, output, capacity, channels, trial)) { SDL_DestroyAudioStream(stream); return 0; }
    }
    if (!trial->first_output_input_frames && trial->output_frames) trial->first_output_input_frames = frames;
    SDL_DestroyAudioStream(stream);
    /* SDL may round the final rate-converted duration by one sample. */
    {
        uint64_t expected = ((uint64_t)frames * 48000u) / rate;
        uint64_t difference = trial->output_frames > expected ? trial->output_frames - expected : expected - trial->output_frames;
        return difference <= 1u;
    }
}
int main(int argc, char** argv) {
    uint32_t block = 256, iterations = 7;
    SF_INFO info = {0}; SNDFILE* file; short* samples = NULL; short* output = NULL; Trial* trials = NULL;
    size_t values; int result = 1, output_capacity; Trial warmup = {0};
    rg_time_init();
    if (argc < 2 || argc > 4 || (argc > 2 && !integer(argv[2], &block)) ||
        (argc > 3 && !integer(argv[3], &iterations)) || block > 4096 || iterations > 1000) {
        fprintf(stderr, "Usage: playback_resample input-pcm.wav [input-block-frames=256] [trials=7]\n"); return 2;
    }
    file = sf_open(argv[1], SFM_READ, &info);
    if (!file) { fprintf(stderr, "%s\n", sf_strerror(NULL)); return 1; }
    if (info.frames <= 0 || info.frames > UINT32_MAX || info.channels <= 0 || info.channels > 8 ||
        info.samplerate <= 0 || (uint64_t)info.frames * (unsigned int)info.channels > SIZE_MAX / sizeof(short)) { sf_close(file); goto cleanup; }
    values = (size_t)info.frames * (unsigned int)info.channels;
    samples = (short*)malloc(values * sizeof(short));
    if (!samples) { sf_close(file); goto cleanup; }
    {
        sf_count_t count = sf_readf_short(file, samples, info.frames); int closed = sf_close(file);
        if (count != info.frames || closed) goto cleanup;
    }
    output_capacity = 8192 * info.channels * (int)sizeof(short);
    output = (short*)malloc((size_t)output_capacity); trials = (Trial*)calloc(iterations, sizeof(Trial));
    if (!output || !trials || !run(samples, (uint32_t)info.frames, (uint32_t)info.samplerate, (uint32_t)info.channels,
                                  block, output, output_capacity, &warmup)) goto cleanup;
    for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
        if (!run(samples, (uint32_t)info.frames, (uint32_t)info.samplerate, (uint32_t)info.channels, block, output, output_capacity, &trials[iteration]) ||
            trials[iteration].output_frames != warmup.output_frames || trials[iteration].checksum != warmup.checksum) goto cleanup;
    }
    printf("{\"sdl_version\":%d,\"source_rate\":%d,\"device_rate\":48000,\"channels\":%d,\"source_frames\":%u,\"input_block_frames\":%u,\"warmups\":1,\"internal_peak_allocation_bytes\":null,\"scope\":\"SDL AudioStream queue/conversion/drain; no device\",\"trials\":[",
        SDL_GetVersion(), info.samplerate, info.channels, (uint32_t)info.frames, block);
    for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
        Trial* trial = trials + iteration;
        printf("%s{\"trial\":%u,\"init_ms\":%.9f,\"put_ms\":%.9f,\"get_ms\":%.9f,\"flush_ms\":%.9f,\"conversion_total_ms\":%.9f,\"output_frames\":%llu,\"first_output_input_frames\":%u,\"checksum\":\"%016llx\"}",
            iteration ? "," : "", iteration, trial->init_ms, trial->put_ms, trial->get_ms, trial->flush_ms,
            trial->put_ms + trial->get_ms + trial->flush_ms, (unsigned long long)trial->output_frames,
            trial->first_output_input_frames, (unsigned long long)trial->checksum);
    }
    printf("]}\n"); result = 0;
cleanup:
    if (result) fprintf(stderr, "SDL playback conversion failed: %s\n", SDL_GetError());
    free(trials); free(output); free(samples); return result;
}
