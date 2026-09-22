# Streaming and realtime decode

RGS provides an allocation-free, frame-at-a-time decoder for bounded decode
work outside an audio callback. The API operates on a complete encoded buffer;
*streaming* here means incremental PCM production, not incremental receipt of
network bytes.

## Decoder lifecycle

`RgRgsDecoder` borrows its source bytes. The caller MUST keep the encoded buffer
at the same address and unchanged from successful initialization until the last
decode call or reset is complete. A decoder is intended to be owned by one
thread at a time.

```c
RgRgsDecoder decoder;
RgRgsInfo info;

if (!rg_rgs_decoder_init(&decoder, encoded, encoded_size, &info))
    return 0;

int16_t frame[RG_RGS_MAX_FRAME_SAMPLES * RG_RGS_MAX_CHANNELS];

for (;;)
{
    uint32_t frame_samples = 0;
    RgRgsDecodeStatus status = rg_rgs_decoder_next_s16(
        &decoder, frame, sizeof(frame) / sizeof(frame[0]), &frame_samples);

    if (status == RG_RGS_DECODE_FRAME)
    {
        consume_pcm(frame, frame_samples, info.channels);
        continue;
    }
    if (status == RG_RGS_DECODE_END)
        break;
    return 0;
}
```

Destination capacity is measured in total interleaved `int16_t` values. The
reported `frame_samples` count is per channel.

The statuses have these state-transition rules:

| Status | Meaning | Decoder advances? |
| --- | --- | --- |
| `RG_RGS_DECODE_FRAME` | one complete frame was validated and decoded | yes |
| `RG_RGS_DECODE_END` | declared sample count and exact source boundary were reached | no |
| `RG_RGS_DECODE_OUTPUT_TOO_SMALL` | destination cannot hold the next frame; reported frame count is the required per-channel count | no |
| `RG_RGS_DECODE_INVALID` | malformed/truncated/trailing/inconsistent stream or invalid arguments | decoder is poisoned |

After `RG_RGS_DECODE_INVALID`, later `next` calls return invalid without
reading more input. `rg_rgs_decoder_reset` rewinds to the first frame and
clears that failure state while retaining the same borrowed source. Resetting
does not make malformed bytes valid; it permits a caller to restart after an
application-level cancellation or to reproduce a failure deterministically.

`rg_rgs_decoder_init` validates the v1 file header and first-frame metadata.
Each `next` call validates its complete frame. Reaching the declared sample
count is not enough for `END`: the decoder also requires exact end of the
borrowed buffer, so trailing data becomes `INVALID`.

## Whole-file and trusted APIs

`rg_rgs_decode_s16` builds the checked whole-file operation on the same frame
validation rules. Prefer it when all decoded PCM is wanted at once or when
validating an asset during import.

`rg_rgs_decode_trusted_s16` is for a complete byte-for-byte stream already
accepted by a checked decode. It may omit payload checks that make malformed
data safe to reject. A successful `rg_rgs_read_header` alone is not sufficient
qualification: later frames and exact EOF have not necessarily been checked.

Neither checked, trusted, nor frame-at-a-time decode allocates memory.

## Realtime playback pattern

Do not parse or decode compressed frames in an audio callback. Decode ahead on
a worker or game thread into fixed PCM slots, then let the callback perform
only bounded copies. Four slots sized for the maximum RGS frame are a useful
starting point:

```text
encoded RGS buffer (stable, read-only)
             |
             v
      worker-owned decoder
             |
             v
  [slot 0][slot 1][slot 2][slot 3]  single-producer/single-consumer ring
             |
             v
      audio callback / stream copy
```

Each slot needs
`RG_RGS_MAX_FRAME_SAMPLES * channels * sizeof(int16_t)` bytes plus valid-frame
and read-offset metadata. Prefill at least two slots before unpausing playback.
The producer publishes a slot only after all PCM and metadata are written; the
consumer releases it only after the last value is copied. Use platform atomics
with acquire/release ordering, or an equivalently correct SPSC primitive.

The callback should not allocate, free, decode, open files, log, wait, or take a
contended mutex. It should copy available PCM to the device stream and return.
If no frame is ready, output silence and count an underrun; preserving the
logical cursor makes recovery and A/B comparison deterministic.

## Loop, restart, and replacement

At `RG_RGS_DECODE_END`, a loop worker may reset the decoder and continue
filling slots. For gapless looping, the consumer timeline and any reference WAV
must wrap at the same declared sample count.

Replacing a track or restarting playback requires an ownership barrier:

1. Pause device consumption.
2. Ask the producer to stop and join it.
3. Lock or otherwise exclude the callback, clear the device stream and ring,
   and reset the shared cursor.
4. Keep the old encoded buffer alive until the producer has joined.
5. Install the new stable buffer, initialize/reset the decoder, start the
   producer, and prefill the ring.
6. Resume device consumption.

Stopping a producer while the ring is full must not depend on the consumer
freeing a slot. Include the stop condition in every producer wait or use
nonblocking retries with a bounded scheduling yield.

## A/B comparison alignment

For WAV/RGS comparison, normalize the reference WAV once to the RGS stored
channel count, rate, and exact sample count. Maintain one common timeline. The
RGS producer and consumer must continue to advance the compressed ring even
while the WAV source is audible; otherwise switching sources compares different
moments. A source toggle should not flush the queued device stream.

If either source underruns, emit silence without advancing the common timeline
or consuming the other source. This keeps subsequent recovery sample-aligned.

## Memory budgeting

The decoder itself is small and borrows encoded data. A four-slot ring requires
at most:

```text
4 * 5120 * channels * 2 bytes
```

or 327,680 bytes at the eight-channel maximum, excluding small slot metadata.
Applications can choose fewer slots to reduce memory, but must account for
worker scheduling and device-buffer latency. Benchmark the copy and decode
paths independently on the target hardware; see [benchmarks.md](benchmarks.md).
