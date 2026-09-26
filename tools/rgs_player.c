/*
 * rgs_player - rg_gui/SDL3 A/B player for a WAV reference and an in-memory
 * RGS v1 encoding. No sidecar is read or written unless Save RGS is pressed
 * or --write-sidecar is supplied.
 */

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#ifndef RGINLINE
#define RGINLINE static inline
#endif
#ifndef RG_SPRINTF_NO_ASM
#define RG_SPRINTF_NO_ASM 1
#endif

#include "rg_rgs.h"
#include "rgs_audio_prepare.h"
#include "rg_gui_gpu.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RGS_PLAYER_STREAM_ATOMIC_TYPE SDL_AtomicU32
#define RGS_PLAYER_STREAM_ATOMIC_LOAD(value) SDL_GetAtomicU32(value)
#define RGS_PLAYER_STREAM_ATOMIC_STORE(value, desired) SDL_SetAtomicU32((value), (Uint32)(desired))
#include "rgs_player_stream.h"

#ifndef RGS_PLAYER_GPU_DEBUG
#define RGS_PLAYER_GPU_DEBUG 0
#endif

#define RGS_PLAYER_PATH_CAPACITY 1024u
#define RGS_PLAYER_NAME_CAPACITY 256u
#define RGS_PLAYER_ENVELOPE_BUCKETS 512u
#define RGS_PLAYER_AUDIO_CHUNK_FRAMES 1024u
#define RGS_PLAYER_PREFILL_SLOTS 2u

#define RGS_PLAYER_FONT_GLYPH_CAPACITY 256u
#define RGS_PLAYER_FONT_KERNING_CAPACITY 4096u
#define RGS_PLAYER_FONT_STEM "inter_medium_16"

typedef struct RgsPlayerFile
{
	char* path;
	char* label;
} RgsPlayerFile;

typedef struct RgsPlayerFileList
{
	RgsPlayerFile* items;
	const char** labels;
	size_t count;
	size_t capacity;
} RgsPlayerFileList;

typedef struct RgsPlayerTrack
{
	char wav_path[RGS_PLAYER_PATH_CAPACITY];
	char sidecar_path[RGS_PLAYER_PATH_CAPACITY];
	char name[RGS_PLAYER_NAME_CAPACITY];
	int16_t* wav_pcm;
	uint8_t* encoded;
	size_t encoded_size;
	uint64_t wav_bytes;
	RgRgsInfo info;
	float envelope[RGS_PLAYER_ENVELOPE_BUCKETS];
} RgsPlayerTrack;

typedef struct RgsPlayerFontAssets
{
	RgTextFont font;
	RgTextGlyph glyphs[RGS_PLAYER_FONT_GLYPH_CAPACITY];
	RgTextKerning kernings[RGS_PLAYER_FONT_KERNING_CAPACITY];
	u8* pixels;
	u32 atlas_width;
	u32 atlas_height;
} RgsPlayerFontAssets;

typedef struct RgsPlayerCursorState
{
	SDL_Cursor* cursors[5];
	RgGuiMouseCursor applied;
	int has_applied;
} RgsPlayerCursorState;

struct RgsPlayer;

typedef struct RgsPlayerDecodeWorker
{
	struct RgsPlayer* player;
	SDL_Thread* thread;
	SDL_AtomicInt stop;
	SDL_AtomicInt state;
} RgsPlayerDecodeWorker;

typedef struct RgsPlayer
{
	SDL_AudioStream* audio;
	RgsPlayerFileList files;
	RgsPlayerTrack track;
	RgsPlayerStreamRing ring;
	RgsPlayerStreamTimeline timeline;
	RgsPlayerDecodeWorker worker;
	size_t file_index;
	int selected_file;
	int list_scroll;
	int paused;
	int write_sidecar;
	char status[256];
} RgsPlayer;

enum
{
	RGS_PLAYER_WORKER_STARTING = 0,
	RGS_PLAYER_WORKER_RUNNING = 1,
	RGS_PLAYER_WORKER_FAILED = -1
};

static char* rgs_player_string_copy(const char* text)
{
	size_t length;
	char* copy;
	if (text == NULL)
		return NULL;
	length = strlen(text);
	copy = (char*)malloc(length + 1u);
	if (copy != NULL)
		memcpy(copy, text, length + 1u);
	return copy;
}

static int rgs_player_has_wav_extension(const char* path)
{
	const char* dot = path != NULL ? strrchr(path, '.') : NULL;
	return dot != NULL && SDL_strcasecmp(dot, ".wav") == 0;
}

static const char* rgs_player_basename(const char* path)
{
	const char* slash;
	const char* backslash;
	if (path == NULL)
		return "";
	slash = strrchr(path, '/');
	backslash = strrchr(path, '\\');
	if (backslash != NULL && (slash == NULL || backslash > slash))
		slash = backslash;
	return slash != NULL ? slash + 1 : path;
}

static int rgs_player_file_list_add(RgsPlayerFileList* list, const char* path)
{
	RgsPlayerFile* items;
	const char** labels;
	char* path_copy;
	char* label_copy;
	if (list->count == list->capacity)
	{
		size_t capacity = list->capacity == 0u ? 16u : list->capacity * 2u;
		if (capacity > SIZE_MAX / sizeof(*items) || capacity > SIZE_MAX / sizeof(*labels))
			return 0;
		items = (RgsPlayerFile*)realloc(list->items, capacity * sizeof(*items));
		if (items == NULL)
			return 0;
		list->items = items;
		labels = (const char**)realloc(list->labels, capacity * sizeof(*labels));
		if (labels == NULL)
			return 0;
		list->labels = labels;
		list->capacity = capacity;
	}
	path_copy = rgs_player_string_copy(path);
	label_copy = rgs_player_string_copy(rgs_player_basename(path));
	if (path_copy == NULL || label_copy == NULL)
	{
		free(path_copy);
		free(label_copy);
		return 0;
	}
	list->items[list->count].path = path_copy;
	list->items[list->count].label = label_copy;
	list->labels[list->count] = label_copy;
	list->count += 1u;
	return 1;
}

static void rgs_player_file_list_destroy(RgsPlayerFileList* list)
{
	size_t i;
	if (list == NULL)
		return;
	for (i = 0u; i < list->count; ++i)
	{
		free(list->items[i].path);
		free(list->items[i].label);
	}
	free(list->items);
	free(list->labels);
	memset(list, 0, sizeof(*list));
}

static int rgs_player_file_compare(const void* left, const void* right)
{
	const RgsPlayerFile* a = (const RgsPlayerFile*)left;
	const RgsPlayerFile* b = (const RgsPlayerFile*)right;
	int folded = SDL_strcasecmp(a->path, b->path);
	return folded != 0 ? folded : strcmp(a->path, b->path);
}

static void rgs_player_file_list_sort(RgsPlayerFileList* list)
{
	size_t i;
	qsort(list->items, list->count, sizeof(*list->items), rgs_player_file_compare);
	for (i = 0u; i < list->count; ++i)
		list->labels[i] = list->items[i].label;
}

static SDL_EnumerationResult SDLCALL rgs_player_scan_callback(void* userdata,
                                                              const char* dirname,
                                                              const char* filename)
{
	RgsPlayerFileList* list = (RgsPlayerFileList*)userdata;
	char path[RGS_PLAYER_PATH_CAPACITY];
	SDL_PathInfo info;
	int written;
	if (!rgs_player_has_wav_extension(filename))
		return SDL_ENUM_CONTINUE;
	written = SDL_snprintf(path, sizeof(path), "%s%s", dirname, filename);
	if (written < 0 || (size_t)written >= sizeof(path))
		return SDL_ENUM_FAILURE;
	if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE)
	{
		SDL_ClearError();
		return SDL_ENUM_CONTINUE;
	}
	return rgs_player_file_list_add(list, path) ? SDL_ENUM_CONTINUE : SDL_ENUM_FAILURE;
}

static int rgs_player_collect_files(RgsPlayerFileList* list, const char* input)
{
	SDL_PathInfo info;
	if (!SDL_GetPathInfo(input, &info))
		return 0;
	if (info.type == SDL_PATHTYPE_DIRECTORY)
	{
		if (!SDL_EnumerateDirectory(input, rgs_player_scan_callback, list))
			return 0;
	}
	else if (info.type == SDL_PATHTYPE_FILE && rgs_player_has_wav_extension(input))
	{
		if (!rgs_player_file_list_add(list, input))
			return 0;
	}
	else
	{
		SDL_SetError("Input is not a WAV file or directory: %s", input);
		return 0;
	}
	rgs_player_file_list_sort(list);
	return list->count != 0u;
}

