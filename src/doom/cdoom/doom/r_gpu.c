//
// Doom renderer hooks for the openfpgaOS span GPU.
//
// The SDK GPU header owns static mutable ring state, so this is the only
// Doom translation unit that includes of_gpu.h.
//

#include "config.h"

#include "doomdef.h"
#include "i_video.h"
#include "m_argv.h"
#include "r_gpu.h"
#include "r_local.h"
#include "r_perf.h"
#include "r_state.h"
#include "w_wad.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef OF_PC

int r_gpu_enabled = 0;

void R_GPU_Init(void) { }
void R_GPU_Shutdown(void) { }
void R_GPU_BeginDisplayFrame(void) { }
void R_GPU_BeginFrame(void) { }
void R_GPU_EndFrame(void) { }
void R_GPU_PrepareForCPUAccess(void) { }
void R_GPU_PrepareForCPUAccessRect(int x, int y, int w, int h)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}
void R_GPU_TextureDataUpdated(void *ptr, unsigned int size)
{
    (void)ptr;
    (void)size;
}
boolean R_GPU_PresentFrame(void) { return false; }
boolean R_GPU_UsingDirectFramebuffer(void) { return false; }
int R_GPU_CurrentDrawSlot(void) { return -1; }
boolean R_GPU_DrawColumn(void) { return false; }
boolean R_GPU_DrawColumnDirect(int x, int yl, int yh, const byte *source,
                               int texturemid, int iscale,
                               const byte *colormap)
{
    (void)x;
    (void)yl;
    (void)yh;
    (void)source;
    (void)texturemid;
    (void)iscale;
    (void)colormap;
    return false;
}
boolean R_GPU_CanDrawFuzz(void) { return false; }
boolean R_GPU_BeginFuzzSpans(void) { return false; }
void R_GPU_EndFuzzSpans(void) { }
boolean R_GPU_DrawFuzzColumnDirect(int x, int yl, int yh)
{
    (void)x;
    (void)yl;
    (void)yh;
    return false;
}
int R_GPU_ColormapRow(const byte *map)
{
    (void)map;
    return -1;
}
boolean R_GPU_DrawColumnLightDirect(int x, int yl, int yh, const byte *source,
                                    int texturemid, int iscale, int light)
{
    (void)x;
    (void)yl;
    (void)yh;
    (void)source;
    (void)texturemid;
    (void)iscale;
    (void)light;
    return false;
}
boolean R_GPU_DrawColumnLightBatchDirect(int x, int yl, int yh, int lanes,
                                         const byte *const *source,
                                         const int32_t *t,
                                         const int32_t *tstep,
                                         const uint8_t *light)
{
    (void)x;
    (void)yl;
    (void)yh;
    (void)lanes;
    (void)source;
    (void)t;
    (void)tstep;
    (void)light;
    return false;
}
boolean R_GPU_DrawColumnLightVarBatchDirect(int x, int lanes,
                                            const int *yl,
                                            const int *yh,
                                            const byte *const *source,
                                            const int32_t *t,
                                            const int32_t *tstep,
                                            const uint8_t *light)
{
    (void)x;
    (void)lanes;
    (void)yl;
    (void)yh;
    (void)source;
    (void)t;
    (void)tstep;
    (void)light;
    return false;
}
boolean R_GPU_DrawSpan(void) { return false; }
boolean R_GPU_DrawSpanDirect(int y, int x1, int x2, const byte *source,
                             fixed_t xfrac, fixed_t yfrac,
                             fixed_t xstep, fixed_t ystep,
                             const byte *colormap)
{
    (void)y;
    (void)x1;
    (void)x2;
    (void)source;
    (void)xfrac;
    (void)yfrac;
    (void)xstep;
    (void)ystep;
    (void)colormap;
    return false;
}
boolean R_GPU_DrawSpanLightDirect(int y, int x1, int x2, const byte *source,
                                  fixed_t xfrac, fixed_t yfrac,
                                  fixed_t xstep, fixed_t ystep, int light)
{
    (void)y;
    (void)x1;
    (void)x2;
    (void)source;
    (void)xfrac;
    (void)yfrac;
    (void)xstep;
    (void)ystep;
    (void)light;
    return false;
}
boolean R_GPU_DeferLumpRelease(int lumpnum)
{
    (void)lumpnum;
    return false;
}

#else

#include "of_cache.h"
#include "of_caps.h"
#include "of_gpu.h"
#include "of_video.h"
#include "v_video.h"

int r_gpu_enabled = 1;

static int gpu_present;
static int gpu_frame_active;
static int gpu_pending;
static int gpu_cpu_dirty;
static int gpu_colormap_rows;
static int gpu_flip_enabled;
static int gpu_draw_idx;
static int gpu_acquire_pending;
static int gpu_acquire_idx;
static uint32_t gpu_acquire_token;
static int gpu_acquire_had_work;
static int gpu_display_frame_active;
static int gpu_framebuffer_cpu_ready;
static int gpu_fuzz_transluc_enabled;
static int gpu_fuzz_batch_active;
static int gpu_plane_spans_enabled;
static uint8_t *gpu_draw_fb;
static uint8_t *gpu_draw_render_base;
static uintptr_t gpu_framebuffer_delta;

#define GPU_DEFERRED_LUMPS 64
#define GPU_COLUMN_BATCH_LANES 8
#define GPU_SPAN_BATCH_SPANS 96
#define GPU_LETTERBOX_Y ((OF_SCREEN_H - SCREENHEIGHT) / 2)
#define GPU_FB_CACHE_LINE_BYTES 64u
#define GPU_FB_CACHE_LINES \
    ((SCREENWIDTH * SCREENHEIGHT + GPU_FB_CACHE_LINE_BYTES - 1u) \
     / GPU_FB_CACHE_LINE_BYTES)
#define GPU_FB_CACHE_WORDS ((GPU_FB_CACHE_LINES + 31u) / 32u)
static int gpu_deferred_lumps[GPU_DEFERRED_LUMPS];
static int gpu_deferred_lump_count;
static of_gpu_span_group_t gpu_column_batch;
static int gpu_column_batch_count;
static of_gpu_span_group_varcount_t gpu_column_var_batch;
static int gpu_column_var_batch_count;
static int gpu_column_var_batch_base_x;
static int gpu_column_var_batch_base_y;
static of_gpu_span_t gpu_span_batch[GPU_SPAN_BATCH_SPANS];
static int gpu_span_batch_count;
static uint32_t gpu_cpu_dirty_lines[GPU_FB_CACHE_WORDS];
static uint32_t gpu_cpu_valid_lines[GPU_FB_CACHE_WORDS];
static uint8_t gpu_fuzz_source_tex[4] = { 0x80, 0x80, 0x80, 0x80 };
static uint8_t gpu_fuzz_transluc_table[256 * 256];

