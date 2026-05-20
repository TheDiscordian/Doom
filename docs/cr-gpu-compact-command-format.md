# CR: Compact GPU Command Format

Status: Draft

## Problem

The GPU renderer is now often limited by command traffic rather than by present
or framebuffer copy cost. Recent perf logs show `min_ring=0` and many
`ring_wait` events while columns and spans are submitted through the current
fixed-size span packets.

The current wire format is simple, but expensive for common affine paths:

| Draw type | Current command | Words per draw | Waste |
|-----------|-----------------|----------------|-------|
| Scalar affine span | header + 15 payload | 16 | Sends 6 unused perspective words |
| 2/4-lane span group | header + 18 payload | 19 | Repeats shared stride/texture/mask fields per group |

When the ring fills, the CPU stalls until the GPU drains enough command words.
Reducing command words should increase effective ring capacity, reduce command
DMA flush/drain cost, and reduce decoder work.

## Goals

- Reduce command words for scalar affine spans by about 40%.
- Reduce command words for grouped affine spans when consecutive groups share
  common stride, texture pitch, wrap mask, flags, and colormap state.
- Keep the existing `GPU_CMD_DRAW_SPAN_GROUP` command as the compatibility
  fallback.
- Preserve draw order exactly. No sorting, merging across ordering boundaries,
  or delayed texture/lump release changes.
- Make the change measurable with the existing `[render_perf]` counters plus
  the new command timing counters.

## Non-Goals

- Compress framebuffer contents, textures, palettes, or save data.
- Increase the ring buffer size.
- Change the pixel pipeline or the texture cache.
- Add engine-specific command formats.

## Proposed Commands

Reserve two new command IDs:

```c
#define GPU_CMD_DRAW_SPAN_AFFINE_BATCH          0x44
#define GPU_CMD_DRAW_SPAN_GROUP_COMPACT_BATCH   0x45
```

Both commands keep the existing command header:

```text
word 0: [31:24] command id, [23:0] payload_words
```

The payload length determines how many fixed-size descriptors follow. This
avoids an extra count word.

## 0x44: DRAW_SPAN_AFFINE_BATCH

Use this for scalar spans where `OF_GPU_SPAN_PERSP` is not set. The descriptor
is the first 9 payload words of the current scalar span format. The six
perspective words are omitted and the decoder internally feeds zero/default
perspective values.

Payload rule:

```text
payload_words must be a multiple of 9
descriptor_count = payload_words / 9
```

Descriptor layout:

| Word | Field |
|------|-------|
| 0 | `fb_addr` |
| 1 | `tex_addr` |
| 2 | `s` |
| 3 | `t` |
| 4 | `sstep` |
| 5 | `tstep` |
| 6 | `[31:28] colormap_id, [27:16] count, [13:8] light, [7:0] flags` |
| 7 | `[31:16] fb_stride, [15:0] tex_width` |
| 8 | `[31:16] tex_h_mask, [15:0] tex_w_mask` |

The RTL should reject or ignore descriptors with `OF_GPU_SPAN_PERSP` set. The
SDK helper should route perspective spans to the existing 0x43 command.

Savings:

```text
old: header + N * (15 payload + 1 header) = 16N words
new: header + N * 9 payload              = 1 + 9N words
```

At 128 spans, this is 2048 words down to 1153 words, a 43.7% reduction.

## 0x45: DRAW_SPAN_GROUP_COMPACT_BATCH

Use this for grouped affine spans where consecutive groups share common state.
This is intended to cover vertical column groups, sprite strip groups, and
other engines that can emit several adjacent affine lanes through the same
fragment path.

The compact command is not tied to any game. It preserves the current 0x43
span-group semantics, but hoists common fields into a batch prefix:

- `flags`
- `colormap_id`
- `fb_stride`
- `lane_delta`
- `tex_width`
- `tex_w_mask`
- `tex_h_mask`

Fallback to 0x43 whenever those fields change before enough groups have been
collected to make the compact batch worthwhile.

Payload rule:

```text
payload_words = 4 + descriptor_count * 15
descriptor_count = (payload_words - 4) / 15
```

Batch prefix:

| Word | Field |
|------|-------|
| 0 | `[31:28] colormap_id, [23:16] flags, [15:0] batch_options_reserved` |
| 1 | `[31:16] fb_stride, [15:0] lane_delta` |
| 2 | `tex_width` |
| 3 | `[31:16] tex_h_mask, [15:0] tex_w_mask` |

`batch_options_reserved` must be zero for this revision.

Descriptor layout, repeated `descriptor_count` times:

