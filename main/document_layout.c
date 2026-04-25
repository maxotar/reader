#include "document_layout.h"

#include "reader_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TITLE_BODY_GAP 12

static int32_t compute_body_start_y(const document_layout_t *layout)
{
    int32_t start_y = TITLE_Y + (int32_t)layout->title_height + TITLE_BODY_GAP;
    if (start_y < BODY_Y)
        start_y = BODY_Y;
    return start_y;
}

static char *copy_paragraph_text_range(const char *text, uint32_t start_offset, uint32_t end_offset)
{
    size_t len = end_offset - start_offset;
    char *buffer = malloc(len + 1);
    if (!buffer)
        return NULL;

    memcpy(buffer, text + start_offset, len);
    buffer[len] = '\0';
    return buffer;
}

static size_t count_paragraphs(const char *text)
{
    size_t count = 0;
    const char *paragraph_start = text;
    const char *cursor = text;

    while (*cursor)
    {
        bool at_boundary = (cursor[0] == '\n' && cursor[1] == '\n');
        if (at_boundary)
        {
            if (cursor > paragraph_start)
                count++;
            cursor += 2;
            paragraph_start = cursor;
            continue;
        }
        cursor++;
    }

    if (paragraph_start < cursor)
        count++;

    return count;
}

static int32_t compute_body_height(const document_layout_t *layout)
{
    if (layout->paragraph_count == 0)
        return 0;

    const paragraph_span_t *last_paragraph = &layout->paragraphs[layout->paragraph_count - 1];
    return (last_paragraph->y_offset + last_paragraph->height) - compute_body_start_y(layout);
}

static void compute_paragraph_layout(document_layout_t *layout, const char *text)
{
    (void)text;
    int32_t paragraph_y = compute_body_start_y(layout);

    for (uint32_t index = 0; index < layout->paragraph_count; index++)
    {
        paragraph_span_t *paragraph_span = &layout->paragraphs[index];
        const char *paragraph = layout->paragraph_texts[index];
        if (!paragraph)
        {
            paragraph_span->y_offset = paragraph_y;
            paragraph_span->height = 0;
            continue;
        }

        lv_point_t paragraph_size;
        lv_txt_get_size(&paragraph_size,
                        paragraph,
                        reader_font_body(),
                        0,
                        BODY_LINE_SPACE,
                        CONTENT_WIDTH,
                        LV_TEXT_FLAG_NONE);

        paragraph_span->y_offset = paragraph_y;
        paragraph_span->height = (uint16_t)paragraph_size.y;
        paragraph_y += paragraph_size.y + BODY_PARAGRAPH_GAP;
    }
}

static int32_t compute_document_height(const document_layout_t *layout)
{
    int32_t body_start_y = compute_body_start_y(layout);
    int32_t body_height = compute_body_height(layout);
    int32_t title_bottom = TITLE_Y + layout->title_height;
    int32_t body_bottom = body_start_y + body_height + CONTENT_BOTTOM_PADDING;
    int32_t total_height = (body_bottom > title_bottom) ? body_bottom : title_bottom;

    return (total_height > LCD_V_RES) ? total_height : LCD_V_RES;
}

static void build_paragraph_index(document_layout_t *layout, const char *text)
{
    size_t paragraph_capacity = count_paragraphs(text);
    layout->paragraph_count = (uint32_t)paragraph_capacity;

    if (paragraph_capacity == 0)
        return;

    layout->paragraphs = calloc(paragraph_capacity, sizeof(paragraph_span_t));
    layout->paragraph_texts = calloc(paragraph_capacity, sizeof(char *));
    if (!layout->paragraphs || !layout->paragraph_texts)
    {
        printf("build_paragraph_index: allocation failed for %u paragraphs\n", (unsigned)layout->paragraph_count);
        abort();
    }

    const char *paragraph_start = text;
    const char *cursor = text;
    uint32_t paragraph_index = 0;

    layout->content_length = (uint32_t)strlen(text);

    while (*cursor)
    {
        bool at_boundary = (cursor[0] == '\n' && cursor[1] == '\n');
        if (at_boundary)
        {
            if (cursor > paragraph_start)
            {
                paragraph_span_t *span = &layout->paragraphs[paragraph_index];
                span->start_offset = (uint32_t)(paragraph_start - text);
                span->end_offset = (uint32_t)(cursor - text);

                layout->paragraph_texts[paragraph_index] = copy_paragraph_text_range(text,
                                                                                     span->start_offset,
                                                                                     span->end_offset);
                if (!layout->paragraph_texts[paragraph_index])
                {
                    printf("build_paragraph_index: failed to allocate paragraph text %u\n", (unsigned)paragraph_index);
                    abort();
                }
                paragraph_index++;
            }

            cursor += 2;
            paragraph_start = cursor;
            continue;
        }
        cursor++;
    }

    if (paragraph_start < cursor)
    {
        paragraph_span_t *span = &layout->paragraphs[paragraph_index];
        span->start_offset = (uint32_t)(paragraph_start - text);
        span->end_offset = (uint32_t)(cursor - text);

        layout->paragraph_texts[paragraph_index] = copy_paragraph_text_range(text,
                                                                             span->start_offset,
                                                                             span->end_offset);
        if (!layout->paragraph_texts[paragraph_index])
        {
            printf("build_paragraph_index: failed to allocate trailing paragraph text %u\n", (unsigned)paragraph_index);
            abort();
        }
        paragraph_index++;
    }

    layout->paragraph_count = paragraph_index;
}

