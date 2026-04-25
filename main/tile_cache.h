#ifndef TILE_CACHE_H
#define TILE_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#include "document_layout.h"

typedef struct
{
    document_layout_t *layout;
    lv_color_t **buffers;
    uint32_t buffer_count;
    uint32_t *generation_counter;
    const char *title;
    const char *content;
} tile_cache_context_t;

void tile_cache_init_context(tile_cache_context_t *ctx,
                             document_layout_t *layout,
                             lv_color_t **buffers,
                             uint32_t buffer_count,
                             uint32_t *generation_counter,
                             const char *title,
                             const char *content);
uint32_t tile_cache_count_loaded(const tile_cache_context_t *ctx);
void tile_cache_invalidate_all(tile_cache_context_t *ctx);
bool tile_cache_ensure_resident(tile_cache_context_t *ctx, int32_t tile_idx);
void tile_cache_prefetch_neighbors(tile_cache_context_t *ctx, int32_t center_tile_idx, int32_t direction);

#endif