static int rgs_player_make_sidecar_path(const char* wav_path, char* out, size_t capacity)
{
	const char* slash = strrchr(wav_path, '/');
	const char* backslash = strrchr(wav_path, '\\');
	const char* dot = strrchr(wav_path, '.');
	size_t prefix;
	if (backslash != NULL && (slash == NULL || backslash > slash))
		slash = backslash;
	if (dot == NULL || (slash != NULL && dot < slash))
		dot = wav_path + strlen(wav_path);
	prefix = (size_t)(dot - wav_path);
	if (prefix > capacity || capacity - prefix <= 4u)
		return 0;
	memcpy(out, wav_path, prefix);
	memcpy(out + prefix, ".rgs", 5u);
	return 1;
}

static void rgs_player_track_destroy(RgsPlayerTrack* track)
{
	if (track == NULL)
		return;
	free(track->wav_pcm);
	free(track->encoded);
	memset(track, 0, sizeof(*track));
}

static void rgs_player_build_envelope(RgsPlayerTrack* track)
{
	uint32_t bucket;
	for (bucket = 0u; bucket < RGS_PLAYER_ENVELOPE_BUCKETS; ++bucket)
	{
		uint64_t begin = (uint64_t)bucket * track->info.samples /
		                 RGS_PLAYER_ENVELOPE_BUCKETS;
		uint64_t end = (uint64_t)(bucket + 1u) * track->info.samples /
		               RGS_PLAYER_ENVELOPE_BUCKETS;
		uint32_t peak = 0u;
		uint64_t frame;
		if (end <= begin)
			end = begin + 1u;
		if (end > track->info.samples)
			end = track->info.samples;
		for (frame = begin; frame < end; ++frame)
		{
			uint32_t channel;
			for (channel = 0u; channel < track->info.channels; ++channel)
			{
				int sample = track->wav_pcm[(size_t)frame * track->info.channels + channel];
				uint32_t magnitude = (uint32_t)(sample < 0 ? -(int64_t)sample : sample);
				if (magnitude > peak)
					peak = magnitude;
			}
		}
		track->envelope[bucket] = (float)peak / 32768.0f;
	}
}

static int rgs_player_load_track(const char* path, RgsPlayerTrack* out)
{
	SDL_AudioSpec src_spec;
	SDL_AudioSpec s16_spec;
	Uint8* src_data = NULL;
	Uint32 src_size = 0u;
	Uint8* converted = NULL;
	int converted_size = 0;
	int bytes_per_frame;
	uint32_t frames;
	RgsPreparedAudio prepared = {0};
	const char* prepare_error;
	size_t bound;
	RgRgsEncodeOptions options;
	SDL_PathInfo path_info;
	const char* base;
	const char* dot;
	size_t name_length;
	memset(out, 0, sizeof(*out));
	if (!SDL_LoadWAV(path, &src_spec, &src_data, &src_size))
		return 0;
	if (src_size > INT_MAX || src_spec.channels <= 0 ||
	    src_spec.channels > (int)RG_RGS_MAX_CHANNELS || src_spec.freq <= 0)
	{
		SDL_free(src_data);
		SDL_SetError("Unsupported WAV channel count, sample rate, or size: %s", path);
		return 0;
	}
	s16_spec.format = SDL_AUDIO_S16;
	s16_spec.channels = src_spec.channels;
	s16_spec.freq = src_spec.freq;
	if (!SDL_ConvertAudioSamples(&src_spec,
	                             src_data,
	                             (int)src_size,
	                             &s16_spec,
	                             &converted,
	                             &converted_size))
	{
		SDL_free(src_data);
		return 0;
	}
	SDL_free(src_data);
	bytes_per_frame = src_spec.channels * (int)sizeof(int16_t);
	if (converted_size <= 0 || converted_size % bytes_per_frame != 0 ||
	    (uint64_t)(converted_size / bytes_per_frame) > UINT32_MAX)
	{
		SDL_free(converted);
		SDL_SetError("Converted WAV has an invalid sample count: %s", path);
		return 0;
	}
	frames = (uint32_t)(converted_size / bytes_per_frame);
	prepare_error = rgs_audio_prepare_s16((const int16_t*)converted, frames,
	                                     (uint32_t)src_spec.channels,
	                                     (uint32_t)src_spec.freq, &prepared);
	SDL_free(converted);
	if (prepare_error != NULL)
	{
		SDL_SetError("Could not prepare WAV '%s': %s", path, prepare_error);
		return 0;
	}
	/* Use the exact encoder input as the A/B reference, including the actual
	 * flushed resampler timeline. There is no second sample-rate conversion. */
	out->wav_pcm = prepared.pcm;
	bound = rg_rgs_encode_bound(prepared.frames, prepared.channels, prepared.samplerate);
	if (bound == 0u)
	{
		rgs_player_track_destroy(out);
		SDL_SetError("WAV is outside the RGS encoder limits: %s", path);
		return 0;
	}
	out->encoded = (uint8_t*)malloc(bound);
	if (out->encoded == NULL)
	{
		rgs_player_track_destroy(out);
		return 0;
	}
	options = rg_rgs_default_options();
	options.quality = RG_RGS_QUALITY_MEDIUM;
	options.target_kbps = 0u;
	out->encoded_size = rg_rgs_encode_s16_ex(out->wav_pcm,
	                                         prepared.frames,
	                                         prepared.channels,
	                                         prepared.samplerate,
	                                         out->encoded,
	                                         bound,
	                                         &options);
	if (out->encoded_size == 0u ||
	    !rg_rgs_read_header(out->encoded, out->encoded_size, &out->info))
	{
		rgs_player_track_destroy(out);
		SDL_SetError("Could not encode a valid in-memory RGS stream: %s", path);
		return 0;
	}
	if (out->info.samples != prepared.frames || out->info.channels != prepared.channels ||
	    out->info.samplerate != prepared.samplerate)
	{
		rgs_player_track_destroy(out);
		SDL_SetError("Encoded timeline differs from the prepared WAV reference: %s", path);
		return 0;
	}
	if (SDL_GetPathInfo(path, &path_info))
		out->wav_bytes = path_info.size;
	SDL_strlcpy(out->wav_path, path, sizeof(out->wav_path));
	if (!rgs_player_make_sidecar_path(path, out->sidecar_path, sizeof(out->sidecar_path)))
	{
		rgs_player_track_destroy(out);
		SDL_SetError("WAV path is too long for an RGS sidecar: %s", path);
		return 0;
	}
	base = rgs_player_basename(path);
	dot = strrchr(base, '.');
	name_length = dot != NULL ? (size_t)(dot - base) : strlen(base);
	if (name_length >= sizeof(out->name))
		name_length = sizeof(out->name) - 1u;
	memcpy(out->name, base, name_length);
	out->name[name_length] = '\0';
	rgs_player_build_envelope(out);
	return 1;
}

static int rgs_player_save_track(const RgsPlayerTrack* track)
{
	char temporary[RGS_PLAYER_PATH_CAPACITY + 64u];
	FILE* file;
	int written = SDL_snprintf(temporary,
	                           sizeof(temporary),
	                           "%s.tmp.%llu",
	                           track->sidecar_path,
	                           (unsigned long long)SDL_GetTicksNS());
	int success = 0;
	if (written < 0 || (size_t)written >= sizeof(temporary))
	{
		SDL_SetError("Sidecar temporary path is too long");
		return 0;
	}
	file = fopen(temporary, "wb");
	if (file == NULL)
	{
		SDL_SetError("Could not create '%s': %s", temporary, strerror(errno));
		return 0;
	}
	success = fwrite(track->encoded, 1u, track->encoded_size, file) == track->encoded_size;
	if (success)
		success = fflush(file) == 0;
	/* fclose consumes the handle even when it reports a flush error. */
	if (fclose(file) != 0)
		success = 0;
	if (success)
		success = SDL_RenamePath(temporary, track->sidecar_path) ? 1 : 0;
	if (!success)
		SDL_RemovePath(temporary);
	return success;
}