static void assign_tile_ranges(document_layout_t *layout)
{
    uint32_t tile_count = (uint32_t)(((uint32_t)layout->total_height + (uint32_t)TILE_HEIGHT - 1U) / (uint32_t)TILE_HEIGHT);
    if (tile_count == 0)
        tile_count = 1;

    layout->tiles = calloc(tile_count, sizeof(render_tile_t));
    if (!layout->tiles)
    {
        printf("assign_tile_ranges: allocation failed for %u tiles\n", (unsigned)tile_count);
        abort();
    }

    layout->tile_count = tile_count;
    int32_t y = 0;
    for (uint32_t index = 0; index < layout->tile_count; index++)
    {
        render_tile_t *tile = &layout->tiles[index];
        tile->start_y = y;
        tile->end_y = y + TILE_HEIGHT;
        if (tile->end_y > layout->total_height)
            tile->end_y = layout->total_height;
        tile->pixel_height = (uint16_t)(tile->end_y - tile->start_y);
        tile->buffer = NULL;
        tile->valid = false;
        tile->generation = 0;
        y += TILE_HEIGHT;
    }
}

void reset_document_layout(document_layout_t *layout)
{
    if (!layout)
        return;

    if (layout->paragraph_texts)
    {
        for (uint32_t i = 0; i < layout->paragraph_count; i++)
            free(layout->paragraph_texts[i]);
    }

    free(layout->paragraph_texts);
    free(layout->paragraphs);
    free(layout->tiles);

    layout->paragraph_texts = NULL;
    layout->paragraphs = NULL;
    layout->tiles = NULL;
    layout->content_length = 0;
    layout->paragraph_count = 0;
    layout->line_count = 0;
    layout->title_height = 0;
    layout->total_height = 0;
    layout->tile_count = 0;
}

void init_document_layout(document_layout_t *layout, const char *title, const char *text)
{
    reset_document_layout(layout);
    build_paragraph_index(layout, text);

    lv_point_t title_size;
    lv_txt_get_size(&title_size,
                    title,
                    reader_font_title(),
                    0,
                    0,
                    CONTENT_WIDTH,
                    LV_TEXT_FLAG_NONE);
    layout->title_height = (uint16_t)title_size.y;
    compute_paragraph_layout(layout, text);
    layout->total_height = compute_document_height(layout);
    assign_tile_ranges(layout);
}

int32_t find_tile_for_y(const document_layout_t *layout, int32_t virtual_y)
{
    if (!layout || !layout->tiles)
        return -1;

    for (uint32_t i = 0; i < layout->tile_count; i++)
    {
        if (virtual_y >= layout->tiles[i].start_y && virtual_y < layout->tiles[i].end_y)
            return (int32_t)i;
    }
    return -1;
}

size_t document_layout_metadata_bytes(const document_layout_t *layout)
{
    if (!layout)
        return 0;

    return ((size_t)layout->paragraph_count * sizeof(paragraph_span_t)) +
           ((size_t)layout->paragraph_count * sizeof(char *)) +
           ((size_t)layout->tile_count * sizeof(render_tile_t));
}

size_t document_layout_text_bytes(const document_layout_t *layout)
{
    if (!layout || !layout->paragraphs || !layout->paragraph_texts)
        return 0;

    size_t total = 0;
    for (uint32_t i = 0; i < layout->paragraph_count; i++)
    {
        if (!layout->paragraph_texts[i])
            continue;
        total += (size_t)(layout->paragraphs[i].end_offset - layout->paragraphs[i].start_offset) + 1;
    }
    return total;
}
