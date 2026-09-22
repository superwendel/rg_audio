#ifndef RGS_PLAYER_STREAM_H
#define RGS_PLAYER_STREAM_H

/*
 * Small, allocation-free pieces of the RGS player data path.
 *
 * The producer owns write_sequence and every slot it has acquired. The audio
 * consumer owns read_sequence, read_offset, and the timeline.  A host may
 * replace the default scalar atomics before including this file; rgs_player.c
 * does that with SDL's sequentially-consistent atomics.  The defaults keep the
 * state machine independently testable without SDL, threads, or an audio
 * device.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef RGS_PLAYER_STREAM_SLOT_COUNT
#define RGS_PLAYER_STREAM_SLOT_COUNT 4u
#endif

#ifndef RGS_PLAYER_STREAM_MAX_FRAME_SAMPLES
#ifdef RG_RGS_MAX_FRAME_SAMPLES
#define RGS_PLAYER_STREAM_MAX_FRAME_SAMPLES RG_RGS_MAX_FRAME_SAMPLES
#else
#define RGS_PLAYER_STREAM_MAX_FRAME_SAMPLES 5120u
#endif
#endif

#ifndef RGS_PLAYER_STREAM_MAX_CHANNELS
#ifdef RG_RGS_MAX_CHANNELS
#define RGS_PLAYER_STREAM_MAX_CHANNELS RG_RGS_MAX_CHANNELS
#else
#define RGS_PLAYER_STREAM_MAX_CHANNELS 8u
#endif
#endif

#ifndef RGS_PLAYER_STREAM_ATOMIC_TYPE
#define RGS_PLAYER_STREAM_ATOMIC_TYPE uint32_t
#define RGS_PLAYER_STREAM_ATOMIC_LOAD(value) (*(value))
#define RGS_PLAYER_STREAM_ATOMIC_STORE(value, desired) (*(value) = (uint32_t)(desired))
#endif

typedef enum RgsPlayerStreamSource
{
	RGS_PLAYER_STREAM_SOURCE_WAV = 0,
	RGS_PLAYER_STREAM_SOURCE_RGS = 1
} RgsPlayerStreamSource;

typedef struct RgsPlayerStreamSlot
{
	uint32_t frames;
	int16_t pcm[RGS_PLAYER_STREAM_MAX_FRAME_SAMPLES * RGS_PLAYER_STREAM_MAX_CHANNELS];
} RgsPlayerStreamSlot;

typedef struct RgsPlayerStreamRing
{
	RGS_PLAYER_STREAM_ATOMIC_TYPE read_sequence;
	RGS_PLAYER_STREAM_ATOMIC_TYPE write_sequence;
	uint32_t channels;
	uint32_t read_offset;
	RgsPlayerStreamSlot slots[RGS_PLAYER_STREAM_SLOT_COUNT];
} RgsPlayerStreamRing;

typedef struct RgsPlayerStreamTimeline
{
	uint64_t cursor;
	uint64_t underruns;
	uint32_t frames;
	uint32_t channels;
	RgsPlayerStreamSource source;
} RgsPlayerStreamTimeline;

static inline void rgs_player_stream_ring_init(RgsPlayerStreamRing* ring, uint32_t channels)
{
	if (ring == NULL)
		return;
	memset(ring, 0, sizeof(*ring));
	ring->channels = channels;
}

/* Only call reset after the producer is stopped and the consumer is locked. */
static inline void rgs_player_stream_ring_reset(RgsPlayerStreamRing* ring)
{
	uint32_t i;
	if (ring == NULL || ring->channels == 0u ||
	    ring->channels > RGS_PLAYER_STREAM_MAX_CHANNELS)
		return;
	RGS_PLAYER_STREAM_ATOMIC_STORE(&ring->read_sequence, 0u);
	RGS_PLAYER_STREAM_ATOMIC_STORE(&ring->write_sequence, 0u);
	ring->read_offset = 0u;
	for (i = 0u; i < RGS_PLAYER_STREAM_SLOT_COUNT; ++i)
		ring->slots[i].frames = 0u;
}