static int SDLCALL rgs_player_decode_thread(void* userdata)
{
	RgsPlayerDecodeWorker* worker = (RgsPlayerDecodeWorker*)userdata;
	RgsPlayer* player = worker->player;
	RgRgsDecoder decoder;
	if (!rg_rgs_decoder_init(&decoder,
	                         player->track.encoded,
	                         player->track.encoded_size,
	                         NULL))
	{
		SDL_SetAtomicInt(&worker->state, RGS_PLAYER_WORKER_FAILED);
		return 1;
	}
	SDL_SetAtomicInt(&worker->state, RGS_PLAYER_WORKER_RUNNING);
	while (!SDL_GetAtomicInt(&worker->stop))
	{
		RgsPlayerStreamSlot* slot = rgs_player_stream_producer_acquire(&player->ring);
		uint32_t frames = 0u;
		RgRgsDecodeStatus status;
		if (slot == NULL)
		{
			SDL_Delay(1u);
			continue;
		}
		status = rg_rgs_decoder_next_s16(&decoder,
		                                 slot->pcm,
		                                 RG_RGS_MAX_FRAME_SAMPLES * player->track.info.channels,
		                                 &frames);
		if (status == RG_RGS_DECODE_FRAME)
		{
			if (!rgs_player_stream_producer_commit(&player->ring, frames))
			{
				SDL_SetAtomicInt(&worker->state, RGS_PLAYER_WORKER_FAILED);
				return 1;
			}
		}
		else if (status == RG_RGS_DECODE_END)
		{
			rg_rgs_decoder_reset(&decoder);
		}
		else
		{
			SDL_SetAtomicInt(&worker->state, RGS_PLAYER_WORKER_FAILED);
			return 1;
		}
	}
	return 0;
}

static void rgs_player_stop_worker(RgsPlayer* player)
{
	if (player->worker.thread == NULL)
		return;
	SDL_SetAtomicInt(&player->worker.stop, 1);
	SDL_WaitThread(player->worker.thread, NULL);
	player->worker.thread = NULL;
}

static int rgs_player_start_worker(RgsPlayer* player)
{
	Uint64 deadline;
	player->worker.player = player;
	SDL_SetAtomicInt(&player->worker.stop, 0);
	SDL_SetAtomicInt(&player->worker.state, RGS_PLAYER_WORKER_STARTING);
	player->worker.thread = SDL_CreateThread(rgs_player_decode_thread, "RGS decoder", &player->worker);
	if (player->worker.thread == NULL)
		return 0;
	deadline = SDL_GetTicksNS() + 2000000000ull;
	while (SDL_GetTicksNS() < deadline)
	{
		if (SDL_GetAtomicInt(&player->worker.state) == RGS_PLAYER_WORKER_FAILED)
			break;
		if (rgs_player_stream_ring_count(&player->ring) >= RGS_PLAYER_PREFILL_SLOTS)
			return 1;
		SDL_Delay(1u);
	}
	rgs_player_stop_worker(player);
	SDL_SetError("RGS decoder could not prefill the playback ring");
	return 0;
}

static void SDLCALL rgs_player_audio_callback(void* userdata,
                                              SDL_AudioStream* stream,
                                              int additional_amount,
                                              int total_amount)
{
	RgsPlayer* player = (RgsPlayer*)userdata;
	int16_t scratch[RGS_PLAYER_AUDIO_CHUNK_FRAMES * RG_RGS_MAX_CHANNELS];
	int bytes_per_frame = (int)player->track.info.channels * (int)sizeof(int16_t);
	(void)total_amount;
	while (bytes_per_frame > 0 && additional_amount >= bytes_per_frame)
	{
		uint32_t frames = (uint32_t)(additional_amount / bytes_per_frame);
		if (frames > RGS_PLAYER_AUDIO_CHUNK_FRAMES)
			frames = RGS_PLAYER_AUDIO_CHUNK_FRAMES;
		(void)rgs_player_stream_pull(&player->ring,
		                             &player->timeline,
		                             player->track.wav_pcm,
		                             scratch,
		                             frames);
		if (!SDL_PutAudioStreamData(stream, scratch, (int)frames * bytes_per_frame))
			return;
		additional_amount -= (int)frames * bytes_per_frame;
	}
}

static int rgs_player_set_paused(RgsPlayer* player, int paused)
{
	int success;
	if (player->audio == NULL)
		return 0;
	success = paused ? SDL_PauseAudioStreamDevice(player->audio) : SDL_ResumeAudioStreamDevice(player->audio);
	if (success)
		player->paused = paused;
	return success;
}

static int rgs_player_set_source(RgsPlayer* player, RgsPlayerStreamSource source)
{
	if (!SDL_LockAudioStream(player->audio))
		return 0;
	player->timeline.source = source;
	/* Deliberately retain queued stream data for a click-free A/B transition. */
	SDL_UnlockAudioStream(player->audio);
	return 1;
}

static int rgs_player_restart(RgsPlayer* player)
{
	int was_paused = player->paused;
	uint64_t underruns;
	if (!was_paused && !rgs_player_set_paused(player, 1))
		return 0;
	rgs_player_stop_worker(player);
	if (!SDL_LockAudioStream(player->audio))
	{
		(void)rgs_player_start_worker(player);
		if (!was_paused)
			(void)rgs_player_set_paused(player, 0);
		return 0;
	}
	underruns = player->timeline.underruns;
	SDL_ClearAudioStream(player->audio);
	rgs_player_stream_ring_reset(&player->ring);
	rgs_player_stream_timeline_restart(&player->timeline);
	player->timeline.underruns = underruns;
	SDL_UnlockAudioStream(player->audio);
	if (!rgs_player_start_worker(player))
		return 0;
	return was_paused || rgs_player_set_paused(player, 0);
}

static int rgs_player_replace_track(RgsPlayer* player,
                                    RgsPlayerTrack* replacement,
                                    size_t file_index)
{
	RgsPlayerTrack old_track;
	RgsPlayerStreamSource source = player->timeline.source;
	int was_paused = player->paused;
	SDL_AudioSpec spec;
	spec.format = SDL_AUDIO_S16;
	spec.channels = (int)replacement->info.channels;
	spec.freq = (int)replacement->info.samplerate;
	if (!was_paused && !rgs_player_set_paused(player, 1))
		return 0;
	rgs_player_stop_worker(player);
	if (!SDL_LockAudioStream(player->audio))
	{
		(void)rgs_player_start_worker(player);
		if (!was_paused)
			(void)rgs_player_set_paused(player, 0);
		return 0;
	}
	SDL_ClearAudioStream(player->audio);
	if (!SDL_SetAudioStreamFormat(player->audio, &spec, NULL))
	{
		uint64_t underruns = player->timeline.underruns;
		rgs_player_stream_ring_reset(&player->ring);
		rgs_player_stream_timeline_restart(&player->timeline);
		player->timeline.underruns = underruns;
		SDL_UnlockAudioStream(player->audio);
		(void)rgs_player_start_worker(player);
		if (!was_paused)
			(void)rgs_player_set_paused(player, 0);
		return 0;
	}
	old_track = player->track;
	player->track = *replacement;
	memset(replacement, 0, sizeof(*replacement));
	rgs_player_stream_ring_init(&player->ring, player->track.info.channels);
	rgs_player_stream_timeline_init(&player->timeline,
	                                player->track.info.samples,
	                                player->track.info.channels);
	player->timeline.source = source;
	SDL_UnlockAudioStream(player->audio);
	rgs_player_track_destroy(&old_track);
	player->file_index = file_index;
	player->selected_file = (int)file_index;
	if (!rgs_player_start_worker(player))
		return 0;
	if (player->write_sidecar && !rgs_player_save_track(&player->track))
		SDL_snprintf(player->status, sizeof(player->status), "Sidecar save failed: %s", SDL_GetError());
	else
		SDL_snprintf(player->status, sizeof(player->status), "Loaded %s", player->track.name);
	return was_paused || rgs_player_set_paused(player, 0);
}

static int rgs_player_load_index(RgsPlayer* player, size_t index)
{
	RgsPlayerTrack replacement;
	if (player->files.count == 0u)
		return 0;
	index %= player->files.count;
	if (!rgs_player_load_track(player->files.items[index].path, &replacement))
		return 0;
	if (!rgs_player_replace_track(player, &replacement, index))
	{
		rgs_player_track_destroy(&replacement);
		return 0;
	}
	return 1;
}

static SDL_SystemCursor rgs_player_sdl_cursor(RgGuiMouseCursor cursor)
{
	switch (cursor)
	{
		case RG_GUI_CURSOR_TEXT: return SDL_SYSTEM_CURSOR_TEXT;
		case RG_GUI_CURSOR_MOVE: return SDL_SYSTEM_CURSOR_MOVE;
		case RG_GUI_CURSOR_RESIZE_H: return SDL_SYSTEM_CURSOR_EW_RESIZE;
		case RG_GUI_CURSOR_RESIZE_V: return SDL_SYSTEM_CURSOR_NS_RESIZE;
		default: return SDL_SYSTEM_CURSOR_DEFAULT;
	}
}

