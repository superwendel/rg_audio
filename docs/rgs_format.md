# RGS v1 format

This document is the normative wire specification for Reverse Gravity Signal
(RGS) version 1. The key words **MUST**, **MUST NOT**, **SHOULD**, and **MAY**
are to be interpreted as described by RFC 2119.

RGS v1 is a lossy, framed encoding of interleaved signed 16-bit PCM. It uses a
QOA-derived four-tap least-mean-squares (LMS) predictor and channel-planar
compressed payloads. All multibyte integers are little-endian. A conforming v1
decoder MUST reject every version byte other than `1`.

## Limits and terminology

- A *sample frame* contains one PCM sample for every channel. Counts called
  `samples` in the wire format are sample frames per channel.
- Channel count is 1 through 8.
- Stored sample rate is 1 through 44,100 Hz.
- A compressed slice covers 1 through 20 sample frames for one channel.
- A compressed frame covers 1 through 5,120 sample frames per channel, or at
  most 256 slices.
- File sample count is 1 through `UINT32_MAX` sample frames per channel.
- The decoded sample order is frame-major interleaved PCM16: frame 0 channel 0,
  frame 0 channel 1, and so on.

The codec accepts input rates at or below 44.1 kHz and rejects higher rates.
Higher-rate input must be low-pass filtered and resampled before encoding;
the codec does not perform resampling.

## File header

Every file begins with this 12-byte header:

| Offset | Size | Field | Required value |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | ASCII `rgs!` (`72 67 73 21`) |
| 4 | 4 | decoded sample frames per channel | little-endian unsigned 32-bit, nonzero |
| 8 | 1 | version | `1` |
| 9 | 1 | file flags | `0x07` |
| 10 | 2 | reserved | zero |

The file-flag bits advertise the three features used by the v1 frame grammar:

| Bit | Value | Meaning |
| ---: | ---: | --- |
| 0 | `0x01` | mixed-width frames may occur |
| 1 | `0x02` | all-2-bit frames may occur |
| 2 | `0x04` | frame payloads are channel-planar |

Bits 3 through 7 are reserved and MUST be zero. A v1 writer MUST emit all
three defined capability bits, hence the exact byte `0x07`.

The file header does not repeat channel count or sample rate. Those values are
read from the first frame and become invariant for the rest of the file. A
header without at least one complete frame is invalid.

## Frame header and predictor state

Every frame starts with an 8-byte header followed by 16 bytes of initial LMS
state for each channel:

| Frame offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | channel count in low nibble; frame flags in high nibble |
| 1 | 3 | stored sample rate, unsigned 24-bit |
| 4 | 2 | sample frames in this frame |
| 6 | 2 | total frame byte size, including this header |

For channel `c`, the predictor state consists of four signed 16-bit history
values followed by four signed 16-bit weight values, each little-endian:

```text
history[c][0..3]  8 bytes
weights[c][0..3]  8 bytes
```

States appear in ascending channel order. They are the exact predictor state
at the beginning of that frame; each channel evolves independently while its
slices are decoded.

The channel nibble MUST be 1 through 8 and MUST match every other frame. The
sample rate MUST be 1 through 44,100 and MUST match every other frame. Frame
sample count MUST equal the smaller of 5,120 and the file's remaining declared
sample count. Consequently, every frame except the last has exactly 5,120
sample frames and the last has 1 through 5,120. Frame size MUST describe
exactly one valid payload and MUST not exceed the remaining file bytes.

### Permitted frame flags

Only these high-nibble values are valid:

| Flags | Name | Payload |
| ---: | --- | --- |
| `0x4` | planar fixed | every slice uses a 3-bit code in 8 bytes |
| `0x5` | planar mixed | a per-channel mode map selects 3-bit or 2-bit slices |
| `0x6` | planar all-2-bit | every slice uses a 2-bit code in 6 bytes |

In particular, mixed (`0x1`) and all-2-bit (`0x2`) are mutually exclusive;
planar (`0x4`) is mandatory; and bit `0x8` is reserved. A decoder MUST reject
every other combination.

Let:

```text
slices     = ceil(frame_samples / 20)
state_size = 16 * channels
base_size  = 8 + state_size
mode_bytes = ceil(slices / 8)
```

The exact fixed-frame size is `base_size + 8 * slices * channels`. The exact
all-2-bit size is `base_size + 6 * slices * channels`.

## Slice payloads

Payloads are channel-planar: all compressed information for channel 0 precedes
all compressed information for channel 1, and so on. Within a channel, slices
appear in timeline order. Every slice except the last covers 20 sample frames;
the last covers the remaining 1 through 20.

### 3-bit slice

A 3-bit slice is an unsigned 64-bit little-endian word:

```text
bits  0..3   scalefactor index, 0..15
bits  4..6   residual code for sample 0
bits  7..9   residual code for sample 1
...
bits 61..63  residual code for sample 19
```

Codes beyond the actual sample count of a partial final slice SHOULD be zero
and are ignored by decoders.

### 2-bit slice

A 2-bit slice is an unsigned 48-bit little-endian word:

```text
bits  0..3   scalefactor index, 0..15
bits  4..5   residual code for sample 0
bits  6..7   residual code for sample 1
...
bits 42..43  residual code for sample 19
bits 44..47  padding, zero in canonical output
```

Codes beyond the actual sample count of a partial final slice are ignored.
Writers SHOULD zero those codes and MUST zero the four high padding bits.

