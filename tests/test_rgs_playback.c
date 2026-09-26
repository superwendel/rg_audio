/* Optional target-host test: actual SDL output callbacks, not a simulated
 * device. Decode and mix real asset PCM, verify it, then submit silence.
 * No audio, video, or GPU device is opened unless this executable is run.
 * The player source supplies the exact worker, prefill, atomics, and ring.
 * Its renamed application entry point is never called by this test. */
#define main rgs_player_application_main
#include "../tools/rgs_player.c"
#undef main

#define PLAYBACK_MAX_VOICES 64u
#define PLAYBACK_MAX_LOAD_THREADS 64u
#define PLAYBACK_CHUNK_VALUES (RGS_PLAYER_AUDIO_CHUNK_FRAMES * 2u)
#define PLAYBACK_STARTUP_NS UINT64_C(250000000)

typedef struct PlaybackPhase
{
	uint64_t callbacks;
	uint64_t requested_voice_frames;
	uint64_t consumed_voice_frames;
	uint64_t queued_output_frames;
	uint64_t underruns;
} PlaybackPhase;

typedef struct PlaybackTest
{
	RgsPlayerTrack track;
	RgsPlayer* voices;
	uint32_t voice_count;
	int16_t* reference;
	SDL_AudioStream* audio;
	SDL_AtomicInt failed;
	SDL_AtomicInt stop_load;
	SDL_AtomicU32 load_sink;
	SDL_Thread* load_threads[PLAYBACK_MAX_LOAD_THREADS];
	uint32_t load_count;
	uint64_t resumed_ns;
	uint64_t first_callback_ns;
	uint64_t max_callback_ns;
	uint64_t over_budget_callbacks;
	uint64_t pcm_mismatches;
	uint64_t mix_mismatches;
	uint64_t submit_errors;
	uint64_t nonzero_mixed_values;
	uint64_t mixed_fnv64;
	PlaybackPhase phases[2];
	int16_t voice_pcm[PLAYBACK_CHUNK_VALUES];
	int16_t output_pcm[PLAYBACK_CHUNK_VALUES];
	int32_t mixed[PLAYBACK_CHUNK_VALUES];
	int32_t expected_mix[PLAYBACK_CHUNK_VALUES];
} PlaybackTest;

static int playback_number(const char* value, uint32_t low, uint32_t high, uint32_t* out)
{
	char* end;
	unsigned long parsed;
	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (value[0] == '\0' || value[0] == '-' || *end != '\0' || errno != 0 || parsed < low || parsed > high)
		return 0;
	*out = (uint32_t)parsed;
	return 1;
}

static void playback_json_string(const char* text)
{
	const unsigned char* p = (const unsigned char*)(text != NULL ? text : "");
	putchar('"');
	for (; *p != 0u; ++p)
	{
		if (*p == '"' || *p == '\\')
		{
			putchar('\\');
			putchar(*p);
		}
		else if (*p < 32u)
			printf("\\u%04x", (unsigned)*p);
		else
			putchar(*p);
	}
	putchar('"');
}

static int SDLCALL playback_load_thread(void* userdata)
{
	PlaybackTest* test = (PlaybackTest*)userdata;
	uint32_t state = 1u;
	while (!SDL_GetAtomicInt(&test->stop_load))
	{
		for (uint32_t i = 0u; i < 50000u; ++i)
			state = state * 1664525u + 1013904223u;
		SDL_SetAtomicU32(&test->load_sink, state);
	}
	return 0;
}