static void rgs_player_cursor_init(RgsPlayerCursorState* state)
{
	u32 i;
	memset(state, 0, sizeof(*state));
	state->cursors[RG_GUI_CURSOR_DEFAULT] = SDL_GetDefaultCursor();
	for (i = (u32)RG_GUI_CURSOR_TEXT; i < (u32)RG_ARRAY_COUNT(state->cursors); ++i)
	{
		state->cursors[i] = SDL_CreateSystemCursor(rgs_player_sdl_cursor((RgGuiMouseCursor)i));
		if (state->cursors[i] == NULL)
			SDL_ClearError();
	}
}

static int rgs_player_cursor_apply(RgsPlayerCursorState* state,
                                   const RgGuiPlatformOutput* output)
{
	RgGuiMouseCursor cursor = output != NULL ? output->cursor : RG_GUI_CURSOR_DEFAULT;
	SDL_Cursor* sdl_cursor;
	if ((u32)cursor >= (u32)RG_ARRAY_COUNT(state->cursors))
		cursor = RG_GUI_CURSOR_DEFAULT;
	if (state->has_applied && cursor == state->applied)
		return 1;
	sdl_cursor = state->cursors[cursor];
	if (sdl_cursor == NULL)
		sdl_cursor = state->cursors[RG_GUI_CURSOR_DEFAULT];
	if (sdl_cursor != NULL && !SDL_SetCursor(sdl_cursor))
		return 0;
	state->applied = cursor;
	state->has_applied = 1;
	return 1;
}

static void rgs_player_cursor_destroy(RgsPlayerCursorState* state)
{
	u32 i;
	if (state->cursors[RG_GUI_CURSOR_DEFAULT] != NULL)
		SDL_SetCursor(state->cursors[RG_GUI_CURSOR_DEFAULT]);
	for (i = (u32)RG_GUI_CURSOR_TEXT; i < (u32)RG_ARRAY_COUNT(state->cursors); ++i)
		if (state->cursors[i] != NULL)
			SDL_DestroyCursor(state->cursors[i]);
	memset(state, 0, sizeof(*state));
}

static int rgs_player_read_file(const char* path, void** out_data, size_t* out_size)
{
	FILE* file = fopen(path, "rb");
	long length;
	void* data;
	size_t read;
	if (file == NULL)
		return 0;
	if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
	    fseek(file, 0, SEEK_SET) != 0)
	{
		fclose(file);
		return 0;
	}
	data = malloc((size_t)length);
	if (data == NULL)
	{
		fclose(file);
		return 0;
	}
	read = fread(data, 1u, (size_t)length, file);
	fclose(file);
	if (read != (size_t)length)
	{
		free(data);
		return 0;
	}
	*out_data = data;
	*out_size = read;
	return 1;
}

static int rgs_player_base64_value(unsigned char character)
{
	if (character >= 'A' && character <= 'Z')
		return (int)(character - 'A');
	if (character >= 'a' && character <= 'z')
		return (int)(character - 'a') + 26;
	if (character >= '0' && character <= '9')
		return (int)(character - '0') + 52;
	if (character == '+')
		return 62;
	if (character == '/')
		return 63;
	return -1;
}

static int rgs_player_is_base64_space(unsigned char character)
{
	return character == ' ' || character == '\t' || character == '\r' ||
	       character == '\n' || character == '\f' || character == '\v';
}

static int rgs_player_decode_base64(const char* encoded,
                                    size_t encoded_size,
                                    void** out_data,
                                    size_t* out_size)
{
	size_t character_count = 0u;
	size_t capacity;
	uint8_t* decoded;
	size_t written = 0u;
	size_t cursor = 0u;
	size_t i;
	for (i = 0u; i < encoded_size; ++i)
		if (!rgs_player_is_base64_space((unsigned char)encoded[i]))
			character_count += 1u;
	if (character_count == 0u || (character_count & 3u) != 0u ||
	    character_count / 4u > SIZE_MAX / 3u)
		return 0;
	capacity = character_count / 4u * 3u;
	decoded = (uint8_t*)malloc(capacity);
	if (decoded == NULL)
		return 0;
	while (cursor < encoded_size)
	{
		int value[4];
		int index;
		int padding = 0;
		for (index = 0; index < 4;)
		{
			unsigned char character;
			while (cursor < encoded_size &&
			       rgs_player_is_base64_space((unsigned char)encoded[cursor]))
				cursor += 1u;
			if (cursor == encoded_size)
			{
				free(decoded);
				return 0;
			}
			character = (unsigned char)encoded[cursor++];
			if (character == '=')
			{
				if (index < 2)
				{
					free(decoded);
					return 0;
				}
				value[index] = 0;
				padding += 1;
			}
			else
			{
				value[index] = rgs_player_base64_value(character);
				if (value[index] < 0 || padding != 0)
				{
					free(decoded);
					return 0;
				}
			}
			index += 1;
		}
		if (padding > 2)
		{
			free(decoded);
			return 0;
		}
		decoded[written++] = (uint8_t)((value[0] << 2) | (value[1] >> 4));
		if (padding < 2)
			decoded[written++] = (uint8_t)((value[1] << 4) | (value[2] >> 2));
		if (padding == 0)
			decoded[written++] = (uint8_t)((value[2] << 6) | value[3]);
		if (padding != 0)
		{
			while (cursor < encoded_size &&
			       rgs_player_is_base64_space((unsigned char)encoded[cursor]))
				cursor += 1u;
			if (cursor != encoded_size)
			{
				free(decoded);
				return 0;
			}
		}
	}
	*out_data = decoded;
	*out_size = written;
	return 1;
}

static int rgs_player_font_try_load(RgsPlayerFontAssets* assets, const char* base)
{
	char metrics_path[RGS_PLAYER_PATH_CAPACITY];
	char atlas_path[RGS_PLAYER_PATH_CAPACITY];
	char encoded_atlas_path[RGS_PLAYER_PATH_CAPACITY];
	void* metrics = NULL;
	size_t metrics_size = 0u;
	void* pixels = NULL;
	size_t pixel_size = 0u;
	RgTextFontLoadDesc desc;
	u64 expected_size;
	int metrics_written = SDL_snprintf(metrics_path, sizeof(metrics_path), "%s.font", base);
	int atlas_written = SDL_snprintf(atlas_path, sizeof(atlas_path), "%s.rgba", base);
	int encoded_atlas_written = SDL_snprintf(encoded_atlas_path,
	                                         sizeof(encoded_atlas_path),
	                                         "%s.rgba.b64",
	                                         base);
	if (metrics_written < 0 || (size_t)metrics_written >= sizeof(metrics_path) ||
	    atlas_written < 0 || (size_t)atlas_written >= sizeof(atlas_path) ||
	    encoded_atlas_written < 0 ||
	    (size_t)encoded_atlas_written >= sizeof(encoded_atlas_path) ||
	    !rgs_player_read_file(metrics_path, &metrics, &metrics_size))
		return 0;
	memset(&desc, 0, sizeof(desc));
	desc.data = metrics;
	desc.data_size = metrics_size;
	desc.glyphs = assets->glyphs;
	desc.glyph_capacity = RGS_PLAYER_FONT_GLYPH_CAPACITY;
	desc.kernings = assets->kernings;
	desc.kerning_capacity = RGS_PLAYER_FONT_KERNING_CAPACITY;
	if (!rg_text_font_load_rgfont(&assets->font, &desc))
	{
		free(metrics);
		return 0;
	}
	free(metrics);
	if (!rgs_player_read_file(atlas_path, &pixels, &pixel_size))
	{
		void* encoded_pixels = NULL;
		size_t encoded_pixel_size = 0u;
		if (!rgs_player_read_file(encoded_atlas_path, &encoded_pixels, &encoded_pixel_size) ||
		    !rgs_player_decode_base64((const char*)encoded_pixels,
		                              encoded_pixel_size,
		                              &pixels,
		                              &pixel_size))
		{
			free(encoded_pixels);
			return 0;
		}
		free(encoded_pixels);
	}
	expected_size = (u64)assets->font.metrics.atlas_width *
	                (u64)assets->font.metrics.atlas_height * 4u;
	if (expected_size == 0u || expected_size != (u64)pixel_size)
	{
		free(pixels);
		return 0;
	}
	assets->pixels = (u8*)pixels;
	assets->atlas_width = assets->font.metrics.atlas_width;
	assets->atlas_height = assets->font.metrics.atlas_height;
	return 1;
}