static inline uint32_t rgs_player_stream_ring_count(RgsPlayerStreamRing* ring)
{
	uint32_t read_sequence;
	uint32_t write_sequence;
	if (ring == NULL)
		return 0u;
	read_sequence = (uint32_t)RGS_PLAYER_STREAM_ATOMIC_LOAD(&ring->read_sequence);
	write_sequence = (uint32_t)RGS_PLAYER_STREAM_ATOMIC_LOAD(&ring->write_sequence);
	return write_sequence - read_sequence;
}

static inline RgsPlayerStreamSlot* rgs_player_stream_producer_acquire(RgsPlayerStreamRing* ring)
{
	uint32_t read_sequence;
	uint32_t write_sequence;
	if (ring == NULL)
		return NULL;
	read_sequence = (uint32_t)RGS_PLAYER_STREAM_ATOMIC_LOAD(&ring->read_sequence);
	write_sequence = (uint32_t)RGS_PLAYER_STREAM_ATOMIC_LOAD(&ring->write_sequence);
	if (write_sequence - read_sequence >= RGS_PLAYER_STREAM_SLOT_COUNT)
		return NULL;
	return &ring->slots[write_sequence % RGS_PLAYER_STREAM_SLOT_COUNT];
}

static inline int rgs_player_stream_producer_commit(RgsPlayerStreamRing* ring, uint32_t frames)
{
	uint32_t read_sequence;
	uint32_t write_sequence;
	RgsPlayerStreamSlot* slot;
	if (ring == NULL || frames == 0u || frames > RGS_PLAYER_STREAM_MAX_FRAME_SAMPLES)
		return 0;
	read_sequence = (uint32_t)RGS_PLAYER_STREAM_ATOMIC_LOAD(&ring->read_sequence);
	write_sequence = (uint32_t)RGS_PLAYER_STREAM_ATOMIC_LOAD(&ring->write_sequence);
	if (write_sequence - read_sequence >= RGS_PLAYER_STREAM_SLOT_COUNT)
		return 0;
	slot = &ring->slots[write_sequence % RGS_PLAYER_STREAM_SLOT_COUNT];
	slot->frames = frames;
	RGS_PLAYER_STREAM_ATOMIC_STORE(&ring->write_sequence, write_sequence + 1u);
	return 1;
}

static inline const int16_t* rgs_player_stream_consumer_peek(RgsPlayerStreamRing* ring,
                                                             uint32_t* out_frames)
{
	uint32_t read_sequence;
	uint32_t write_sequence;
	const RgsPlayerStreamSlot* slot;
	if (out_frames != NULL)
		*out_frames = 0u;
	if (ring == NULL)
		return NULL;
	read_sequence = (uint32_t)RGS_PLAYER_STREAM_ATOMIC_LOAD(&ring->read_sequence);
	write_sequence = (uint32_t)RGS_PLAYER_STREAM_ATOMIC_LOAD(&ring->write_sequence);
	if (read_sequence == write_sequence)
		return NULL;
	slot = &ring->slots[read_sequence % RGS_PLAYER_STREAM_SLOT_COUNT];
	if (slot->frames == 0u || ring->read_offset >= slot->frames)
		return NULL;
	if (out_frames != NULL)
		*out_frames = slot->frames - ring->read_offset;
	return slot->pcm + (size_t)ring->read_offset * ring->channels;
}

static inline int rgs_player_stream_consumer_advance(RgsPlayerStreamRing* ring, uint32_t frames)
{
	uint32_t available;
	uint32_t read_sequence;
	if (ring == NULL || frames == 0u ||
	    rgs_player_stream_consumer_peek(ring, &available) == NULL || frames > available)
		return 0;
	ring->read_offset += frames;
	if (ring->read_offset == ring->slots[((uint32_t)RGS_PLAYER_STREAM_ATOMIC_LOAD(
	                                         &ring->read_sequence)) %
	                                     RGS_PLAYER_STREAM_SLOT_COUNT]
	                             .frames)
	{
		read_sequence = (uint32_t)RGS_PLAYER_STREAM_ATOMIC_LOAD(&ring->read_sequence);
		ring->read_offset = 0u;
		RGS_PLAYER_STREAM_ATOMIC_STORE(&ring->read_sequence, read_sequence + 1u);
	}
	return 1;
}

