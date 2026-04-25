#include "tile_cache.h"

#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "reader_config.h"
#include "reader_font.h"

void tile_cache_init_context(tile_cache_context_t *ctx,
                             document_layout_t *layout,
                             lv_color_t **buffers,
                             uint32_t buffer_count,
                             uint32_t *generation_counter,
                             const char *title,
                             const char *content)
{
    ctx->layout = layout;
    ctx->buffers = buffers;
    ctx->buffer_count = buffer_count;
    ctx->generation_counter = generation_counter;
    ctx->title = title;
    ctx->content = content;
}

uint32_t tile_cache_count_loaded(const tile_cache_context_t *ctx)
{
    uint32_t loaded = 0;
    for (uint32_t i = 0; i < ctx->layout->tile_count; i++)
    {
        if (ctx->layout->tiles[i].valid && ctx->layout->tiles[i].buffer)
            loaded++;
    }
    return loaded;
}

static lv_color_t *acquire_tile_buffer(tile_cache_context_t *ctx)
{
    for (uint32_t slot = 0; slot < ctx->buffer_count; slot++)
    {
        lv_color_t *buffer = ctx->buffers[slot];
        bool in_use = false;

        for (uint32_t index = 0; index < ctx->layout->tile_count; index++)
        {
            if (ctx->layout->tiles[index].valid && ctx->layout->tiles[index].buffer == buffer)
            {
                in_use = true;
                break;
            }
        }

        if (!in_use)
            return buffer;
    }

    int32_t oldest_index = -1;
    uint32_t oldest_generation = 0;
    for (uint32_t index = 0; index < ctx->layout->tile_count; index++)
    {
        render_tile_t *tile = &ctx->layout->tiles[index];
        if (!tile->valid || !tile->buffer)
            continue;

        if (oldest_index < 0 || tile->generation < oldest_generation)
        {
            oldest_index = (int32_t)index;
            oldest_generation = tile->generation;
        }
    }

    if (oldest_index < 0)
        return NULL;

    lv_color_t *buffer = ctx->layout->tiles[oldest_index].buffer;
    ctx->layout->tiles[oldest_index].buffer = NULL;
    ctx->layout->tiles[oldest_index].valid = false;
    ctx->layout->tiles[oldest_index].generation = 0;
    return buffer;
}

static bool bake_tile_bitmap(tile_cache_context_t *ctx, render_tile_t *tile)
{
    if (!tile || !tile->buffer || tile->pixel_height == 0)
        return false;

    size_t tile_size = (size_t)LCD_H_RES * (size_t)tile->pixel_height * sizeof(lv_color_t);
    memset(tile->buffer, 0, tile_size);

    lv_obj_t *canvas = lv_canvas_create(lv_scr_act());
    if (!canvas)
        return false;
    lv_canvas_set_buffer(canvas, tile->buffer, LCD_H_RES, tile->pixel_height, LV_IMG_CF_TRUE_COLOR);

    lv_draw_label_dsc_t title_dsc;
    lv_draw_label_dsc_init(&title_dsc);
    title_dsc.color = lv_color_hex(0xFF2200);
    title_dsc.font = reader_font_title();

    int32_t title_bottom = TITLE_Y + ctx->layout->title_height;
    if (TITLE_Y < tile->end_y && title_bottom > tile->start_y)
    {
        lv_canvas_draw_text(canvas,
                            CONTENT_X_PADDING,
                            TITLE_Y - tile->start_y,
                            CONTENT_WIDTH,
                            &title_dsc,
                            ctx->title);
    }

    lv_draw_label_dsc_t body_dsc;
    lv_draw_label_dsc_init(&body_dsc);
    body_dsc.color = lv_color_hex(0xCC1100);
    body_dsc.font = reader_font_body();
    body_dsc.line_space = BODY_LINE_SPACE;

    for (uint32_t index = 0; index < ctx->layout->paragraph_count; index++)
    {
        paragraph_span_t paragraph_span = ctx->layout->paragraphs[index];
        const char *paragraph = ctx->layout->paragraph_texts[index];
        if (!paragraph)
            break;

        int32_t paragraph_bottom = paragraph_span.y_offset + paragraph_span.height;
        if (paragraph_span.y_offset < tile->end_y && paragraph_bottom > tile->start_y)
        {
            lv_canvas_draw_text(canvas,
                                CONTENT_X_PADDING,
                                paragraph_span.y_offset - tile->start_y,
                                CONTENT_WIDTH,
                                &body_dsc,
                                paragraph);
        }

        if (paragraph_span.y_offset >= tile->end_y)
            break;
    }

    lv_obj_del(canvas);
    tile->valid = true;
    tile->generation = ++(*ctx->generation_counter);
    return true;
}

bool tile_cache_ensure_resident(tile_cache_context_t *ctx, int32_t tile_idx)
{
    if (tile_idx < 0 || (uint32_t)tile_idx >= ctx->layout->tile_count)
        return false;

    render_tile_t *tile = &ctx->layout->tiles[tile_idx];
    if (tile->valid && tile->buffer)
    {
        tile->generation = ++(*ctx->generation_counter);
        return true;
    }

    lv_color_t *buffer = acquire_tile_buffer(ctx);
    if (!buffer)
        return false;

    for (uint32_t index = 0; index < ctx->layout->tile_count; index++)
    {
        if ((int32_t)index == tile_idx)
            continue;
        if (ctx->layout->tiles[index].buffer == buffer)
        {
            ctx->layout->tiles[index].buffer = NULL;
            ctx->layout->tiles[index].valid = false;
            ctx->layout->tiles[index].generation = 0;
        }
    }

    tile->buffer = buffer;
    return bake_tile_bitmap(ctx, tile);
}

void tile_cache_prefetch_neighbors(tile_cache_context_t *ctx, int32_t center_tile_idx, int32_t direction)
{
    int32_t primary_idx = center_tile_idx + direction;
    int32_t secondary_idx = center_tile_idx - direction;

    if (primary_idx >= 0 && (uint32_t)primary_idx < ctx->layout->tile_count)
        tile_cache_ensure_resident(ctx, primary_idx);
    if (secondary_idx >= 0 && (uint32_t)secondary_idx < ctx->layout->tile_count)
        tile_cache_ensure_resident(ctx, secondary_idx);
}