static int rgs_player_font_load(RgsPlayerFontAssets* assets)
{
	char base[RGS_PLAYER_PATH_CAPACITY];
	const char* executable_dir;
	memset(assets, 0, sizeof(*assets));
	executable_dir = SDL_GetBasePath();
	if (executable_dir != NULL)
	{
		if ((SDL_snprintf(base,
		                  sizeof(base),
		                  "%stools/assets/rgs_player/%s",
		                  executable_dir,
		                  RGS_PLAYER_FONT_STEM) >= 0 &&
		     rgs_player_font_try_load(assets, base)) ||
		    (SDL_snprintf(base,
		                  sizeof(base),
		                  "%sassets/rgs_player/%s",
		                  executable_dir,
		                  RGS_PLAYER_FONT_STEM) >= 0 &&
		     rgs_player_font_try_load(assets, base)))
		{
			SDL_ClearError();
			return 1;
		}
	}
	if (rgs_player_font_try_load(assets,
	                             "tools/assets/rgs_player/" RGS_PLAYER_FONT_STEM) ||
	    rgs_player_font_try_load(assets, "assets/rgs_player/" RGS_PLAYER_FONT_STEM))
	{
		SDL_ClearError();
		return 1;
	}
	SDL_SetError("Could not load the Inter player font metrics and RGBA atlas (.rgba or .rgba.b64)");
	return 0;
}

static void rgs_player_font_destroy(RgsPlayerFontAssets* assets)
{
	free(assets->pixels);
	memset(assets, 0, sizeof(*assets));
}

static int rgs_player_shader_root(char* out, size_t capacity)
{
	char compiled[RGS_PLAYER_PATH_CAPACITY];
	SDL_PathInfo info;
	const char* executable_dir = SDL_GetBasePath();
	if (executable_dir != NULL &&
	    SDL_snprintf(out, capacity, "%sshaders", executable_dir) >= 0 &&
	    SDL_snprintf(compiled, sizeof(compiled), "%s/Compiled", out) >= 0 &&
	    SDL_GetPathInfo(compiled, &info) && info.type == SDL_PATHTYPE_DIRECTORY)
	{
		SDL_ClearError();
		return 1;
	}
	if (SDL_GetPathInfo("shaders/Compiled", &info) && info.type == SDL_PATHTYPE_DIRECTORY &&
	    SDL_snprintf(out, capacity, "shaders") >= 0)
	{
		SDL_ClearError();
		return 1;
	}
	SDL_SetError("Could not find shaders/Compiled beside the executable or current directory");
	return 0;
}

static SDL_GPUTexture* rgs_player_atlas_create(SDL_GPUDevice* device,
                                               const RgsPlayerFontAssets* assets)
{
	u64 size64 = (u64)assets->atlas_width * assets->atlas_height * 4u;
	u32 size;
	SDL_GPUTextureCreateInfo texture_info;
	SDL_GPUTransferBufferCreateInfo transfer_info;
	SDL_GPUTexture* texture;
	SDL_GPUTransferBuffer* transfer;
	void* mapped;
	SDL_GPUCommandBuffer* command_buffer;
	SDL_GPUCopyPass* copy;
	SDL_GPUTextureTransferInfo source;
	SDL_GPUTextureRegion destination;
	if (device == NULL || assets->pixels == NULL || size64 == 0u || size64 > UINT32_MAX)
		return NULL;
	size = (u32)size64;
	memset(&texture_info, 0, sizeof(texture_info));
	texture_info.type = SDL_GPU_TEXTURETYPE_2D;
	texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
	texture_info.width = assets->atlas_width;
	texture_info.height = assets->atlas_height;
	texture_info.layer_count_or_depth = 1u;
	texture_info.num_levels = 1u;
	texture = SDL_CreateGPUTexture(device, &texture_info);
	if (texture == NULL)
		return NULL;
	memset(&transfer_info, 0, sizeof(transfer_info));
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size = size;
	transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);
	if (transfer == NULL)
	{
		SDL_ReleaseGPUTexture(device, texture);
		return NULL;
	}
	mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
	if (mapped == NULL)
	{
		SDL_ReleaseGPUTransferBuffer(device, transfer);
		SDL_ReleaseGPUTexture(device, texture);
		return NULL;
	}
	memcpy(mapped, assets->pixels, size);
	SDL_UnmapGPUTransferBuffer(device, transfer);
	command_buffer = SDL_AcquireGPUCommandBuffer(device);
	if (command_buffer == NULL)
	{
		SDL_ReleaseGPUTransferBuffer(device, transfer);
		SDL_ReleaseGPUTexture(device, texture);
		return NULL;
	}
	copy = SDL_BeginGPUCopyPass(command_buffer);
	if (copy == NULL)
	{
		SDL_CancelGPUCommandBuffer(command_buffer);
		SDL_ReleaseGPUTransferBuffer(device, transfer);
		SDL_ReleaseGPUTexture(device, texture);
		return NULL;
	}
	memset(&source, 0, sizeof(source));
	source.transfer_buffer = transfer;
	source.pixels_per_row = assets->atlas_width;
	source.rows_per_layer = assets->atlas_height;
	memset(&destination, 0, sizeof(destination));
	destination.texture = texture;
	destination.w = assets->atlas_width;
	destination.h = assets->atlas_height;
	destination.d = 1u;
	SDL_UploadToGPUTexture(copy, &source, &destination, false);
	SDL_EndGPUCopyPass(copy);
	if (!SDL_SubmitGPUCommandBuffer(command_buffer))
	{
		SDL_ReleaseGPUTransferBuffer(device, transfer);
		SDL_ReleaseGPUTexture(device, texture);
		return NULL;
	}
	SDL_ReleaseGPUTransferBuffer(device, transfer);
	return texture;
}

static int rgs_player_render_frame(SDL_GPUDevice* device,
                                   SDL_Window* window,
                                   RgGuiGpuRenderer* gpu,
                                   RgGuiRenderer* text_renderer,
                                   RgGpuUploadRing* upload_ring,
                                   RgGuiContext* gui)
{
	const RgGuiDrawList* draw_list = rg_gui_draw_list(gui);
	u32 overlay_start = rg_gui_draw_list_overlay_start(gui);
	RgGuiGpuUpload upload = {0};
	SDL_GPUCommandBuffer* command_buffer;
	SDL_GPUTexture* swapchain = NULL;
	u32 width = 0u;
	u32 height = 0u;
	int result = 0;
	rg_gui_renderer_begin_frame(text_renderer);
	if (!rg_gui_gpu_prepare(gpu, text_renderer, draw_list, overlay_start))
		goto done;
	rg_gpu_upload_ring_begin(upload_ring, 1);
	if (!rg_gui_gpu_stage_upload(gpu, text_renderer, upload_ring, &upload))
	{
		rg_gpu_upload_ring_end(upload_ring);
		goto done;
	}
	rg_gpu_upload_ring_end(upload_ring);
	command_buffer = SDL_AcquireGPUCommandBuffer(device);
	if (command_buffer == NULL)
		goto done;
	if (upload.has_text || upload.has_geometry)
	{
		SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command_buffer);
		if (copy == NULL)
		{
			SDL_CancelGPUCommandBuffer(command_buffer);
			goto done;
		}
		rg_gui_gpu_encode_upload(gpu, copy, upload_ring, &upload);
		SDL_EndGPUCopyPass(copy);
	}
	if (!rg_gui_gpu_dispatch_upload(gpu, command_buffer, &upload) ||
	    !rg_gui_gpu_upload_ready(gpu, &upload))
	{
		SDL_CancelGPUCommandBuffer(command_buffer);
		SDL_SetError("GUI upload packet is not ready for submission");
		goto done;
	}
	/* Validate before acquiring a swapchain texture: SDL forbids cancelling
	 * its command buffer after acquisition. No packet state changes intervene. */
	if (!SDL_WaitAndAcquireGPUSwapchainTexture(command_buffer,
	                                           window,
	                                           &swapchain,
	                                           &width,
	                                           &height))
	{
		SDL_CancelGPUCommandBuffer(command_buffer);
		goto done;
	}
	if (swapchain != NULL)
	{
		SDL_GPUColorTargetInfo target;
		SDL_GPURenderPass* pass;
		RgGuiGpuDrawDesc draw_desc;
		memset(&target, 0, sizeof(target));
		target.texture = swapchain;
		target.clear_color = (SDL_FColor){0.025f, 0.032f, 0.046f, 1.0f};
		target.load_op = SDL_GPU_LOADOP_CLEAR;
		target.store_op = SDL_GPU_STOREOP_STORE;
		pass = SDL_BeginGPURenderPass(command_buffer, &target, 1u, NULL);
		if (pass == NULL)
		{
			char render_error[512];
			SDL_strlcpy(render_error, SDL_GetError(), sizeof(render_error));
			if (SDL_SubmitGPUCommandBuffer(command_buffer))
				rg_gui_gpu_upload_commit(gpu, &upload);
			SDL_SetError("Could not begin player render pass: %s", render_error);
			goto done;
		}
		memset(&draw_desc, 0, sizeof(draw_desc));
		draw_desc.output_width = width;
		draw_desc.output_height = height;
		draw_desc.viewport = (SDL_Rect){0, 0, (int)width, (int)height};
		rg_gui_gpu_draw(gpu, command_buffer, pass, &draw_desc, &upload);
		SDL_EndGPURenderPass(pass);
	}
	if (SDL_SubmitGPUCommandBuffer(command_buffer))
	{
		rg_gui_gpu_upload_commit(gpu, &upload);
		result = 1;
	}