static inline void rgs_player_stream_timeline_init(RgsPlayerStreamTimeline* timeline,
                                                   uint32_t frames,
                                                   uint32_t channels)
{
	if (timeline == NULL)
		return;
	timeline->cursor = 0u;
	timeline->underruns = 0u;
	timeline->frames = frames;
	timeline->channels = channels;
	timeline->source = RGS_PLAYER_STREAM_SOURCE_WAV;
}

static inline void rgs_player_stream_timeline_restart(RgsPlayerStreamTimeline* timeline)
{
	if (timeline != NULL)
		timeline->cursor = 0u;
}

/* Copy a converted S16 buffer into an exact timeline, cropping or zero-padding. */
static inline uint32_t rgs_player_stream_copy_exact_s16(int16_t* dst,
                                                        uint32_t dst_frames,
                                                        const int16_t* src,
                                                        uint32_t src_frames,
                                                        uint32_t channels)
{
	uint32_t copied_frames;
	size_t copied_values;
	size_t padded_values;
	if (dst == NULL || channels == 0u || channels > RGS_PLAYER_STREAM_MAX_CHANNELS)
		return 0u;
	copied_frames = src_frames < dst_frames ? src_frames : dst_frames;
	copied_values = (size_t)copied_frames * channels;
	if (copied_values != 0u && src != NULL)
		memcpy(dst, src, copied_values * sizeof(int16_t));
	else if (copied_values != 0u)
		copied_frames = 0u;
	padded_values = (size_t)(dst_frames - copied_frames) * channels;
	if (padded_values != 0u)
		memset(dst + (size_t)copied_frames * channels,
		       0,
		       padded_values * sizeof(int16_t));
	return copied_frames;
}

/*
 * Consume the same decoded RGS frames in both modes. In WAV mode the decoded
 * samples are discarded and timeline-aligned reference PCM is copied instead.
 * Missing ring data becomes silence; neither the ring nor timeline advances
 * for the missing suffix.
 *
 * Returns the number of real timeline frames supplied (which can be smaller
 * than requested on underrun). The whole destination is always initialized.
 */
static inline uint32_t rgs_player_stream_pull(RgsPlayerStreamRing* ring,
                                              RgsPlayerStreamTimeline* timeline,
                                              const int16_t* wav_pcm,
                                              int16_t* dst,
                                              uint32_t requested_frames)
{
	uint32_t produced = 0u;
	size_t frame_values;
	if (dst == NULL || ring == NULL || timeline == NULL || timeline->channels == 0u ||
	    timeline->channels > RGS_PLAYER_STREAM_MAX_CHANNELS || timeline->frames == 0u)
		return 0u;
	if (ring->channels != timeline->channels)
		return 0u;
	frame_values = (size_t)timeline->channels;
	while (produced < requested_frames)
	{
		uint32_t available = 0u;
		const int16_t* rgs_pcm = rgs_player_stream_consumer_peek(ring, &available);
		uint32_t chunk;
		uint32_t until_wrap;
		const int16_t* source;
		if (rgs_pcm == NULL)
		{
			memset(dst + (size_t)produced * frame_values,
			       0,
			       (size_t)(requested_frames - produced) * frame_values * sizeof(int16_t));
			timeline->underruns += 1u;
			break;
		}

		chunk = requested_frames - produced;
		if (chunk > available)
			chunk = available;
		until_wrap = timeline->frames - (uint32_t)timeline->cursor;
		if (chunk > until_wrap)
			chunk = until_wrap;
		if (timeline->source == RGS_PLAYER_STREAM_SOURCE_WAV)
		{
			if (wav_pcm == NULL)
			{
				memset(dst + (size_t)produced * frame_values,
				       0,
				       (size_t)(requested_frames - produced) * frame_values * sizeof(int16_t));
				timeline->underruns += 1u;
				break;
			}
			source = wav_pcm + (size_t)timeline->cursor * frame_values;
		}
		else
		{
			source = rgs_pcm;
		}
		memcpy(dst + (size_t)produced * frame_values,
		       source,
		       (size_t)chunk * frame_values * sizeof(int16_t));
		if (!rgs_player_stream_consumer_advance(ring, chunk))
			break;
		produced += chunk;
		timeline->cursor += chunk;
		if (timeline->cursor == timeline->frames)
			timeline->cursor = 0u;
	}
	return produced;
}

#endif /* RGS_PLAYER_STREAM_H */