static void gpu_flush_column_batch(void);
static void gpu_flush_column_var_batch(void);
static void gpu_flush_column_batches(void);
static void gpu_flush_span_batch(void);
static void gpu_flush_draw_batches(void);
static void gpu_release_deferred_lumps(void);
static uint32_t gpu_framebuffer_addr(pixel_t *ptr);

static int gpu_has_pending_draw_batches(void)
{
    return gpu_column_batch_count != 0
        || gpu_column_var_batch_count != 0
        || gpu_span_batch_count != 0;
}

static void gpu_line_set(uint32_t *bits, unsigned int line)
{
    bits[line >> 5] |= 1u << (line & 31u);
}

static int gpu_line_test(const uint32_t *bits, unsigned int line)
{
    return (bits[line >> 5] & (1u << (line & 31u))) != 0;
}

static void gpu_clear_line_bits(uint32_t *bits)
{
    memset(bits, 0, sizeof(gpu_cpu_dirty_lines));
}

static void gpu_reset_cpu_cache_tracking(void)
{
    gpu_clear_line_bits(gpu_cpu_dirty_lines);
    gpu_clear_line_bits(gpu_cpu_valid_lines);
    gpu_cpu_dirty = 0;
}

static int gpu_clip_rect(int *x, int *y, int *w, int *h)
{
    int x2;
    int y2;

    if (*w <= 0 || *h <= 0)
        return 0;

    x2 = *x + *w;
    y2 = *y + *h;

    if (*x < 0)
        *x = 0;
    if (*y < 0)
        *y = 0;
    if (x2 > SCREENWIDTH)
        x2 = SCREENWIDTH;
    if (y2 > SCREENHEIGHT)
        y2 = SCREENHEIGHT;

    *w = x2 - *x;
    *h = y2 - *y;

    return *w > 0 && *h > 0;
}

static void gpu_flush_line_run(unsigned int first, unsigned int last)
{
    uint8_t *ptr;
    uint32_t size;

    if (gpu_draw_render_base == NULL || first > last)
        return;

    ptr = gpu_draw_render_base + first * GPU_FB_CACHE_LINE_BYTES;
    size = (uint32_t)(last - first + 1u) * GPU_FB_CACHE_LINE_BYTES;
    of_cache_flush_range(ptr, size);
}

static void gpu_inval_line_run(unsigned int first, unsigned int last)
{
    uint8_t *ptr;
    uint32_t size;

    if (gpu_draw_render_base == NULL || first > last)
        return;

    ptr = gpu_draw_render_base + first * GPU_FB_CACHE_LINE_BYTES;
    size = (uint32_t)(last - first + 1u) * GPU_FB_CACHE_LINE_BYTES;
    of_cache_inval_range(ptr, size);
}

static void gpu_flush_cpu_dirty_lines(void)
{
    unsigned int cache_start;
    unsigned int line = 0;

    if (!gpu_cpu_dirty || gpu_draw_render_base == NULL)
        return;

    cache_start = R_Perf_BeginStage();

    while (line < GPU_FB_CACHE_LINES)
    {
        unsigned int first;

        while (line < GPU_FB_CACHE_LINES &&
               !gpu_line_test(gpu_cpu_dirty_lines, line))
            line++;

        if (line >= GPU_FB_CACHE_LINES)
            break;

        first = line;
        while (line < GPU_FB_CACHE_LINES &&
               gpu_line_test(gpu_cpu_dirty_lines, line))
            line++;

        gpu_flush_line_run(first, line - 1u);
    }

    R_Perf_EndStage(R_PERF_STAGE_CACHE, cache_start);
    gpu_clear_line_bits(gpu_cpu_dirty_lines);
    gpu_cpu_dirty = 0;
}

static void gpu_invalidate_rect_for_cpu(int x, int y, int w, int h)
{
    unsigned int cache_start;

    if (gpu_draw_render_base == NULL)
        return;
    if (!gpu_clip_rect(&x, &y, &w, &h))
        return;

    cache_start = R_Perf_BeginStage();

    for (int row = y; row < y + h; row++)
    {
        unsigned int row_off = (unsigned int)row * SCREENWIDTH;
        unsigned int first = (row_off + (unsigned int)x)
                           / GPU_FB_CACHE_LINE_BYTES;
        unsigned int last = (row_off + (unsigned int)x + (unsigned int)w - 1u)
                          / GPU_FB_CACHE_LINE_BYTES;
        unsigned int line = first;

        while (line <= last)
        {
            unsigned int run_first;

            while (line <= last &&
                   (gpu_line_test(gpu_cpu_dirty_lines, line) ||
                    gpu_line_test(gpu_cpu_valid_lines, line)))
                line++;

            if (line > last)
                break;

            run_first = line;
            while (line <= last &&
                   !gpu_line_test(gpu_cpu_dirty_lines, line) &&
                   !gpu_line_test(gpu_cpu_valid_lines, line))
            {
                gpu_line_set(gpu_cpu_valid_lines, line);
                line++;
            }

            gpu_inval_line_run(run_first, line - 1u);
        }
    }

    R_Perf_EndStage(R_PERF_STAGE_CACHE, cache_start);
}

static void gpu_mark_cpu_dirty_rect(int x, int y, int w, int h)
{
    if (!gpu_clip_rect(&x, &y, &w, &h))
        return;

    for (int row = y; row < y + h; row++)
    {
        unsigned int row_off = (unsigned int)row * SCREENWIDTH;
        unsigned int first = (row_off + (unsigned int)x)
                           / GPU_FB_CACHE_LINE_BYTES;
        unsigned int last = (row_off + (unsigned int)x + (unsigned int)w - 1u)
                          / GPU_FB_CACHE_LINE_BYTES;

        for (unsigned int line = first; line <= last; line++)
        {
            gpu_line_set(gpu_cpu_dirty_lines, line);
            gpu_line_set(gpu_cpu_valid_lines, line);
        }
    }

    gpu_cpu_dirty = 1;
}

static int gpu_rect_overlaps_view_rows(int x, int y, int w, int h)
{
    int view_top;
    int view_bottom;

    if (!gpu_clip_rect(&x, &y, &w, &h))
        return 0;

    view_top = viewwindowy;
    view_bottom = viewwindowy + viewheight;

    if (view_top < 0)
        view_top = 0;
    if (view_bottom > SCREENHEIGHT)
        view_bottom = SCREENHEIGHT;

    return y < view_bottom && y + h > view_top;
}