done:
	/* Commit releases successful packets; abort is harmless for those and
	 * releases every packet retained by an error or cancellation path. */
	rg_gui_gpu_upload_abort(gpu, &upload);
	return result;
}

typedef struct RgsPlayerUiActions
{
	int select_file;
	int toggle_pause;
	int restart;
	int previous;
	int next;
	int save;
	int source;
} RgsPlayerUiActions;

static void rgs_player_build_ui(RgGuiContext* gui,
                                RgsPlayer* player,
                                int width,
                                int height,
                                uint64_t cursor,
                                uint64_t underruns,
                                RgsPlayerStreamSource source,
                                RgsPlayerUiActions* actions)
{
	RgGuiRect bounds = rg_gui_make_rect(0.0f, 0.0f, (f32)width, (f32)height);
	RgGuiRect playlist;
	RgGuiRect content;
	RgGuiRect waveform;
	char line[384];
	double seconds = (double)cursor / player->track.info.samplerate;
	double duration = (double)player->track.info.samples / player->track.info.samplerate;
	double bitrate = player->track.info.samples != 0u ? (double)player->track.encoded_size * 8.0 *
	                                                        player->track.info.samplerate /
	                                                        ((double)player->track.info.samples * 1000.0)
	                                                  : 0.0;
	float progress = player->track.info.samples != 0u ? (float)((double)cursor / player->track.info.samples) : 0.0f;
	int source_value = (int)source;
	int selected = player->selected_file;
	int worker_state = SDL_GetAtomicInt(&player->worker.state);

	rg_gui_window_set_bounds(gui, bounds);
	rg_gui_push_rect(gui, rg_gui_make_rect(0.0f, 0.0f, (f32)width, 58.0f),
	                 rg_gui_color(0.046f, 0.064f, 0.096f, 1.0f));
	rg_gui_push_rect(gui, rg_gui_make_rect(0.0f, 56.0f, (f32)width, 2.0f),
	                 gui->style.color_accent);
	rg_gui_label_static(gui, "rg_audio / RGS A-B Player",
	                    rg_gui_make_rect(18.0f, 10.0f, 420.0f, 36.0f));
	rg_gui_label_static(gui, "Space play/pause  Tab A/B  1 WAV  2/B RGS  R restart  arrows tracks",
	                    rg_gui_make_rect((f32)width - 650.0f, 12.0f, 632.0f, 32.0f));

	playlist = rg_gui_make_rect(16.0f, 76.0f, 270.0f, (f32)height - 92.0f);
	content = rg_gui_make_rect(304.0f, 76.0f, (f32)width - 320.0f, (f32)height - 92.0f);
	rg_gui_label_static(gui, "WAV playlist", rg_gui_make_rect(playlist.x, playlist.y, playlist.w, 24.0f));
	if (rg_gui_list(gui,
	                player->files.labels,
	                (u32)player->files.count,
	                &selected,
	                &player->list_scroll,
	                rg_gui_make_rect(playlist.x, playlist.y + 30.0f, playlist.w, playlist.h - 30.0f),
	                rg_gui_id_str("playlist")))
	{
		actions->select_file = selected;
	}

	SDL_snprintf(line,
	             sizeof(line),
	             "%zu / %zu    %s",
	             player->file_index + 1u,
	             player->files.count,
	             player->track.name);
	rg_gui_label(gui, line, rg_gui_make_rect(content.x, content.y, content.w, 30.0f));
	SDL_snprintf(line,
	             sizeof(line),
	             "%u Hz  |  %u channel%s  |  %u frames  |  %.2f seconds",
	             player->track.info.samplerate,
	             player->track.info.channels,
	             player->track.info.channels == 1u ? "" : "s",
	             player->track.info.samples,
	             duration);
	rg_gui_label(gui, line, rg_gui_make_rect(content.x, content.y + 32.0f, content.w, 26.0f));
	SDL_snprintf(line,
	             sizeof(line),
	             "RGS v1: %.2f KiB  |  %.1f kbps  |  %.1f%% of source WAV",
	             (double)player->track.encoded_size / 1024.0,
	             bitrate,
	             player->track.wav_bytes != 0u ? (double)player->track.encoded_size * 100.0 / player->track.wav_bytes : 0.0);
	rg_gui_label(gui, line, rg_gui_make_rect(content.x, content.y + 60.0f, content.w, 26.0f));

	waveform = rg_gui_make_rect(content.x, content.y + 98.0f, content.w, 185.0f);
	rg_gui_plot_lines(gui,
	                  "Normalized WAV peak envelope",
	                  player->track.envelope,
	                  RGS_PLAYER_ENVELOPE_BUCKETS,
	                  0.0f,
	                  1.0f,
	                  waveform);
	SDL_snprintf(line, sizeof(line), "%.2f / %.2f seconds", seconds, duration);
	rg_gui_progress_bar(gui,
	                    line,
	                    progress,
	                    0.0f,
	                    1.0f,
	                    rg_gui_make_rect(content.x, content.y + 296.0f, content.w, 26.0f));

	if (rg_gui_radio_static(gui,
	                        "A  WAV reference",
	                        &source_value,
	                        RGS_PLAYER_STREAM_SOURCE_WAV,
	                        rg_gui_make_rect(content.x, content.y + 338.0f, 190.0f, 28.0f),
	                        rg_gui_id_str("source_wav")))
		actions->source = source_value;
	if (rg_gui_radio_static(gui,
	                        "B  streaming RGS",
	                        &source_value,
	                        RGS_PLAYER_STREAM_SOURCE_RGS,
	                        rg_gui_make_rect(content.x + 200.0f,
	                                         content.y + 338.0f,
	                                         190.0f,
	                                         28.0f),
	                        rg_gui_id_str("source_rgs")))
		actions->source = source_value;

	if (rg_gui_button_static(gui,
	                         "Previous",
	                         rg_gui_make_rect(content.x, content.y + 382.0f, 100.0f, 32.0f),
	                         rg_gui_id_str("previous")))
		actions->previous = 1;
	if (rg_gui_button(gui,
	                  player->paused ? "Play" : "Pause",
	                  rg_gui_make_rect(content.x + 110.0f,
	                                   content.y + 382.0f,
	                                   90.0f,
	                                   32.0f),
	                  rg_gui_id_str("pause")))
		actions->toggle_pause = 1;
	if (rg_gui_button_static(gui,
	                         "Restart",
	                         rg_gui_make_rect(content.x + 210.0f,
	                                          content.y + 382.0f,
	                                          90.0f,
	                                          32.0f),
	                         rg_gui_id_str("restart")))
		actions->restart = 1;
	if (rg_gui_button_static(gui,
	                         "Next",
	                         rg_gui_make_rect(content.x + 310.0f,
	                                          content.y + 382.0f,
	                                          90.0f,
	                                          32.0f),
	                         rg_gui_id_str("next")))
		actions->next = 1;
	rg_gui_begin_disabled(gui, player->track.sidecar_path[0] == '\0');
	if (rg_gui_button_static(gui,
	                         "Save RGS",
	                         rg_gui_make_rect(content.x + 420.0f,
	                                          content.y + 382.0f,
	                                          110.0f,
	                                          32.0f),
	                         rg_gui_id_str("save")))
		actions->save = 1;
	rg_gui_end_disabled(gui);

	SDL_snprintf(line,
	             sizeof(line),
	             "Decoder: %s  |  buffered frames: %u / %u  |  underruns: %llu",
	             worker_state == RGS_PLAYER_WORKER_RUNNING ? "running" : (worker_state == RGS_PLAYER_WORKER_FAILED ? "FAILED" : "starting"),
	             rgs_player_stream_ring_count(&player->ring),
	             RGS_PLAYER_STREAM_SLOT_COUNT,
	             (unsigned long long)underruns);
	rg_gui_label(gui, line, rg_gui_make_rect(content.x, content.y + 430.0f, content.w, 26.0f));
	rg_gui_label(gui,
	             player->status,
	             rg_gui_make_rect(content.x, content.y + 460.0f, content.w, 28.0f));
}