static void SDLCALL playback_callback(void* userdata, SDL_AudioStream* stream,
                                     int additional_amount, int total_amount)
{
	PlaybackTest* test = (PlaybackTest*)userdata;
	uint32_t channels = test->track.info.channels;
	int bytes_per_frame = (int)(channels * sizeof(int16_t));
	uint64_t begin = SDL_GetTicksNS();
	uint64_t requested;
	PlaybackPhase* phase;
	(void)total_amount;
	if (additional_amount < bytes_per_frame)
		return;
	requested = (uint64_t)(additional_amount / bytes_per_frame);
	phase = &test->phases[begin - test->resumed_ns >= PLAYBACK_STARTUP_NS ? 1 : 0];
	phase->callbacks++;
	if (test->first_callback_ns == 0u)
		test->first_callback_ns = begin;
	while (additional_amount >= bytes_per_frame)
	{
		uint32_t frames = (uint32_t)(additional_amount / bytes_per_frame);
		size_t values;
		if (frames > RGS_PLAYER_AUDIO_CHUNK_FRAMES)
			frames = RGS_PLAYER_AUDIO_CHUNK_FRAMES;
		values = (size_t)frames * channels;
		memset(test->mixed, 0, values * sizeof(*test->mixed));
		memset(test->expected_mix, 0, values * sizeof(*test->expected_mix));
		for (uint32_t v = 0u; v < test->voice_count; ++v)
		{
			RgsPlayer* voice = &test->voices[v];
			uint64_t cursor = voice->timeline.cursor;
			uint64_t underruns = voice->timeline.underruns;
			uint32_t produced = rgs_player_stream_pull(&voice->ring, &voice->timeline,
			    test->track.wav_pcm, test->voice_pcm, frames);
			phase->requested_voice_frames += frames;
			phase->consumed_voice_frames += produced;
			phase->underruns += voice->timeline.underruns - underruns;
			for (uint32_t f = 0u; f < frames; ++f)
			{
				size_t reference_frame = (size_t)((cursor + f) % test->track.info.samples);
				for (uint32_t c = 0u; c < channels; ++c)
				{
					size_t index = (size_t)f * channels + c;
					int16_t expected = (int16_t)(f < produced ? test->reference[reference_frame * channels + c] : 0);
					if (test->voice_pcm[index] != expected)
						test->pcm_mismatches++;
					test->mixed[index] += test->voice_pcm[index];
					test->expected_mix[index] += expected;
				}
			}
		}
		for (size_t i = 0u; i < values; ++i)
		{
			uint16_t sample;
			/* At most 64 signed16 voices fit in int32_t; the average fits s16. */
			if (test->mixed[i] != test->expected_mix[i])
				test->mix_mismatches++;
			test->output_pcm[i] = (int16_t)(test->mixed[i] / (int32_t)test->voice_count);
			if (test->output_pcm[i] != (int16_t)(test->expected_mix[i] / (int32_t)test->voice_count))
				test->mix_mismatches++;
			test->nonzero_mixed_values += test->output_pcm[i] != 0;
			sample = (uint16_t)test->output_pcm[i];
			test->mixed_fnv64 = (test->mixed_fnv64 ^ (sample & 255u)) * UINT64_C(1099511628211);
			test->mixed_fnv64 = (test->mixed_fnv64 ^ (sample >> 8u)) * UINT64_C(1099511628211);
		}
		/* Silence is applied only after consuming, validating, and mixing PCM. */
		memset(test->output_pcm, 0, values * sizeof(*test->output_pcm));
		if (!SDL_PutAudioStreamData(stream, test->output_pcm, (int)(values * sizeof(int16_t))))
		{
			test->submit_errors++;
			SDL_SetAtomicInt(&test->failed, 1);
			break;
		}
		phase->queued_output_frames += frames;
		additional_amount -= (int)frames * bytes_per_frame;
	}
	if (test->pcm_mismatches != 0u || test->mix_mismatches != 0u)
		SDL_SetAtomicInt(&test->failed, 1);
	{
		uint64_t elapsed = SDL_GetTicksNS() - begin;
		if (elapsed > test->max_callback_ns)
			test->max_callback_ns = elapsed;
		if (elapsed * test->track.info.samplerate > requested * UINT64_C(1000000000))
			test->over_budget_callbacks++;
	}
}