static int gpu_prepare_cpu_write_outside_view(int x, int y, int w, int h)
{
    if (!gpu_flip_enabled || !gpu_display_frame_active)
        return 0;

    if (gpu_rect_overlaps_view_rows(x, y, w, h))
        return 0;

    gpu_invalidate_rect_for_cpu(x, y, w, h);
    gpu_mark_cpu_dirty_rect(x, y, w, h);
    return 1;
}

static void gpu_record_debug_snapshot(void)
{
#if R_RENDER_PERF
    of_gpu_debug_snapshot_t snap;

    of_gpu_debug_snapshot(&snap, 1);
    R_Perf_AddGpuDebug(snap.dma_waits, snap.dma_spin_iters,
                       0,
                       snap.ring_waits, snap.ring_spin_iters,
                       0,
                       0, 0,
                       0, 0,
                       snap.min_ring_free, snap.ring_free,
                       snap.status);
#endif
}

static void gpu_mark_framebuffer_gpu_dirty(void)
{
    gpu_framebuffer_cpu_ready = 0;
}

static int gpu_prepare_framebuffer_for_cpu(void)
{
    unsigned int wait_start;

    gpu_flush_draw_batches();

    if (gpu_framebuffer_cpu_ready && !gpu_pending)
        return 0;

    wait_start = R_Perf_BeginStage();
    of_gpu_prepare_framebuffer_for_cpu();
    R_Perf_EndStage(R_PERF_STAGE_GPU_WAIT, wait_start);
    R_Perf_CountGpuFinish();
    gpu_record_debug_snapshot();
    gpu_pending = 0;
    gpu_framebuffer_cpu_ready = 1;
    gpu_release_deferred_lumps();
    return 1;
}

static void gpu_clear_pending_acquire(void)
{
    gpu_acquire_pending = 0;
    gpu_acquire_idx = -1;
    gpu_acquire_token = 0;
    gpu_acquire_had_work = 0;
}

static int gpu_acquire_draw_buffer(void)
{
    unsigned int wait_start;

    if (gpu_draw_idx >= 0)
        return 1;
    if (!gpu_acquire_pending)
        return 0;

    wait_start = R_Perf_BeginStage();
    gpu_draw_idx = of_video_acquire_next(gpu_acquire_idx,
                                         gpu_acquire_token);
    R_Perf_EndStage(R_PERF_STAGE_GPU_WAIT, wait_start);

    if (gpu_acquire_had_work)
        R_Perf_CountGpuFinish();
    gpu_record_debug_snapshot();
    gpu_pending = 0;
    gpu_framebuffer_cpu_ready = 1;
    gpu_release_deferred_lumps();
    gpu_clear_pending_acquire();

    if (gpu_draw_idx < 0)
    {
        gpu_flip_enabled = 0;
        return 0;
    }

    return 1;
}

static void gpu_release_deferred_lumps(void)
{
    for (int i = 0; i < gpu_deferred_lump_count; i++)
        W_ReleaseLumpNum(gpu_deferred_lumps[i]);
    gpu_deferred_lump_count = 0;
}

static void gpu_emit_batched_column_lane(int lane)
{
    _gpu_cmd_header(GPU_CMD_DRAW_SPAN_GROUP, 9);
    _gpu_ring_write(gpu_column_batch.fb_addr
                    + (uint32_t)((int32_t)gpu_column_batch.lane_delta * lane));
    _gpu_ring_write(gpu_column_batch.tex_addr[lane]);
    _gpu_ring_write(0);
    _gpu_ring_write((uint32_t)gpu_column_batch.t[lane]);
    _gpu_ring_write(0);
    _gpu_ring_write((uint32_t)gpu_column_batch.tstep[lane]);
    _gpu_ring_write((((uint32_t)gpu_column_batch.colormap_id & 0xFu) << 28) |
                    (((uint32_t)gpu_column_batch.count & 0x0FFFu) << 16) |
                    (((uint32_t)gpu_column_batch.light[lane] & 0x3Fu) << 8) |
                    ((uint32_t)gpu_column_batch.flags));
    _gpu_ring_write(((uint32_t)(uint16_t)gpu_column_batch.fb_stride << 16) |
                    (uint32_t)gpu_column_batch.tex_width);
    _gpu_ring_write(((uint32_t)gpu_column_batch.tex_h_mask << 16) |
                    (uint32_t)gpu_column_batch.tex_w_mask);
}

static void gpu_emit_varcount_scalar_lane(int lane)
{
    _gpu_cmd_header(GPU_CMD_DRAW_SPAN_GROUP, 9);
    _gpu_ring_write(gpu_column_var_batch.fb_addr
                    + (uint32_t)((int32_t)gpu_column_var_batch.lane_delta
                                 * lane)
                    + (uint32_t)((int32_t)gpu_column_var_batch.fb_stride
                                 * (int32_t)gpu_column_var_batch.y_start[lane]));
    _gpu_ring_write(gpu_column_var_batch.tex_addr[lane]);
    _gpu_ring_write(0);
    _gpu_ring_write((uint32_t)gpu_column_var_batch.t[lane]);
    _gpu_ring_write(0);
    _gpu_ring_write((uint32_t)gpu_column_var_batch.tstep[lane]);
    _gpu_ring_write((((uint32_t)gpu_column_var_batch.colormap_id & 0xFu) << 28) |
                    (((uint32_t)gpu_column_var_batch.count[lane] & 0x0FFFu) << 16) |
                    (((uint32_t)gpu_column_var_batch.light[lane] & 0x3Fu) << 8) |
                    ((uint32_t)gpu_column_var_batch.flags));
    _gpu_ring_write(((uint32_t)(uint16_t)gpu_column_var_batch.fb_stride << 16) |
                    (uint32_t)gpu_column_var_batch.tex_width);
    _gpu_ring_write(((uint32_t)gpu_column_var_batch.tex_h_mask << 16) |
                    (uint32_t)gpu_column_var_batch.tex_w_mask);
}

static void gpu_emit_column_group_chunk(int first_lane, int lanes)
{
    uint32_t words[OF_GPU_SPAN_GROUP_WORDS];

    _gpu_encode_span_group_chunk(words, &gpu_column_batch,
                                 (uint32_t)first_lane,
                                 (uint32_t)lanes);
    _gpu_cmd_header(GPU_CMD_DRAW_SPAN_GROUP, OF_GPU_SPAN_GROUP_WORDS);
    for (uint32_t i = 0; i < OF_GPU_SPAN_GROUP_WORDS; i++)
        _gpu_ring_write(words[i]);
}