static int rgs_player_make_smoke_track(RgsPlayerTrack* track)
{
	const uint32_t frames = 44100u;
	const uint32_t channels = 2u;
	uint64_t values = (uint64_t)frames * channels;
	uint32_t frame;
	size_t bound;
	RgRgsEncodeOptions options;
	memset(track, 0, sizeof(*track));
	track->wav_pcm = (int16_t*)malloc((size_t)values * sizeof(int16_t));
	if (track->wav_pcm == NULL)
		return 0;
	for (frame = 0u; frame < frames; ++frame)
	{
		int16_t sample = ((frame / 64u) & 1u) != 0u ? 9000 : -9000;
		track->wav_pcm[(size_t)frame * channels] = sample;
		track->wav_pcm[(size_t)frame * channels + 1u] = (int16_t)-sample;
	}
	bound = rg_rgs_encode_bound(frames, channels, 44100u);
	track->encoded = (uint8_t*)malloc(bound);
	if (track->encoded == NULL)
	{
		rgs_player_track_destroy(track);
		return 0;
	}
	options = rg_rgs_default_options();
	track->encoded_size = rg_rgs_encode_s16_ex(track->wav_pcm,
	                                           frames,
	                                           channels,
	                                           44100u,
	                                           track->encoded,
	                                           bound,
	                                           &options);
	if (track->encoded_size == 0u ||
	    !rg_rgs_read_header(track->encoded, track->encoded_size, &track->info))
	{
		rgs_player_track_destroy(track);
		return 0;
	}
	track->wav_bytes = values * sizeof(int16_t) + 44u;
	SDL_strlcpy(track->name, "generated smoke tone", sizeof(track->name));
	rgs_player_build_envelope(track);
	return 1;
}

static void rgs_player_print_usage(void)
{
	printf("Usage: rgs_player [wav-file|directory] [--write-sidecar]\n");
	printf("       rgs_player --smoke-test\n");
}