static void playback_phase_json(const PlaybackPhase* phase)
{
	printf("{\"callbacks\":%llu,\"requested_voice_frames\":%llu,\"consumed_voice_frames\":%llu,"
	       "\"queued_output_frames\":%llu,\"underruns\":%llu}",
	       (unsigned long long)phase->callbacks, (unsigned long long)phase->requested_voice_frames,
	       (unsigned long long)phase->consumed_voice_frames, (unsigned long long)phase->queued_output_frames,
	       (unsigned long long)phase->underruns);
}

int main(int argc, char** argv)
{
	const char* input = NULL;
	const char* quality_name = "medium";
	RgRgsQuality quality = RG_RGS_QUALITY_MEDIUM;
	uint32_t voices = 1u, load_threads = 0u, seconds = 5u;
	PlaybackTest* test = NULL;
	SDL_AudioSpec spec, device_spec;
	SDL_AudioDeviceID device = 0;
	int device_frames = 0;
	uint32_t workers_started = 0u, worker_errors = 0u;
	int initialized = 0, device_opened = 0, passed = 0;
	uint64_t startup_begin = 0u, startup_ns = 0u, elapsed_ns = 0u;
	char driver_name[128] = "", device_name[512] = "", error[1024] = "";
	memset(&device_spec, 0, sizeof(device_spec));
	for (int i = 1; i < argc; ++i)
	{
		if (strcmp(argv[i], "--input") == 0 && i + 1 < argc)
			input = argv[++i];
		else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc && playback_number(argv[i + 1], 1u, 60u, &seconds))
			++i;
		else if (strcmp(argv[i], "--voices") == 0 && i + 1 < argc && playback_number(argv[i + 1], 1u, PLAYBACK_MAX_VOICES, &voices))
			++i;
		else if (strcmp(argv[i], "--load-threads") == 0 && i + 1 < argc && playback_number(argv[i + 1], 0u, PLAYBACK_MAX_LOAD_THREADS, &load_threads))
			++i;
		else if (strcmp(argv[i], "--quality") == 0 && i + 1 < argc)
		{
			quality_name = argv[++i];
			if (strcmp(quality_name, "high") == 0) quality = RG_RGS_QUALITY_HIGH;
			else if (strcmp(quality_name, "medium") == 0) quality = RG_RGS_QUALITY_MEDIUM;
			else if (strcmp(quality_name, "low") == 0) quality = RG_RGS_QUALITY_LOW;
			else goto usage;
		}
		else goto usage;
	}
	if (input == NULL) goto usage;
	test = (PlaybackTest*)calloc(1u, sizeof(*test));
	if (test == NULL) return 1;
	test->voice_count = voices;
	test->mixed_fnv64 = UINT64_C(14695981039346656037);
	if (!SDL_Init(SDL_INIT_AUDIO)) goto cleanup;
	initialized = 1;
	{
		const char* driver = SDL_GetCurrentAudioDriver();
		if (driver == NULL) goto cleanup;
		SDL_strlcpy(driver_name, driver, sizeof(driver_name));
	}
	if (strcmp(driver_name, "dummy") == 0 || strcmp(driver_name, "disk") == 0)
	{
		SDL_SetError("A real output backend is required; dummy/disk is not a device test");
		goto cleanup;
	}
	if (!rgs_player_load_track(input, &test->track)) goto cleanup;
	if (test->track.info.channels < 1u || test->track.info.channels > 2u)
	{
		SDL_SetError("This host test requires a mono or stereo WAV");
		goto cleanup;
	}
	if (quality != RG_RGS_QUALITY_MEDIUM)
	{
		RgRgsEncodeOptions options = rg_rgs_default_options();
		size_t bound = rg_rgs_encode_bound(test->track.info.samples, test->track.info.channels, test->track.info.samplerate);
		options.quality = quality;
		test->track.encoded_size = rg_rgs_encode_s16_ex(test->track.wav_pcm, test->track.info.samples,
		    test->track.info.channels, test->track.info.samplerate, test->track.encoded, bound, &options);
		if (test->track.encoded_size == 0u)
		{
			SDL_SetError("Requested quality could not be encoded");
			goto cleanup;
		}
	}
	{
		size_t values = (size_t)test->track.info.samples * test->track.info.channels;
		test->reference = (int16_t*)malloc(values * sizeof(*test->reference));
		if (test->reference == NULL || rg_rgs_decode_s16(test->track.encoded, test->track.encoded_size,
		    test->reference, values, NULL) != values)
		{
			SDL_SetError("Checked PCM reference decode failed");
			goto cleanup;
		}
	}
	test->voices = (RgsPlayer*)calloc(voices, sizeof(*test->voices));
	if (test->voices == NULL)
	{
		SDL_SetError("Could not allocate voice state");
		goto cleanup;
	}
	startup_begin = SDL_GetTicksNS();
	for (uint32_t v = 0u; v < voices; ++v)
	{
		RgsPlayer* voice = &test->voices[v];
		voice->track = test->track; /* Borrow immutable PCM/encoded storage. */
		rgs_player_stream_ring_init(&voice->ring, test->track.info.channels);
		rgs_player_stream_timeline_init(&voice->timeline, test->track.info.samples, test->track.info.channels);
		voice->timeline.source = RGS_PLAYER_STREAM_SOURCE_RGS;
		if (!rgs_player_start_worker(voice))
		{
			worker_errors++;
			goto cleanup;
		}
		workers_started++;
	}
	spec.format = SDL_AUDIO_S16;
	spec.channels = (int)test->track.info.channels;
	spec.freq = (int)test->track.info.samplerate;
	test->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, playback_callback, test);
	if (test->audio == NULL) goto cleanup;
	device_opened = 1;
	device = SDL_GetAudioStreamDevice(test->audio);
	{
		const char* name = SDL_GetAudioDeviceName(device);
		if (name != NULL) SDL_strlcpy(device_name, name, sizeof(device_name));
	}
	if (!SDL_GetAudioDeviceFormat(device, &device_spec, &device_frames)) goto cleanup;
	for (uint32_t i = 0u; i < load_threads; ++i)
	{
		test->load_threads[i] = SDL_CreateThread(playback_load_thread, "RGS playback test load", test);
		if (test->load_threads[i] == NULL) goto cleanup;
		test->load_count++;
	}
	startup_ns = SDL_GetTicksNS() - startup_begin;
	test->resumed_ns = SDL_GetTicksNS();
	if (!SDL_ResumeAudioStreamDevice(test->audio)) goto cleanup;
	while (SDL_GetTicksNS() - test->resumed_ns < PLAYBACK_STARTUP_NS + (uint64_t)seconds * UINT64_C(1000000000))
	{
		if (SDL_GetAtomicInt(&test->failed)) break;
		for (uint32_t v = 0u; v < workers_started; ++v)
			if (SDL_GetAtomicInt(&test->voices[v].worker.state) == RGS_PLAYER_WORKER_FAILED)
				SDL_SetAtomicInt(&test->failed, 1);
		SDL_Delay(10u);
	}
	elapsed_ns = SDL_GetTicksNS() - test->resumed_ns;
	if (!SDL_PauseAudioStreamDevice(test->audio) || !SDL_LockAudioStream(test->audio)) goto cleanup;
	passed = !SDL_GetAtomicInt(&test->failed) && worker_errors == 0u &&
	         test->phases[0].underruns == 0u && test->phases[1].underruns == 0u &&
	         test->phases[0].requested_voice_frames == test->phases[0].consumed_voice_frames &&
	         test->phases[1].callbacks >= 2u && test->nonzero_mixed_values != 0u &&
	         test->phases[1].consumed_voice_frames >= (uint64_t)seconds * test->track.info.samplerate * voices * 8u / 10u &&
	         test->phases[1].requested_voice_frames == test->phases[1].consumed_voice_frames;
	SDL_UnlockAudioStream(test->audio);
	if (!passed) SDL_SetError("Playback validation failed; inspect progress, PCM, worker and underrun counters");