static void gpu_emit_varcount_group_chunk(int first_lane, int lanes)
{
    uint32_t words[OF_GPU_SPAN_GROUP_VARCOUNT_WORDS];
    uint16_t row_count;

    row_count = _gpu_encode_span_group_varcount_chunk(
        words, &gpu_column_var_batch, (uint32_t)first_lane,
        (uint32_t)lanes);
    if (row_count == 0)
        return;

    _gpu_cmd_header(GPU_CMD_DRAW_SPAN_GROUP,
                    OF_GPU_SPAN_GROUP_VARCOUNT_WORDS);
    for (uint32_t i = 0; i < OF_GPU_SPAN_GROUP_VARCOUNT_WORDS; i++)
        _gpu_ring_write(words[i]);
}

static void gpu_flush_column_batch(void)
{
    int lanes = gpu_column_batch_count;
    int first = 0;
    int left;

    if (lanes <= 0)
        return;

    gpu_column_batch.lane_count = (uint8_t)lanes;

    left = lanes;
    while (left >= 4)
    {
        gpu_emit_column_group_chunk(first, 4);
        first += 4;
        left -= 4;
    }

    if (left == 3)
    {
        gpu_emit_column_group_chunk(first, 2);
        gpu_emit_batched_column_lane(first + 2);
    }
    else if (left == 2)
    {
        gpu_emit_column_group_chunk(first, 2);
    }
    else if (left == 1)
    {
        gpu_emit_batched_column_lane(first);
    }

    R_Perf_CountGpuColumnBatch((unsigned int)lanes);
    gpu_column_batch_count = 0;
    gpu_pending = 1;
    gpu_mark_framebuffer_gpu_dirty();
}

static void gpu_flush_column_var_batch(void)
{
    int lanes = gpu_column_var_batch_count;
    int first = 0;
    int left;

    if (lanes <= 0)
        return;

    gpu_column_var_batch.lane_count = (uint8_t)lanes;

    left = lanes;
    while (left >= 4)
    {
        gpu_emit_varcount_group_chunk(first, 4);
        first += 4;
        left -= 4;
    }

    if (left == 3)
    {
        gpu_emit_varcount_group_chunk(first, 2);
        gpu_emit_varcount_scalar_lane(first + 2);
    }
    else if (left == 2)
    {
        gpu_emit_varcount_scalar_lane(first);
        gpu_emit_varcount_scalar_lane(first + 1);
    }
    else if (left == 1)
    {
        gpu_emit_varcount_scalar_lane(first);
    }

    R_Perf_CountGpuColumnBatch((unsigned int)lanes);
    gpu_column_var_batch_count = 0;
    gpu_pending = 1;
    gpu_mark_framebuffer_gpu_dirty();
}

static void gpu_flush_column_batches(void)
{
    gpu_flush_column_batch();
    gpu_flush_column_var_batch();
}

static void gpu_flush_span_batch(void)
{
    int count = gpu_span_batch_count;

    if (count <= 0)
        return;

    for (int i = 0; i < count; i++)
        of_gpu_draw_span(&gpu_span_batch[i]);

    gpu_span_batch_count = 0;
    gpu_pending = 1;
    gpu_mark_framebuffer_gpu_dirty();
}

static void gpu_flush_draw_batches(void)
{
    gpu_flush_column_batches();
    gpu_flush_span_batch();
}

static int gpu_can_batch_column(uint32_t fb_addr, uint16_t count,
                                uint8_t flags, uint8_t colormap_id,
                                int16_t fb_stride, int16_t lane_delta,
                                uint16_t tex_width, uint16_t tex_w_mask,
                                uint16_t tex_h_mask)
{
    uint32_t expected_fb;

    if (gpu_column_batch_count <= 0)
        return 1;
    if (gpu_column_batch_count >= GPU_COLUMN_BATCH_LANES)
        return 0;
    if (gpu_column_batch.count != count)
        return 0;
    if (gpu_column_batch.flags != flags ||
        gpu_column_batch.colormap_id != colormap_id ||
        gpu_column_batch.fb_stride != fb_stride ||
        gpu_column_batch.lane_delta != lane_delta ||
        gpu_column_batch.tex_width != tex_width ||
        gpu_column_batch.tex_w_mask != tex_w_mask ||
        gpu_column_batch.tex_h_mask != tex_h_mask)
        return 0;

    expected_fb = gpu_column_batch.fb_addr
                + (uint32_t)((int32_t)lane_delta * gpu_column_batch_count);
    return fb_addr == expected_fb;
}

static int gpu_can_batch_varcount_column(int x, int yl, int count,
                                         uint8_t flags, uint8_t colormap_id,
                                         int16_t fb_stride, int16_t lane_delta,
                                         uint16_t tex_width,
                                         uint16_t tex_w_mask,
                                         uint16_t tex_h_mask)
{
    if (gpu_column_var_batch_count <= 0)
        return 1;
    if (gpu_column_var_batch_count >= GPU_COLUMN_BATCH_LANES)
        return 0;
    if (x != gpu_column_var_batch_base_x + gpu_column_var_batch_count)
        return 0;
    if (gpu_column_var_batch.flags != flags ||
        gpu_column_var_batch.colormap_id != colormap_id ||
        gpu_column_var_batch.fb_stride != fb_stride ||
        gpu_column_var_batch.lane_delta != lane_delta ||
        gpu_column_var_batch.tex_width != tex_width ||
        gpu_column_var_batch.tex_w_mask != tex_w_mask ||
        gpu_column_var_batch.tex_h_mask != tex_h_mask)
        return 0;

    if (yl >= gpu_column_var_batch_base_y)
        return (yl - gpu_column_var_batch_base_y + count) <= 0xffff;

    return (gpu_column_var_batch_base_y - yl + SCREENHEIGHT) <= 0xffff;
}