### Mixed mode maps

A mixed frame stores `mode_bytes * channels` bytes immediately after all LMS
states. Mode-map blocks appear in channel order. In each block, bit `s & 7` of
byte `s / 8` selects slice `s`: zero means an 8-byte 3-bit slice and one means a
6-byte 2-bit slice. Unused high bits in the last mode byte MUST be zero in
canonical output.

After all mode-map blocks, payload slices appear channel-planar in the same
order as fixed and all-2-bit frames. The exact mixed frame size is therefore:

```text
base_size + mode_bytes * channels +
sum(payload size selected for every channel and slice)
```

A decoder MUST consume exactly that many bytes and MUST require the result to
equal the frame-size field.

## Residual reconstruction

The following signed dequantizer tables define the decoded residual. Rows are
indexed by the 4-bit scalefactor and columns by the residual code.

For 3-bit slices:

```text
{    1,    -1,     3,    -3,     5,    -5,     7,    -7 },
{    5,    -5,    18,   -18,    32,   -32,    49,   -49 },
{   16,   -16,    53,   -53,    95,   -95,   147,  -147 },
{   34,   -34,   113,  -113,   203,  -203,   315,  -315 },
{   63,   -63,   210,  -210,   378,  -378,   588,  -588 },
{  104,  -104,   345,  -345,   621,  -621,   966,  -966 },
{  158,  -158,   528,  -528,   950,  -950,  1477, -1477 },
{  228,  -228,   760,  -760,  1368, -1368,  2128, -2128 },
{  316,  -316,  1053, -1053,  1895, -1895,  2947, -2947 },
{  422,  -422,  1405, -1405,  2529, -2529,  3934, -3934 },
{  548,  -548,  1828, -1828,  3290, -3290,  5117, -5117 },
{  696,  -696,  2320, -2320,  4176, -4176,  6496, -6496 },
{  868,  -868,  2893, -2893,  5207, -5207,  8099, -8099 },
{ 1064, -1064,  3548, -3548,  6386, -6386,  9933, -9933 },
{ 1286, -1286,  4288, -4288,  7718, -7718, 12005,-12005 },
{ 1536, -1536,  5120, -5120,  9216, -9216, 14336,-14336 }
```

For 2-bit slices:

```text
{    1,    -1,     5,    -5 },
{    5,    -5,    32,   -32 },
{   16,   -16,    95,   -95 },
{   34,   -34,   203,  -203 },
{   63,   -63,   378,  -378 },
{  104,  -104,   621,  -621 },
{  158,  -158,   950,  -950 },
{  228,  -228,  1368, -1368 },
{  316,  -316,  1895, -1895 },
{  422,  -422,  2529, -2529 },
{  548,  -548,  3290, -3290 },
{  696,  -696,  4176, -4176 },
{  868,  -868,  5207, -5207 },
{ 1064, -1064,  6386, -6386 },
{ 1286, -1286,  7718, -7718 },
{ 1536, -1536,  9216, -9216 }
```

For each decoded residual `r`, compute the next sample and update the selected
channel's LMS state in this order:

```text
prediction = floor(sum(weights[i] * history[i], i=0..3) / 8192)
sample     = clamp_to_int16(prediction + r)
delta      = floor(r / 16)

for i = 0..3:
    weights[i] += history[i] < 0 ? -delta : delta

history[0] = history[1]
history[1] = history[2]
history[2] = history[3]
history[3] = sample
```

`floor` here is mathematical floor, including for negative operands; it is not
C integer division's truncation toward zero. Implementations SHOULD use wide
intermediates for the dot product and additions and MUST avoid signed overflow.
The output sample is clamped to `[-32768, 32767]` before the state update.

## Whole-file validation

A checked decoder MUST validate the file and every frame before claiming
success. In addition to the local rules above, it MUST enforce all of these
whole-stream invariants:

1. Every frame has the first frame's channel count and sample rate.
2. Every frame contains `min(5120, remaining_file_samples)` sample frames.
3. Each frame payload ends exactly at its declared frame boundary.
4. The sum of frame sample counts equals the file header's sample count without
   integer overflow or overrun.
5. The final frame ends exactly at the supplied file-buffer boundary. Trailing
   bytes are invalid.
6. The destination has room for `file_samples * channels` PCM16 values.

RGS v1 has no checksum or recovery marker. A container that concatenates or
streams RGS objects MUST provide each complete object's exact byte boundary.

## Reference encoder behavior

The reference encoder's initial search uses histories `{0, 0, 0, 0}` and
weights `{0, 0, -8192, 16384}`, then carries the evolved state between frames.
Prediction starts from the signed 16-bit state stored in each frame header.
The encoder measures decoded error per frame and channel; difficult channels
may be retried with another initial predictor state or slice-width policy.
Only a retry with lower squared PCM error is retained, and its initial state
is stored in the frame header. Decoders MUST use that stored state rather
than assume continuity with a previous frame.

High quality writes planar fixed 3-bit frames. Medium and low may also use
2-bit slices. Uniform frames omit the mode map; mixed frames retain it even
when the map makes the frame slightly larger than a fixed-width alternative.
Encoder revisions may produce different valid bytes and reconstructed PCM
without changing the v1 wire format.

Quality choice and `target_kbps` affect encoder decisions, not the decoder or
wire grammar. The hint is intentionally nonbinding and MUST NOT be recorded as
stream metadata.