cleanup:
	if (!passed) SDL_strlcpy(error, SDL_GetError(), sizeof(error));
	if (startup_begin != 0u && startup_ns == 0u) startup_ns = SDL_GetTicksNS() - startup_begin;
	if (test->audio != NULL)
	{
		(void)SDL_PauseAudioStreamDevice(test->audio);
		SDL_DestroyAudioStream(test->audio); /* Join/stop callback before reading counters. */
		test->audio = NULL;
	}
	SDL_SetAtomicInt(&test->stop_load, 1);
	for (uint32_t i = 0u; i < test->load_count; ++i)
		SDL_WaitThread(test->load_threads[i], NULL);
	for (uint32_t v = 0u; v < workers_started; ++v)
	{
		rgs_player_stop_worker(&test->voices[v]);
		worker_errors += SDL_GetAtomicInt(&test->voices[v].worker.state) == RGS_PLAYER_WORKER_FAILED;
	}
	if (worker_errors != 0u)
	{
		passed = 0;
		if (error[0] == '\0') SDL_strlcpy(error, "Decoder worker failed", sizeof(error));
	}
	printf("{\"passed\":%s,\"real_device_opened\":%s,\"silent_after_pcm_checks\":true,\"input\":", passed ? "true" : "false", device_opened ? "true" : "false");
	playback_json_string(input);
	printf(",\"error\":"); playback_json_string(error);
	printf(",\"driver\":"); playback_json_string(driver_name);
	printf(",\"device\":"); playback_json_string(device_name);
	printf(",\"quality\":"); playback_json_string(quality_name);
	printf(",\"channels\":%u,\"source_rate\":%u,\"source_frames\":%u,\"encoded_bytes\":%zu,"
	       "\"device_rate\":%d,\"device_channels\":%d,\"device_sample_frames\":%d,\"voices\":%u,\"workers_prefilled\":%u,\"load_threads\":%u,"
	       "\"requested_running_seconds\":%u,\"startup_ms\":%.3f,\"elapsed_ms\":%.3f,\"first_callback_ms\":%.3f,"
	       "\"callback_max_ms_including_validation\":%.6f,\"over_budget_callbacks\":%llu,\"callback_deadline_met\":%s,\"worker_errors\":%u,"
	       "\"pcm_mismatches\":%llu,\"mix_mismatches\":%llu,\"submit_errors\":%llu,\"nonzero_mixed_values\":%llu,"
	       "\"mixed_fnv64\":\"%016llx\",\"startup\":",
	       test->track.info.channels, test->track.info.samplerate, test->track.info.samples, test->track.encoded_size,
	       device_spec.freq, device_spec.channels, device_frames, voices, workers_started, load_threads, seconds,
	       (double)startup_ns / 1e6, (double)elapsed_ns / 1e6,
	       test->first_callback_ns != 0u ? (double)(test->first_callback_ns - test->resumed_ns) / 1e6 : 0.0,
	       (double)test->max_callback_ns / 1e6, (unsigned long long)test->over_budget_callbacks,
	       test->phases[1].callbacks >= 2u && test->over_budget_callbacks == 0u ? "true" : "false", worker_errors,
	       (unsigned long long)test->pcm_mismatches, (unsigned long long)test->mix_mismatches,
	       (unsigned long long)test->submit_errors, (unsigned long long)test->nonzero_mixed_values,
	       (unsigned long long)test->mixed_fnv64);
	playback_phase_json(&test->phases[0]);
	printf(",\"running\":"); playback_phase_json(&test->phases[1]);
	printf("}\n");
	free(test->voices);
	free(test->reference);
	rgs_player_track_destroy(&test->track);
	free(test);
	if (initialized) SDL_Quit();
	return passed ? 0 : 1;

usage:
	fprintf(stderr, "Usage: test_rgs_playback --input mono-or-stereo.wav [--seconds 1..60] [--voices 1..64] [--load-threads 0..64] [--quality high|medium|low]\n");
	return 2;
}