static void gpu_add_varcount_column(int x, int yl, int count,
                                    const byte *source, int32_t t,
                                    int32_t tstep, uint8_t light,
                                    uint8_t flags, uint8_t colormap_id,
                                    int16_t fb_stride, int16_t lane_delta,
                                    uint16_t tex_width, uint16_t tex_w_mask,
                                    uint16_t tex_h_mask)
{
    int lane;

    gpu_flush_column_batch();

    if (!gpu_can_batch_varcount_column(x, yl, count, flags, colormap_id,
                                       fb_stride, lane_delta, tex_width,
                                       tex_w_mask, tex_h_mask))
        gpu_flush_column_var_batch();

    if (gpu_column_var_batch_count == 0)
    {
        gpu_column_var_batch_base_x = x;
        gpu_column_var_batch_base_y = yl;
        gpu_column_var_batch.fb_addr =
            gpu_framebuffer_addr(ylookup[yl] + columnofs[x]);
        gpu_column_var_batch.flags = flags;
        gpu_column_var_batch.colormap_id = colormap_id;
        gpu_column_var_batch.fb_stride = fb_stride;
        gpu_column_var_batch.lane_delta = lane_delta;
        gpu_column_var_batch.tex_width = tex_width;
        gpu_column_var_batch.tex_w_mask = tex_w_mask;
        gpu_column_var_batch.tex_h_mask = tex_h_mask;
    }
    else if (yl < gpu_column_var_batch_base_y)
    {
        uint16_t delta = (uint16_t)(gpu_column_var_batch_base_y - yl);

        for (int i = 0; i < gpu_column_var_batch_count; i++)
            gpu_column_var_batch.y_start[i] += delta;

        gpu_column_var_batch_base_y = yl;
        gpu_column_var_batch.fb_addr =
            gpu_framebuffer_addr(ylookup[yl] +
                                 columnofs[gpu_column_var_batch_base_x]);
    }

    lane = gpu_column_var_batch_count++;
    gpu_column_var_batch.y_start[lane] =
        (uint16_t)(yl - gpu_column_var_batch_base_y);
    gpu_column_var_batch.count[lane] = (uint16_t)count;
    gpu_column_var_batch.tex_addr[lane] = (uint32_t)(uintptr_t)source;
    gpu_column_var_batch.t[lane] = t;
    gpu_column_var_batch.tstep[lane] = tstep;
    gpu_column_var_batch.light[lane] = light;
    gpu_column_var_batch.lane_count = (uint8_t)gpu_column_var_batch_count;

    if (gpu_column_var_batch_count == GPU_COLUMN_BATCH_LANES)
        gpu_flush_column_var_batch();
}