int main(int argc, char** argv)
{
	const char* input = ".";
	int has_input = 0;
	int smoke_test = 0;
	int frame_limit = 0;
	int hidden = 0;
	int exit_code = 1;
	int sdl_initialized = 0;
	int window_claimed = 0;
	int gui_initialized = 0;
	int text_initialized = 0;
	int gpu_initialized = 0;
	int upload_initialized = 0;
	int i;
	RgsPlayer* player = NULL;
	SDL_Window* window = NULL;
	SDL_GPUDevice* device = NULL;
	SDL_GPUTexture* atlas = NULL;
	RgGuiGpuRenderer gpu;
	RgGpuUploadRing upload_ring;
	RgsPlayerFontAssets font_assets;
	RgsPlayerCursorState cursor_state;
	RgGuiContext gui;
	RgGuiRenderer text_renderer;
	void* gui_memory = NULL;
	void* text_memory = NULL;
	RgInputState input_state;
	RgInputEvent input_event_storage[256];
	char input_event_text[KB(8)];
	RgInputEventQueue input_events;
	SDL_WindowID window_id = 0u;
	u64 previous_ticks = 0u;
	int running = 1;
	int submitted_frames = 0;

	memset(&gpu, 0, sizeof(gpu));
	memset(&upload_ring, 0, sizeof(upload_ring));
	memset(&font_assets, 0, sizeof(font_assets));
	memset(&cursor_state, 0, sizeof(cursor_state));
	memset(&gui, 0, sizeof(gui));
	memset(&text_renderer, 0, sizeof(text_renderer));
	for (i = 1; i < argc; ++i)
	{
		if (strcmp(argv[i], "--write-sidecar") == 0)
			continue;
		if (strcmp(argv[i], "--smoke-test") == 0)
		{
			smoke_test = 1;
			frame_limit = 3;
		}
		else if (strcmp(argv[i], "--hidden") == 0)
			hidden = 1;
		else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
		{
			frame_limit = atoi(argv[++i]);
			if (frame_limit < 1)
				frame_limit = 1;
		}
		else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
		{
			rgs_player_print_usage();
			return 0;
		}
		else if (argv[i][0] == '-')
		{
			fprintf(stderr, "Unknown option: %s\n", argv[i]);
			rgs_player_print_usage();
			return 1;
		}
		else if (!has_input)
		{
			input = argv[i];
			has_input = 1;
		}
		else
		{
			fprintf(stderr, "Only one WAV file or directory may be supplied.\n");
			return 1;
		}
	}

	player = (RgsPlayer*)calloc(1u, sizeof(*player));
	if (player == NULL)
		goto cleanup;
	for (i = 1; i < argc; ++i)
		if (strcmp(argv[i], "--write-sidecar") == 0)
			player->write_sidecar = 1;
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO))
		goto cleanup;
	sdl_initialized = 1;

	if (smoke_test && !has_input)
	{
		if (!rgs_player_file_list_add(&player->files, "generated smoke tone.wav") ||
		    !rgs_player_make_smoke_track(&player->track))
			goto cleanup;
	}
	else
	{
		if (!rgs_player_collect_files(&player->files, input))
			goto cleanup;
		if (!rgs_player_load_track(player->files.items[0].path, &player->track))
			goto cleanup;
	}
	player->selected_file = 0;
	rgs_player_stream_ring_init(&player->ring, player->track.info.channels);
	rgs_player_stream_timeline_init(&player->timeline,
	                                player->track.info.samples,
	                                player->track.info.channels);
	if (!rgs_player_start_worker(player))
		goto cleanup;
	if (player->write_sidecar && player->track.sidecar_path[0] != '\0' &&
	    !rgs_player_save_track(&player->track))
		goto cleanup;
	SDL_snprintf(player->status,
	             sizeof(player->status),
	             "Encoded %s in memory%s",
	             player->track.name,
	             player->write_sidecar ? " and saved its sidecar" : "");

	{
		SDL_AudioSpec audio_spec;
		audio_spec.format = SDL_AUDIO_S16;
		audio_spec.channels = (int)player->track.info.channels;
		audio_spec.freq = (int)player->track.info.samplerate;
		player->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
		                                          &audio_spec,
		                                          rgs_player_audio_callback,
		                                          player);
		if (player->audio == NULL)
			goto cleanup;
		player->paused = 1;
	}

	window = SDL_CreateWindow("RGS A/B Player",
	                          1120,
	                          720,
	                          SDL_WINDOW_RESIZABLE | (hidden ? SDL_WINDOW_HIDDEN : 0));
	if (window == NULL)
		goto cleanup;
	if (!SDL_SetWindowMinimumSize(window, 850, 620))
		goto cleanup;
	{
		RgGpuDeviceDesc desc;
		memset(&desc, 0, sizeof(desc));
		desc.shader_formats = RG_GPU_DEFAULT_SHADER_FORMATS;
		desc.enable_debug = RGS_PLAYER_GPU_DEBUG;
		device = rg_gpu_device_create(&desc);
	}
	if (device == NULL || !rg_gpu_claim_window(device, window))
		goto cleanup;
	window_claimed = 1;
	if (!rgs_player_font_load(&font_assets))
		goto cleanup;
	atlas = rgs_player_atlas_create(device, &font_assets);
	if (atlas == NULL)
		goto cleanup;

	{
		RgGuiInitDesc desc;
		size_t memory_size;
		RgArena arena;
		memset(&desc, 0, sizeof(desc));
		desc.font = &font_assets.font;
		desc.max_draw_cmds = 4096u;
		desc.text_buffer_size = KB(64);
		memory_size = rg_gui_memory_required(&desc);
		gui_memory = malloc(memory_size);
		if (memory_size == 0u || gui_memory == NULL)
			goto cleanup;
		arena = (RgArena){(char*)gui_memory, memory_size, 0u, memory_size};
		if (!rg_gui_init(&gui, &arena, &desc))
			goto cleanup;
		gui_initialized = 1;
	}
	{
		RgGuiRendererLimits limits = rg_gui_renderer_limits_default();
		RgGuiRendererInitDesc desc;
		size_t memory_size = rg_gui_renderer_memory_required(&limits, 0u);
		RgArena arena;
		char shader_root[RGS_PLAYER_PATH_CAPACITY];
		RgGuiGpuDesc gpu_desc;
		if (memory_size == SIZE_MAX)
			goto cleanup;
		text_memory = malloc(memory_size);
		if (text_memory == NULL)
			goto cleanup;
		arena = (RgArena){(char*)text_memory, memory_size, 0u, memory_size};
		memset(&desc, 0, sizeof(desc));
		desc.font = &font_assets.font;
		desc.limits = limits;
		if (!rg_gui_renderer_init(&text_renderer, &arena, &desc))
			goto cleanup;
		text_initialized = 1;
		if (!rgs_player_shader_root(shader_root, sizeof(shader_root)))
			goto cleanup;
		memset(&gpu_desc, 0, sizeof(gpu_desc));
		gpu_desc.device = device;
		gpu_desc.target_format = rg_gpu_swapchain_format(device, window);
		gpu_desc.shader_root = shader_root;
		gpu_desc.atlas_texture = atlas;
		gpu_desc.atlas_width = font_assets.atlas_width;
		gpu_desc.atlas_height = font_assets.atlas_height;
		gpu_desc.max_cached_quads = limits.max_cached_quads;
		gpu_desc.max_runs = limits.max_frame_instances;
		gpu_desc.max_text_instances = limits.max_frame_instances;
		gpu_desc.max_geometry_vertices = 16384u;
		gpu_desc.max_items = 2048u;
		gpu_desc.min_filter = SDL_GPU_FILTER_LINEAR;
		gpu_desc.mag_filter = SDL_GPU_FILTER_LINEAR;
		if (!rg_gui_gpu_create(&gpu, &gpu_desc))
			goto cleanup;
		gpu_initialized = 1;
	}
	{
		const u32 upload_bytes = rg_gui_gpu_upload_ring_size_required(&gpu);
		if (upload_bytes == 0u || !rg_gpu_upload_ring_init(&upload_ring, device, upload_bytes))
			goto cleanup;
	}
	upload_initialized = 1;
	rgs_player_cursor_init(&cursor_state);
	rg_input_init(&input_state);
	rg_input_event_queue_init(&input_events,
	                          input_event_storage,
	                          RG_ARRAY_COUNT(input_event_storage),
	                          input_event_text,
	                          sizeof(input_event_text));
	window_id = SDL_GetWindowID(window);
	previous_ticks = SDL_GetTicksNS();
	if (!rgs_player_set_paused(player, 0))
		goto cleanup;

	while (running)
	{
		SDL_Event event;
		u64 now;
		f32 delta_time;
		int width = 0;
		int height = 0;
		uint64_t cursor = 0u;
		uint64_t underruns = 0u;
		RgsPlayerStreamSource source = RGS_PLAYER_STREAM_SOURCE_WAV;
		RgsPlayerUiActions actions;
		memset(&actions, 0, sizeof(actions));
		actions.select_file = -1;
		actions.source = -1;
		rg_input_begin_frame(&input_state);
		rg_input_event_queue_reset(&input_events, SDL_GetModState());
		while (SDL_PollEvent(&event))
		{
			if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
				running = 0;
			rg_input_process_event_ex(&input_state, &event, &input_events);
		}
		rg_input_sample(&input_state);
		if (rg_input_is_key_pressed(&input_state, SDL_SCANCODE_ESCAPE))
			running = 0;
		if (!running)
			break;
		if (rg_input_is_key_pressed(&input_state, SDL_SCANCODE_SPACE))
			actions.toggle_pause = 1;
		if (rg_input_is_key_pressed(&input_state, SDL_SCANCODE_R))
			actions.restart = 1;
		if (rg_input_is_key_pressed(&input_state, SDL_SCANCODE_LEFT))
			actions.previous = 1;
		if (rg_input_is_key_pressed(&input_state, SDL_SCANCODE_RIGHT))
			actions.next = 1;

		if (!SDL_LockAudioStream(player->audio))
			goto cleanup;
		cursor = player->timeline.cursor;
		underruns = player->timeline.underruns;
		source = player->timeline.source;
		SDL_UnlockAudioStream(player->audio);
		if (rg_input_is_key_pressed(&input_state, SDL_SCANCODE_TAB))
			actions.source = source == RGS_PLAYER_STREAM_SOURCE_WAV ? RGS_PLAYER_STREAM_SOURCE_RGS : RGS_PLAYER_STREAM_SOURCE_WAV;
		if (rg_input_is_key_pressed(&input_state, SDL_SCANCODE_1))
			actions.source = RGS_PLAYER_STREAM_SOURCE_WAV;
		if (rg_input_is_key_pressed(&input_state, SDL_SCANCODE_2) ||
		    rg_input_is_key_pressed(&input_state, SDL_SCANCODE_B))
			actions.source = RGS_PLAYER_STREAM_SOURCE_RGS;

		now = SDL_GetTicksNS();
		delta_time = (f32)((f64)(now - previous_ticks) / 1000000000.0);
		previous_ticks = now;
		if (delta_time > 0.1f)
			delta_time = 0.1f;
		if (!SDL_GetWindowSizeInPixels(window, &width, &height))
			goto cleanup;
		rg_gui_begin_frame_ex(&gui, &input_state, &input_events, window_id, delta_time);
		rgs_player_build_ui(&gui,
		                    player,
		                    width,
		                    height,
		                    cursor,
		                    underruns,
		                    source,
		                    &actions);
		rg_gui_end_frame(&gui);
		if (!rgs_player_cursor_apply(&cursor_state, rg_gui_platform_output(&gui)) ||
		    !rgs_player_render_frame(device,
		                             window,
		                             &gpu,
		                             &text_renderer,
		                             &upload_ring,
		                             &gui))
			goto cleanup;
		submitted_frames += 1;

		if (actions.source >= 0 &&
		    !rgs_player_set_source(player, (RgsPlayerStreamSource)actions.source))
			goto cleanup;
		if (actions.toggle_pause && !rgs_player_set_paused(player, !player->paused))
			goto cleanup;
		if (actions.restart && !rgs_player_restart(player))
			goto cleanup;
		if (actions.save)
		{
			if (rgs_player_save_track(&player->track))
				SDL_snprintf(player->status,
				             sizeof(player->status),
				             "Saved %s",
				             player->track.sidecar_path);
			else
				SDL_snprintf(player->status,
				             sizeof(player->status),
				             "Save failed: %s",
				             SDL_GetError());
		}
		if (!smoke_test && actions.previous)
		{
			size_t previous = player->file_index == 0u ? player->files.count - 1u : player->file_index - 1u;
			if (!rgs_player_load_index(player, previous))
				SDL_snprintf(player->status, sizeof(player->status), "Load failed: %s", SDL_GetError());
		}
		else if (!smoke_test && actions.next)
		{
			if (!rgs_player_load_index(player, (player->file_index + 1u) % player->files.count))
				SDL_snprintf(player->status, sizeof(player->status), "Load failed: %s", SDL_GetError());
		}
		else if (!smoke_test && actions.select_file >= 0 &&
		         (size_t)actions.select_file != player->file_index)
		{
			if (!rgs_player_load_index(player, (size_t)actions.select_file))
				SDL_snprintf(player->status, sizeof(player->status), "Load failed: %s", SDL_GetError());
		}
		if (frame_limit > 0 && submitted_frames >= frame_limit)
			running = 0;
	}
	exit_code = 0;

cleanup:
	if (exit_code != 0)
		fprintf(stderr, "rgs_player failed: %s\n", SDL_GetError());
	if (player != NULL)
	{
		if (player->audio != NULL)
			SDL_PauseAudioStreamDevice(player->audio);
		rgs_player_stop_worker(player);
		if (player->audio != NULL)
			SDL_DestroyAudioStream(player->audio);
	}
	if (device != NULL)
		rg_gpu_wait_idle(device);
	if (upload_initialized)
		rg_gpu_upload_ring_destroy(&upload_ring);
	if (gpu_initialized)
		rg_gui_gpu_destroy(&gpu);
	(void)text_initialized;
	(void)gui_initialized;
	free(text_memory);
	free(gui_memory);
	if (atlas != NULL)
		SDL_ReleaseGPUTexture(device, atlas);
	rgs_player_font_destroy(&font_assets);
	rgs_player_cursor_destroy(&cursor_state);
	if (window_claimed)
		SDL_ReleaseWindowFromGPUDevice(device, window);
	if (device != NULL)
		rg_gpu_device_destroy(device);
	if (window != NULL)
		SDL_DestroyWindow(window);
	if (player != NULL)
	{
		rgs_player_track_destroy(&player->track);
		rgs_player_file_list_destroy(&player->files);
		free(player);
	}
	if (sdl_initialized)
		SDL_Quit();
	return exit_code;
}
