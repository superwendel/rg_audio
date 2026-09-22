#include "../tools/rgs_player_stream.h"

#include <stdio.h>

static int failures;

#define CHECK(condition)                                                                  \
	do                                                                                    \
	{                                                                                     \
		if (!(condition))                                                                 \
		{                                                                                 \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
			failures += 1;                                                                \
		}                                                                                 \
	} while (0)

static void fill_slot(RgsPlayerStreamRing* ring, uint32_t frames, int16_t first)
{
	RgsPlayerStreamSlot* slot = rgs_player_stream_producer_acquire(ring);
	uint32_t i;
	CHECK(slot != NULL);
	if (slot == NULL)
		return;
	for (i = 0u; i < frames; ++i)
		slot->pcm[(size_t)i * ring->channels] = (int16_t)(first + (int16_t)i);
	CHECK(rgs_player_stream_producer_commit(ring, frames));
}

static void test_ring_wrap_and_backpressure(void)
{
	static RgsPlayerStreamRing ring;
	uint32_t i;
	uint32_t available = 0u;
	const int16_t* pcm;
	rgs_player_stream_ring_init(&ring, 1u);
	for (i = 0u; i < RGS_PLAYER_STREAM_SLOT_COUNT; ++i)
		fill_slot(&ring, 3u, (int16_t)(10 * (int)i));
	CHECK(rgs_player_stream_ring_count(&ring) == RGS_PLAYER_STREAM_SLOT_COUNT);
	CHECK(rgs_player_stream_producer_acquire(&ring) == NULL);
	CHECK(!rgs_player_stream_producer_commit(&ring, 1u));
	pcm = rgs_player_stream_consumer_peek(&ring, &available);
	CHECK(pcm != NULL && available == 3u && pcm[0] == 0);
	CHECK(rgs_player_stream_consumer_advance(&ring, 2u));
	pcm = rgs_player_stream_consumer_peek(&ring, &available);
	CHECK(pcm != NULL && available == 1u && pcm[0] == 2);
	CHECK(rgs_player_stream_consumer_advance(&ring, 1u));
	CHECK(rgs_player_stream_ring_count(&ring) == RGS_PLAYER_STREAM_SLOT_COUNT - 1u);
	fill_slot(&ring, 2u, 99);
	CHECK(rgs_player_stream_ring_count(&ring) == RGS_PLAYER_STREAM_SLOT_COUNT);
	for (i = 0u; i < RGS_PLAYER_STREAM_SLOT_COUNT - 1u; ++i)
		CHECK(rgs_player_stream_consumer_advance(&ring, 3u));
	pcm = rgs_player_stream_consumer_peek(&ring, &available);
	CHECK(pcm != NULL && available == 2u && pcm[0] == 99);
}

static void test_ab_alignment_and_wav_discard(void)
{
	static RgsPlayerStreamRing ring;
	RgsPlayerStreamTimeline timeline;
	int16_t wav[8] = {100, 101, 102, 103, 104, 105, 106, 107};
	int16_t out[4] = {0};
	rgs_player_stream_ring_init(&ring, 1u);
	rgs_player_stream_timeline_init(&timeline, 8u, 1u);
	fill_slot(&ring, 4u, 10);
	fill_slot(&ring, 4u, 14);
	CHECK(rgs_player_stream_pull(&ring, &timeline, wav, out, 3u) == 3u);
	CHECK(out[0] == 100 && out[1] == 101 && out[2] == 102);
	CHECK(timeline.cursor == 3u);
	CHECK(ring.read_offset == 3u);
	timeline.source = RGS_PLAYER_STREAM_SOURCE_RGS;
	CHECK(rgs_player_stream_pull(&ring, &timeline, wav, out, 3u) == 3u);
	CHECK(out[0] == 13 && out[1] == 14 && out[2] == 15);
	CHECK(timeline.cursor == 6u);
	CHECK(rgs_player_stream_ring_count(&ring) == 1u);
	CHECK(ring.read_offset == 2u);
}

static void test_underrun_freeze_and_recovery(void)
{
	static RgsPlayerStreamRing ring;
	RgsPlayerStreamTimeline timeline;
	int16_t wav[4] = {1, 2, 3, 4};
	int16_t out[4] = {9, 9, 9, 9};
	rgs_player_stream_ring_init(&ring, 1u);
	rgs_player_stream_timeline_init(&timeline, 4u, 1u);
	timeline.source = RGS_PLAYER_STREAM_SOURCE_RGS;
	CHECK(rgs_player_stream_pull(&ring, &timeline, wav, out, 4u) == 0u);
	CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);
	CHECK(timeline.cursor == 0u && timeline.underruns == 1u);
	fill_slot(&ring, 2u, 40);
	out[2] = 9;
	out[3] = 9;
	CHECK(rgs_player_stream_pull(&ring, &timeline, wav, out, 4u) == 2u);
	CHECK(out[0] == 40 && out[1] == 41 && out[2] == 0 && out[3] == 0);
	CHECK(timeline.cursor == 2u && timeline.underruns == 2u);
}

static void test_restart_and_safe_replacement(void)
{
	static RgsPlayerStreamRing ring;
	RgsPlayerStreamTimeline timeline;
	int16_t wav[6] = {5, 6, 7, 8, 9, 10};
	int16_t out[2];
	rgs_player_stream_ring_init(&ring, 1u);
	rgs_player_stream_timeline_init(&timeline, 6u, 1u);
	fill_slot(&ring, 4u, 20);
	CHECK(rgs_player_stream_pull(&ring, &timeline, wav, out, 2u) == 2u);
	CHECK(timeline.cursor == 2u && ring.read_offset == 2u);
	/* This is the stopped-producer/locked-consumer replacement sequence. */
	rgs_player_stream_ring_reset(&ring);
	rgs_player_stream_timeline_init(&timeline, 3u, 1u);
	CHECK(rgs_player_stream_ring_count(&ring) == 0u && ring.read_offset == 0u);
	fill_slot(&ring, 3u, 70);
	timeline.source = RGS_PLAYER_STREAM_SOURCE_RGS;
	CHECK(rgs_player_stream_pull(&ring, &timeline, wav, out, 2u) == 2u);
	CHECK(out[0] == 70 && out[1] == 71 && timeline.cursor == 2u);
	rgs_player_stream_timeline_restart(&timeline);
	CHECK(timeline.cursor == 0u);
}

static void test_exact_reference_crop_and_pad(void)
{
	int16_t source[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
	int16_t padded[10];
	int16_t cropped[6];
	CHECK(rgs_player_stream_copy_exact_s16(padded, 5u, source, 3u, 2u) == 3u);
	CHECK(padded[0] == 1 && padded[1] == 2 && padded[4] == 5 && padded[5] == 6);
	CHECK(padded[6] == 0 && padded[7] == 0 && padded[8] == 0 && padded[9] == 0);
	CHECK(rgs_player_stream_copy_exact_s16(cropped, 3u, source, 5u, 2u) == 3u);
	CHECK(cropped[0] == 1 && cropped[1] == 2 && cropped[4] == 5 && cropped[5] == 6);
}

int main(void)
{
	test_ring_wrap_and_backpressure();
	test_ab_alignment_and_wav_discard();
	test_underrun_freeze_and_recovery();
	test_restart_and_safe_replacement();
	test_exact_reference_crop_and_pad();
	if (failures != 0)
	{
		fprintf(stderr, "%d player stream test(s) failed\n", failures);
		return 1;
	}
	printf("player stream tests passed\n");
	return 0;
}