static void gpu_finish_pending(void)
{
    unsigned int wait_start;
    int waited_for_pending_acquire;

    gpu_flush_draw_batches();

    if (!gpu_pending)
        return;

    waited_for_pending_acquire = gpu_acquire_pending;
    wait_start = R_Perf_BeginStage();
    of_gpu_finish();
    R_Perf_EndStage(R_PERF_STAGE_GPU_WAIT, wait_start);
    R_Perf_CountGpuFinish();
    gpu_record_debug_snapshot();
    gpu_pending = 0;
    if (waited_for_pending_acquire)
        gpu_acquire_had_work = 0;

    /* GPU writes bypass the CPU cache.  Flush+invalidate gives any dirty
     * CPU-only lines a chance to land and drops stale lines for GPU-written
     * pixels before the CPU renders overlays or copies the screen out. */
    if (I_VideoBuffer != NULL && !gpu_flip_enabled)
    {
        unsigned int cache_start = R_Perf_BeginStage();
        of_cache_flush_range(I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
        R_Perf_EndStage(R_PERF_STAGE_CACHE, cache_start);
    }

    gpu_release_deferred_lumps();
}

static void gpu_prepare_for_gpu_write(void)
{
    if (!gpu_frame_active || I_VideoBuffer == NULL)
        return;

    if (gpu_cpu_dirty)
    {
        if (gpu_flip_enabled)
        {
            gpu_flush_cpu_dirty_lines();
            gpu_clear_line_bits(gpu_cpu_valid_lines);
        }
        else
        {
            unsigned int cache_start = R_Perf_BeginStage();
            of_cache_flush_range(I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
            R_Perf_EndStage(R_PERF_STAGE_CACHE, cache_start);
            gpu_cpu_dirty = 0;
        }
    }
}

static int gpu_colormap_row(const lighttable_t *map)
{
    if (map == NULL || colormaps == NULL || gpu_colormap_rows <= 0)
        return -1;

    uintptr_t base = (uintptr_t)colormaps;
    uintptr_t p = (uintptr_t)map;
    uintptr_t end = base + (uintptr_t)gpu_colormap_rows * 256u;

    if (p < base || p >= end)
        return -1;

    uintptr_t off = p - base;
    if ((off & 0xffu) != 0)
        return -1;

    return (int)(off >> 8);
}

int R_GPU_ColormapRow(const byte *map)
{
    return gpu_colormap_row((const lighttable_t *)map);
}

static void gpu_upload_fuzz_translucency(void)
{
    const byte *darkmap;
    int row;

    if (colormaps == NULL || gpu_colormap_rows <= 0)
        return;

    row = gpu_colormap_rows > 6 ? 6 : 0;
    darkmap = colormaps + row * 256;

    for (int src = 0; src < 256; src++)
        memcpy(gpu_fuzz_transluc_table + src * 256, darkmap, 256);

    of_gpu_translucency_upload(gpu_fuzz_transluc_table,
                               sizeof(gpu_fuzz_transluc_table));
}

static uint32_t gpu_framebuffer_addr(pixel_t *ptr)
{
    if (gpu_flip_enabled && gpu_draw_render_base != NULL)
        return (uint32_t)((uintptr_t)ptr + gpu_framebuffer_delta);

    return (uint32_t)(uintptr_t)ptr;
}

static int32_t gpu_column_t_start_direct(int yl, int texturemid, int iscale)
{
    uint32_t y = (uint32_t)(yl - centery);
    return (int32_t)((uint32_t)texturemid + y * (uint32_t)iscale);
}

void R_GPU_Init(void)
{
    gpu_present = 0;
    gpu_frame_active = 0;
    gpu_pending = 0;
    gpu_cpu_dirty = 0;
    gpu_colormap_rows = 0;
    gpu_fuzz_transluc_enabled = 0;
    gpu_fuzz_batch_active = 0;
    gpu_plane_spans_enabled = 1;
    gpu_flip_enabled = 0;
    gpu_draw_idx = -1;
    gpu_clear_pending_acquire();
    gpu_display_frame_active = 0;
    gpu_framebuffer_cpu_ready = 1;
    gpu_draw_fb = NULL;
    gpu_draw_render_base = NULL;
    gpu_framebuffer_delta = 0;
    gpu_deferred_lump_count = 0;
    gpu_column_batch_count = 0;
    gpu_column_var_batch_count = 0;
    gpu_span_batch_count = 0;
    gpu_reset_cpu_cache_tracking();

    if (!r_gpu_enabled || M_CheckParm("-nogpu") > 0)
        return;

    const struct of_capabilities *caps = of_get_caps();
    if (caps == NULL || caps->gpu_base == 0 ||
        (caps->hw_features & OF_HW_GPU_SPAN) == 0)
    {
        return;
    }

    of_gpu_init();

    lumpindex_t lump = W_GetNumForName("COLORMAP");
    int cmap_size = W_LumpLength(lump);
    if (cmap_size > 64 * 256)
        cmap_size = 64 * 256;

    gpu_colormap_rows = cmap_size / 256;

    /* Make all already-loaded WAD/cache data visible to the GPU, then copy
     * Doom's palette remap rows into the fabric palookup table. */
    of_cache_flush();
    of_gpu_palookup_upload(0, colormaps, (uint32_t)cmap_size);
    /* The current span GPU exposes fuzz through OF_GPU_SPAN_TRANSLUC.
     * Some firmware builds do not advertise the older OF_HW_GPU_ALPHA
     * capability bit even though span translucency is wired, so do not gate
     * Doom's spectre path on that stale bit. */
    gpu_fuzz_transluc_enabled = M_CheckParm("-nogpufuzz") <= 0;
    if (gpu_fuzz_transluc_enabled)
        gpu_upload_fuzz_translucency();
    of_cache_flush_range(gpu_fuzz_source_tex, sizeof(gpu_fuzz_source_tex));
    GPU_TEX_FLUSH = 1;

    gpu_present = 1;
    gpu_plane_spans_enabled = M_CheckParm("-nogpuplanes") <= 0;

    if (M_CheckParm("-nogpuflip") <= 0)
    {
        gpu_draw_idx = of_video_acquire_next(-1, 0);
        gpu_flip_enabled = gpu_draw_idx >= 0;
    }

}

void R_GPU_Shutdown(void)
{
    if (!gpu_present)
        return;

    if (gpu_flip_enabled && gpu_acquire_pending)
        gpu_acquire_draw_buffer();
    if (gpu_flip_enabled)
        of_video_wait_flip();
    gpu_finish_pending();
    of_gpu_shutdown();
    gpu_flip_enabled = 0;
    gpu_draw_idx = -1;
    gpu_clear_pending_acquire();
    gpu_display_frame_active = 0;
    gpu_framebuffer_cpu_ready = 1;
    gpu_draw_fb = NULL;
    gpu_draw_render_base = NULL;
    gpu_framebuffer_delta = 0;
    gpu_column_batch_count = 0;
    gpu_column_var_batch_count = 0;
    gpu_span_batch_count = 0;
    gpu_reset_cpu_cache_tracking();
    gpu_present = 0;
}

boolean R_GPU_UsingDirectFramebuffer(void)
{
    return gpu_present && gpu_flip_enabled;
}

int R_GPU_CurrentDrawSlot(void)
{
    if (!gpu_present || !gpu_flip_enabled || !gpu_display_frame_active)
        return -1;

    return gpu_draw_idx;
}

void R_GPU_BeginDisplayFrame(void)
{
    if (!gpu_present || !gpu_flip_enabled)
        return;
    if (gpu_display_frame_active)
        return;
    if (!gpu_acquire_draw_buffer())
        return;

    gpu_finish_pending();

    gpu_draw_fb = of_video_buffer_addr(gpu_draw_idx);
    if (gpu_draw_fb == NULL)
    {
        gpu_flip_enabled = 0;
        return;
    }

    gpu_draw_render_base = gpu_draw_fb + GPU_LETTERBOX_Y * OF_SCREEN_W;
    pixel_t *video_buffer = (pixel_t *)gpu_draw_render_base;
    gpu_framebuffer_delta = 0;
    gpu_reset_cpu_cache_tracking();

    if (I_VideoBuffer != video_buffer)
    {
        I_VideoBuffer = video_buffer;
        R_RetargetBuffer();
    }

    V_RestoreBuffer();

    gpu_display_frame_active = 1;
    gpu_framebuffer_cpu_ready = 1;

    /* All three hardware buffers are cleared during video init.  Doom only
     * renders into the centered 320x200 window, so the bars stay black and
     * do not need per-frame GPU clears. */
}

void R_GPU_BeginFrame(void)
{
    if (!gpu_present || I_VideoBuffer == NULL)
        return;
    if (gpu_flip_enabled && !gpu_display_frame_active)
        return;

    gpu_finish_pending();
    gpu_frame_active = 1;

    /* Direct-FB mode now lets CPU HUD/menu drawing use the cached alias.
     * If status/border code dirtied lines before the 3D view starts, publish
     * those lines before the GPU writes, then force later CPU overlays to
     * revalidate against GPU-produced pixels. */
    if (gpu_flip_enabled)
    {
        gpu_flush_cpu_dirty_lines();
        gpu_clear_line_bits(gpu_cpu_valid_lines);
    }
    else
    {
        unsigned int cache_start;

        gpu_cpu_dirty = 0;
        cache_start = R_Perf_BeginStage();
        of_cache_flush_range(I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
        R_Perf_EndStage(R_PERF_STAGE_CACHE, cache_start);
    }
    of_gpu_set_framebuffer((uint32_t)(uintptr_t)
                           (gpu_flip_enabled && gpu_draw_render_base != NULL
                            ? gpu_draw_render_base
                            : (uint8_t *)I_VideoBuffer),
                           SCREENWIDTH);
    gpu_framebuffer_cpu_ready = 1;
}

void R_GPU_EndFrame(void)
{
    if (!gpu_present || !gpu_frame_active)
        return;

    if (gpu_flip_enabled)
    {
        gpu_finish_pending();
    }
    else
    {
        if (gpu_prepare_framebuffer_for_cpu())
        {
            unsigned int cache_start = R_Perf_BeginStage();
            of_cache_inval_range(I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
            R_Perf_EndStage(R_PERF_STAGE_CACHE, cache_start);
        }
    }
    gpu_frame_active = 0;
    gpu_release_deferred_lumps();
}

void R_GPU_PrepareForCPUAccess(void)
{
    R_GPU_PrepareForCPUAccessRect(0, 0, SCREENWIDTH, SCREENHEIGHT);
}

void R_GPU_PrepareForCPUAccessRect(int x, int y, int w, int h)
{
    if (gpu_present && gpu_flip_enabled && gpu_acquire_pending)
        gpu_acquire_draw_buffer();

    if (gpu_present && gpu_prepare_cpu_write_outside_view(x, y, w, h))
        return;

    if (!gpu_present || !gpu_frame_active)
    {
        if (gpu_present && gpu_flip_enabled && gpu_display_frame_active)
        {
            gpu_prepare_framebuffer_for_cpu();
            gpu_invalidate_rect_for_cpu(x, y, w, h);
            gpu_mark_cpu_dirty_rect(x, y, w, h);
        }
        return;
    }

    if (gpu_cpu_dirty && gpu_framebuffer_cpu_ready &&
        !gpu_pending && !gpu_has_pending_draw_batches())
    {
        if (gpu_flip_enabled)
        {
            gpu_invalidate_rect_for_cpu(x, y, w, h);
            gpu_mark_cpu_dirty_rect(x, y, w, h);
        }
        return;
    }

    R_Perf_CountPrepareCPU();
    int waited_for_gpu = gpu_prepare_framebuffer_for_cpu();

    if (gpu_flip_enabled)
    {
        gpu_invalidate_rect_for_cpu(x, y, w, h);
        gpu_mark_cpu_dirty_rect(x, y, w, h);
    }
    else
    {
        if (waited_for_gpu)
        {
            unsigned int cache_start = R_Perf_BeginStage();
            of_cache_inval_range(I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
            R_Perf_EndStage(R_PERF_STAGE_CACHE, cache_start);
        }
        gpu_cpu_dirty = 1;
    }
}

void R_GPU_TextureDataUpdated(void *ptr, unsigned int size)
{
    unsigned int cache_start;

    if (!gpu_present || ptr == NULL || size == 0)
        return;

    /* Texture-cache invalidation is only safe while the GPU is idle.  This
     * path is cold: it runs when Doom loads a lump or builds a composite
     * texture, not for every already-cached texel fetch. */
    gpu_finish_pending();
    cache_start = R_Perf_BeginStage();
    of_cache_flush_range(ptr, size);
    R_Perf_EndStage(R_PERF_STAGE_CACHE, cache_start);
    GPU_TEX_FLUSH = 1;
}

boolean R_GPU_PresentFrame(void)
{
    unsigned int present_start;
    unsigned int stage_start;
    uint32_t token;
    int had_gpu_work;

    if (!gpu_present || !gpu_flip_enabled || I_VideoBuffer == NULL ||
        gpu_draw_idx < 0 || !gpu_display_frame_active)
        return false;

    present_start = R_Perf_BeginStage();

    /* Queue the flip and return immediately.  The next display frame will
     * acquire a safe draw buffer after this flip's fence retires, which gives
     * game/audio work room to run while the swap is pending.
     */
    gpu_flush_draw_batches();
    had_gpu_work = gpu_pending;
    gpu_flush_cpu_dirty_lines();

    stage_start = R_Perf_BeginStage();
    token = of_gpu_flip_to(gpu_draw_idx);
    of_gpu_kick();
    R_Perf_EndStage(R_PERF_STAGE_GPU_FLIP, stage_start);

    gpu_acquire_pending = 1;
    gpu_acquire_idx = gpu_draw_idx;
    gpu_acquire_token = token;
    gpu_acquire_had_work = had_gpu_work;
    gpu_draw_idx = -1;

    gpu_display_frame_active = 0;
    gpu_frame_active = 0;
    gpu_fuzz_batch_active = 0;
    gpu_draw_fb = NULL;
    gpu_draw_render_base = NULL;
    gpu_reset_cpu_cache_tracking();

    R_Perf_EndStage(R_PERF_STAGE_PRESENT, present_start);
    return true;
}

boolean R_GPU_DrawColumn(void)
{
    return R_GPU_DrawColumnDirect(dc_x, dc_yl, dc_yh, dc_source,
                                  dc_texturemid, dc_iscale,
                                  (const byte *)dc_colormap);
}

boolean R_GPU_DrawColumnDirect(int x, int yl, int yh, const byte *source,
                               int texturemid, int iscale,
                               const byte *colormap)
{
    int light = gpu_colormap_row((const lighttable_t *)colormap);
    return R_GPU_DrawColumnLightDirect(x, yl, yh, source, texturemid, iscale,
                                       light);
}

static boolean gpu_can_draw_fuzz(void)
{
    return gpu_present && gpu_frame_active && gpu_fuzz_transluc_enabled
        && I_VideoBuffer != NULL;
}

boolean R_GPU_CanDrawFuzz(void)
{
    return gpu_can_draw_fuzz();
}

boolean R_GPU_BeginFuzzSpans(void)
{
    if (!gpu_can_draw_fuzz())
        return false;

    gpu_prepare_for_gpu_write();
    gpu_flush_draw_batches();
    gpu_fuzz_batch_active = 1;
    return true;
}

void R_GPU_EndFuzzSpans(void)
{
    gpu_fuzz_batch_active = 0;
}

boolean R_GPU_DrawFuzzColumnDirect(int x, int yl, int yh)
{
    int count = yh - yl + 1;
    unsigned int submit_start = 0;
    int timing_enabled;

    if (!gpu_can_draw_fuzz())
        return false;
    if (x < 0 || x >= SCREENWIDTH || yl < 0 || yh >= SCREENHEIGHT)
        return false;
    if (count <= 0 || count > 4095)
        return false;

    timing_enabled = R_Perf_FuzzTimingEnabled();
    if (timing_enabled)
        submit_start = R_Perf_NowUS();

    if (!gpu_fuzz_batch_active)
        gpu_prepare_for_gpu_write();
    gpu_flush_span_batch();

    gpu_add_varcount_column(x, yl, count, gpu_fuzz_source_tex, 0, 0, 0,
                            OF_GPU_SPAN_TRANSLUC, 0, SCREENWIDTH, 1,
                            1, 0, 0);

    R_Perf_CountGpuColumn((unsigned int)count);
    R_Perf_CountFuzzSpan(timing_enabled ? R_Perf_NowUS() - submit_start : 0,
                         (unsigned int)count);
    return true;
}

boolean R_GPU_DrawColumnLightDirect(int x, int yl, int yh, const byte *source,
                                    int texturemid, int iscale, int light)
{
    int count = yh - yl + 1;
    if (!gpu_present || !gpu_frame_active || I_VideoBuffer == NULL)
        return false;
    if (x < 0 || x >= SCREENWIDTH || yl < 0 || yh >= SCREENHEIGHT)
        return false;
    if (count <= 0 || count > 4095 || source == NULL)
        return false;
    if (light < 0 || light > 63)
        return false;

    gpu_prepare_for_gpu_write();
    gpu_flush_span_batch();
    gpu_add_varcount_column(x, yl, count, source,
                            gpu_column_t_start_direct(yl, texturemid, iscale),
                            iscale, (uint8_t)light, OF_GPU_SPAN_COLORMAP,
                            0, SCREENWIDTH, 1, 1, 0, 127);

    R_Perf_CountGpuColumn((unsigned int)count);
    return true;
}

boolean R_GPU_DrawColumnLightBatchDirect(int x, int yl, int yh, int lanes,
                                         const byte *const *source,
                                         const int32_t *t,
                                         const int32_t *tstep,
                                         const uint8_t *light)
{
    int count = yh - yl + 1;
    uint32_t fb_addr;
    uint16_t gpu_count;

    if (!gpu_present || !gpu_frame_active || I_VideoBuffer == NULL)
        return false;
    if (lanes <= 0 || lanes > GPU_COLUMN_BATCH_LANES)
        return false;
    if (x < 0 || x + lanes > SCREENWIDTH || yl < 0 || yh >= SCREENHEIGHT)
        return false;
    if (count <= 0 || count > 4095)
        return false;

    for (int i = 0; i < lanes; i++)
    {
        if (source[i] == NULL || light[i] > 63)
            return false;
    }

    gpu_prepare_for_gpu_write();
    gpu_flush_span_batch();
    gpu_flush_column_var_batch();

    fb_addr = gpu_framebuffer_addr(ylookup[yl] + columnofs[x]);
    gpu_count = (uint16_t)count;

    for (int i = 0; i < lanes; i++)
    {
        uint32_t lane_fb_addr = fb_addr + (uint32_t)i;

        if (!gpu_can_batch_column(lane_fb_addr, gpu_count,
                                  OF_GPU_SPAN_COLORMAP,
                                  0, SCREENWIDTH, 1, 1, 0, 127))
            gpu_flush_column_batch();

        if (gpu_column_batch_count == 0)
        {
            gpu_column_batch.fb_addr = lane_fb_addr;
            gpu_column_batch.count = gpu_count;
            gpu_column_batch.flags = OF_GPU_SPAN_COLORMAP;
            gpu_column_batch.colormap_id = 0;
            gpu_column_batch.fb_stride = SCREENWIDTH;
            gpu_column_batch.lane_delta = 1;
            gpu_column_batch.tex_width = 1;
            gpu_column_batch.tex_w_mask = 0;
            gpu_column_batch.tex_h_mask = 127;
        }

        int lane = gpu_column_batch_count++;
        gpu_column_batch.tex_addr[lane] = (uint32_t)(uintptr_t)source[i];
        gpu_column_batch.t[lane] = t[i];
        gpu_column_batch.tstep[lane] = tstep[i];
        gpu_column_batch.light[lane] = light[i];
        gpu_column_batch.lane_count = (uint8_t)gpu_column_batch_count;

        if (gpu_column_batch_count == GPU_COLUMN_BATCH_LANES)
            gpu_flush_column_batch();
    }

    R_Perf_CountGpuColumns((unsigned int)lanes,
                           (unsigned int)(lanes * count));
    return true;
}

boolean R_GPU_DrawColumnLightVarBatchDirect(int x, int lanes,
                                            const int *yl,
                                            const int *yh,
                                            const byte *const *source,
                                            const int32_t *t,
                                            const int32_t *tstep,
                                            const uint8_t *light)
{
    unsigned int pixels = 0;
    int all_same_range = 1;

    if (!gpu_present || !gpu_frame_active || I_VideoBuffer == NULL)
        return false;
    if (lanes <= 0 || lanes > GPU_COLUMN_BATCH_LANES ||
        yl == NULL || yh == NULL)
        return false;
    if (x < 0 || x + lanes > SCREENWIDTH)
        return false;

    for (int i = 0; i < lanes; i++)
    {
        int count = yh[i] - yl[i] + 1;

        if (count <= 0 || count > 4095)
            return false;
        if (source[i] == NULL || light[i] > 63)
            return false;
        if (yl[i] < 0 || yh[i] >= SCREENHEIGHT)
            return false;

        if (yl[i] != yl[0] || yh[i] != yh[0])
            all_same_range = 0;
        pixels += (unsigned int)count;
    }

    if (all_same_range)
        return R_GPU_DrawColumnLightBatchDirect(x, yl[0], yh[0], lanes,
                                                source, t, tstep, light);

    gpu_prepare_for_gpu_write();
    gpu_flush_span_batch();

    for (int i = 0; i < lanes; i++)
    {
        gpu_add_varcount_column(x + i, yl[i], yh[i] - yl[i] + 1,
                                source[i], t[i], tstep[i], light[i],
                                OF_GPU_SPAN_COLORMAP, 0, SCREENWIDTH, 1,
                                1, 0, 127);
    }

    R_Perf_CountGpuColumns((unsigned int)lanes, pixels);
    return true;
}

boolean R_GPU_DrawSpan(void)
{
    return R_GPU_DrawSpanDirect(ds_y, ds_x1, ds_x2, ds_source,
                                ds_xfrac, ds_yfrac, ds_xstep, ds_ystep,
                                ds_colormap);
}

boolean R_GPU_DrawSpanDirect(int y, int x1, int x2, const byte *source,
                             fixed_t xfrac, fixed_t yfrac,
                             fixed_t xstep, fixed_t ystep,
                             const byte *colormap)
{
    int light = gpu_colormap_row((const lighttable_t *)colormap);
    return R_GPU_DrawSpanLightDirect(y, x1, x2, source,
                                     xfrac, yfrac, xstep, ystep, light);
}

boolean R_GPU_DrawSpanLightDirect(int y, int x1, int x2, const byte *source,
                                  fixed_t xfrac, fixed_t yfrac,
                                  fixed_t xstep, fixed_t ystep, int light)
{
    int count = x2 - x1 + 1;
    if (!gpu_present || !gpu_frame_active || I_VideoBuffer == NULL)
        return false;
    if (!gpu_plane_spans_enabled)
        return false;
    if (count <= 0 || count > 4095 || source == NULL)
        return false;
    if (light < 0 || light > 63)
        return false;

    if (gpu_span_batch_count == 0)
    {
        gpu_prepare_for_gpu_write();
        gpu_flush_column_batches();
    }

    of_gpu_span_t *span = &gpu_span_batch[gpu_span_batch_count++];
    span->fb_addr = gpu_framebuffer_addr(ylookup[y] + columnofs[x1]);
    span->tex_addr = (uint32_t)(uintptr_t)source;
    span->s = xfrac;
    span->t = yfrac;
    span->sstep = xstep;
    span->tstep = ystep;
    span->count = (uint16_t)count;
    span->light = (uint8_t)light;
    span->flags = OF_GPU_SPAN_COLORMAP;
    span->colormap_id = 0;
    span->fb_stride = 1;
    span->tex_width = 64;
    span->tex_w_mask = 63;
    span->tex_h_mask = 63;

    if (gpu_span_batch_count == GPU_SPAN_BATCH_SPANS)
        gpu_flush_span_batch();

    R_Perf_CountGpuSpan((unsigned int)count);
    return true;
}

boolean R_GPU_DeferLumpRelease(int lumpnum)
{
    if (!gpu_present || !gpu_frame_active ||
        (!gpu_pending && !gpu_has_pending_draw_batches()))
        return false;

    for (int i = 0; i < gpu_deferred_lump_count; i++)
    {
        if (gpu_deferred_lumps[i] == lumpnum)
            return true;
    }

    if (gpu_deferred_lump_count == GPU_DEFERRED_LUMPS)
    {
        gpu_finish_pending();
        return false;
    }

    gpu_deferred_lumps[gpu_deferred_lump_count++] = lumpnum;
    return true;
}

#endif
