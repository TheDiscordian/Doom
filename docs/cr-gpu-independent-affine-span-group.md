# CR: Independent Affine Span Group GPU Command

## Problem

The previous generic affine grouped span command was correct, but too restrictive for software renderers that already generate exact spans.

The old fixed/shared affine payload shared these fields across all lanes:

- `tex_addr`
- `sstep`
- `tstep`
- texture format/masks
- flags/colormap

That made it poor for Doom wall columns and similar renderers. Adjacent columns usually share framebuffer layout and texture format, but each lane often has a different texture column pointer and a different vertical step. The result was many 1-lane group commands. A 1-lane fixed affine group was much more command-heavy than the retired Doom-specific grouped path, so rendering regressed even though the pixel work was still GPU-accelerated.

## Goal

Use a general-purpose native grouped affine span command for 1-4 independent spans that share only the true common state, while allowing per-lane texture address and stepping.

This is not Doom-specific. It benefits any CPU span renderer that computes exact spans and wants the GPU to act as a fast fragment pipe.

The desired shape is:

1. Native independent affine span group with per-lane `tex_addr`, `s`, `t`, `sstep`, `tstep`, `light`, and `count`.
2. Shared `colormap_id` for the first implementation, with renderer fallback/split when slots differ.
3. SDK helper accepting up to 8 lanes and emitting one or two native 4-lane commands.
4. Use the same primitive for vertical columns and horizontal rows; do not add a separate floor/ceiling command first.

## Command

The existing affine-group opcode is now independent:

```c
GPU_CMD_DRAW_AFFINE_SPAN_GROUP // 0x47
```

Native hardware lane count: 1-4.

SDK may expose 1-8 lanes and split to native 4-lane chunks, matching `of_gpu_draw_persp_span_group()`.

## SDK API

```c
typedef struct {
    uint8_t  lane_count;      // 1..8, SDK splits to native 4-lane chunks
    uint8_t  flags;           // OF_GPU_SPAN_* shared by all lanes
    uint8_t  colormap_id;     // shared palookup/colormap slot
    uint8_t  reserved;

    uint16_t tex_width;
    uint16_t tex_w_mask;
    uint16_t tex_h_mask;

    int32_t  fb_step;         // byte step per pixel inside each span

    uint32_t fb_addr[8];
    uint32_t tex_addr[8];
    uint16_t count[8];
    int32_t  s[8];
    int32_t  t[8];
    int32_t  sstep[8];
    int32_t  tstep[8];
    uint8_t  light[8];
} of_gpu_affine_span_group_t;

void of_gpu_draw_affine_span_group(const of_gpu_affine_span_group_t *group);
```

The public helper accepts `lane_count` 1-8. Hardware remains 4-lane; the SDK splits 5-8 lane submissions into two native commands. This matches the existing `of_gpu_draw_persp_span_group()` pattern and keeps Doom/Duke call sites simple.

## Native Payload

For 4 native lanes:

```text
word 0:  lane_count[31:28] flags[27:20] reserved[19:16] colormap_id[3:0]
word 1:  tex_width
word 2:  tex_h_mask[31:16] tex_w_mask[15:0]
word 3:  fb_step

per lane, 7 words:
  fb_addr
  tex_addr
  count/light
  s
  t
  sstep
  tstep
```

`count` and `light` are packed:

```text
lane word 2: count[15:0] light[21:16]
4 common words + 4 lanes * 7 words = 32 words
```

## Semantics

For lane `i`:

```text
fb  = fb_addr[i]
tex = tex_addr[i]
s   = s[i]
t   = t[i]

repeat count[i]:
    texel = sample(tex, s, t, tex_width, tex_w_mask, tex_h_mask)
    pixel = apply flags/colormap/light
    write fb
    fb += fb_step
    s  += sstep[i]
    t  += tstep[i]
```

`OF_GPU_SPAN_PERSP` is not supported by this command. Perspective spans should continue using `of_gpu_draw_persp_span_group()`.

The same command handles both major span orientations:

Vertical columns:

```text
fb_step = framebuffer_stride
```

Horizontal rows:

```text
fb_step = +1 or -1
```

There is no need for a separate floor/ceiling primitive in the first version.

## Main Use Cases

- Doom wall columns: adjacent columns with different `tex_addr` and `tstep`.
- Doom clipped two-sided wall fragments: uneven counts, different starts, same draw order.
- Doom floors/ceilings: horizontal row spans with per-row `fb_addr`, `s/t`, and `sstep/tstep`.
- Doom fuzz/translucent posts when they are affine and order-sensitive.
- Duke3D floors/walls/sprites: exact software spans with different starts and steps.
- Quake software-style spans: CPU computes spans, GPU executes fragment work.

## Colormap Slot Policy

Keep `colormap_id` shared in the first implementation.

The per-lane `light` field already handles the common shade-row variation that Doom and Duke need. In Doom, wall columns generally use the same uploaded colormap slot and vary only the light row. Making `colormap_id` per-lane would increase payload/decoder complexity for little expected gain.

Renderer batching should therefore require matching `colormap_id`. If adjacent spans use different slots, flush the current group and start a new one. A 1-lane independent group is the scalar-equivalent fallback.

Per-lane `colormap_id` can remain a future extension if real workloads show that slot changes are a major batching limiter.

## Doom Expected Win

Before this command, migrated Doom often became:

```text
1 affine group command -> 1 useful wall column
```

With this command, Doom can return to:

```text
1 independent affine group command -> up to 4 useful wall columns
```

That reduces:

- CPU command setup
- ring/DMA traffic
- decoder overhead
- command stalls in fragmented geometry scenes

It should especially help the worst Doom cases: animated-texture stair/step scenes, many clipped two-sided walls, and star-shaped high-geometry rooms.

Recommended Doom integration order:

1. Use this for wall columns and clipped wall fragments first.
2. Use this for floor/ceiling row spans next.
3. Keep perspective span groups disabled for Doom walls until the previous wall corruption issue is proven fixed in hardware.

## RTL Notes

- Reuse the existing affine fragment pipeline.
- No edge setup, no triangle setup, no surface generation.
- Command decode should load up to 4 lane descriptors.
- Existing per-lane live/count handling can mirror the perspective span group command.
- Texture format/masks remain shared to keep the command general but compact.
- `tex_addr`, `sstep`, and `tstep` must be per-lane.
- `colormap_id` is shared; batchers must split when it differs.

## SDK Notes

- Add a helper that accepts 1-8 lanes and emits one or two native commands.
- Validate `lane_count != 0`.
- Drop lanes with `count == 0` or let RTL ignore them consistently.
- Flush/fallback when `colormap_id` differs between candidate lanes.
- The same API should be used for horizontal rows and vertical columns by changing only `fb_step`.

## Validation

1. Doom byte/visual validation:
   - solid walls
   - clipped two-sided upper/lower walls
   - masked posts/sprites if routed through this path
   - fuzz/translucency ordering

2. Stress scenes:
   - animated texture stair/step scenes
   - many short wall fragments
   - large open rooms with many subsectors

3. Performance counters:
   - commands per frame
   - lanes per command
   - ring wait time
   - frame prepare time

## Non-Goals

- No new surface/edge/triangle path.
- No perspective math in this command.
- No Doom-specific texture assumptions.
- No replacement for `of_gpu_draw_persp_span_group()`.