| Word | Field |
|------|-------|
| 0 | `fb_addr` for lane 0 |
| 1 | `[27:16] count, [15:14] lane_count_code, [13:0] reserved` |
| 2 | `tex_addr[0]` |
| 3 | `tex_addr[1]` |
| 4 | `tex_addr[2]` |
| 5 | `tex_addr[3]` |
| 6 | `t[0]` |
| 7 | `t[1]` |
| 8 | `t[2]` |
| 9 | `t[3]` |
| 10 | `tstep[0]` |
| 11 | `tstep[1]` |
| 12 | `tstep[2]` |
| 13 | `tstep[3]` |
| 14 | `[31:24] light[3], [23:16] light[2], [15:8] light[1], [7:0] light[0]` |

`lane_count_code` maps to native lane counts:

| Code | Lanes |
|------|-------|
| 0 | invalid |
| 1 | 1 lane |
| 2 | 2 lanes |
| 3 | 4 lanes |

Unused lanes must be encoded as zero by the SDK and ignored by RTL.
Reserved bits must be zero.

The decoder expands this descriptor into the same internal span-group state as
0x43 using the batch prefix for the common fields. The texture addresses,
coordinates, steps, counts, and lights remain per group/per lane.

Savings:

```text
old: header + N * (18 payload + 1 header) = 19N words
new: header + 4 prefix + N * 15 payload = 5 + 15N words
```

At 64 groups, this is 1216 words down to 965 words, a 20.6% reduction. The
break-even point is two groups:

```text
2 groups old = 38 words
2 groups new = 35 words
```

## SDK API

Add helpers beside the existing APIs:

```c
void of_gpu_draw_span_affine_batch(const of_gpu_span_t *spans, int count);
void of_gpu_draw_span_group_compact_batch(const of_gpu_span_group_t *groups,
                                          int count);
```

Callers can use a local command-stream buffer and emit mixed compact commands
in draw order:

- scalar affine spans route to `GPU_CMD_DRAW_SPAN_AFFINE_BATCH`
- compatible grouped spans route to `GPU_CMD_DRAW_SPAN_GROUP_COMPACT_BATCH`
- non-matching spans/groups route to existing `GPU_CMD_DRAW_SPAN_GROUP`

The SDK should flush only at command boundaries and should preserve the
caller's existing resource lifetime and ordering behavior.

## RTL Decode

Implementation should be a decode-side expansion, not a new raster path:

1. Validate `payload_words` is a multiple of descriptor size.
2. Loop over descriptors.
3. Fill the existing span/scalar or span-group registers.
4. Start the existing fragment pipeline.
5. Continue to the next descriptor only when the previous draw is accepted by
   the pipeline.

This keeps risk low because texture fetch, colormap, translucency, skip-zero,
and framebuffer write behavior remain unchanged.

## Compatibility

Existing command IDs and helpers remain valid. New SDK helpers should be guarded
by a capability bit before they are used by game code:

```c
#define OF_GPU_CAP_COMPACT_SPANS    (1u << n)
```

If the bit is missing, callers keep using the current 0x43 path.

## Validation

Functional checks:

- boot the current port's IWAD, shareware, SIGIL, Earth, and Revolution
  instances as regression coverage
- verify menus, intermission, status bar, fuzz, translated sprites, and melt
- verify GPU_FLIP still presents the correct rotating framebuffer
- compare screenshots or framebuffer hashes against the 0x43 path for static
  scenes

Performance checks:

- `cmd_words/s` should drop sharply in scenes with many scalar affine spans
- `cmd_flush_words/s` should drop similarly
- `ring_wait_ms` and `dma_wait_ms` should drop if the renderer is command-ring
  bound
- `min_ring` should stop reaching zero in lighter scenes
- FPS should improve most in scenes with many affine spans or repeated
  span-group state

## Risks

- Compact group batches only win when groups share state. The SDK should avoid
  emitting a one-group compact batch unless it is immediately followed by more
  compatible groups.
- Batching descriptors under one command means the decoder needs an internal
  descriptor loop. It must not fetch the next descriptor until the current draw
  has been accepted.
- If a scene is texture-cache or framebuffer-write limited, fewer command words
  may not move FPS much. The new `gpu_time` counters will show this by keeping
  `ring_wait_ms` low while frame time remains high.

## Recommended Rollout

1. Implement 0x44 first. It is the largest win and only removes unused
   perspective fields.
2. Add SDK routing for affine scalar spans and compare framebuffer output.
3. Measure `cmd_words/s`, `ring_wait_ms`, and FPS.
4. Implement 0x45 only if grouped-span scenes still show `min_ring=0` or high
   `ring_wait_ms`.
5. Keep a runtime `-nogpucompact` switch until representative content from
   multiple engines or WADs has been tested.
