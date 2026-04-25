#ifndef DOCUMENT_LAYOUT_H
#define DOCUMENT_LAYOUT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "reader_config.h"

typedef struct
{
    uint32_t start_offset;
    uint32_t end_offset;
    int32_t y_offset;
    uint16_t height;
} paragraph_span_t;

typedef struct
{
    uint32_t start_offset;
    uint32_t end_offset;
    int32_t y_offset;
    uint16_t height;
} wrapped_line_t;

typedef struct
{
    lv_color_t *buffer;
    uint16_t pixel_height;
    uint32_t start_line;
    uint32_t end_line;
    int32_t start_y;
    int32_t end_y;
    uint32_t generation;
    bool valid;
} render_tile_t;

typedef struct
{
    uint32_t content_length;
    uint32_t paragraph_count;
    uint32_t line_count;
    uint16_t title_height;
    int32_t total_height;
    paragraph_span_t *paragraphs;
    char **paragraph_texts;
    uint32_t tile_count;
    render_tile_t *tiles;
} document_layout_t;

void reset_document_layout(document_layout_t *layout);
void init_document_layout(document_layout_t *layout, const char *title, const char *text);
int32_t find_tile_for_y(const document_layout_t *layout, int32_t virtual_y);
size_t document_layout_metadata_bytes(const document_layout_t *layout);
size_t document_layout_text_bytes(const document_layout_t *layout);

#endif
