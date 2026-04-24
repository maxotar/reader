# Tile Cache Implementation Plan

## Goal
Replace the single giant pre-rendered chapter buffer with a bounded rotating tile cache that:

- keeps scroll FPS above 60 when possible
- keeps idle CPU near zero
- supports scrolling both forward and backward
- uses LVGL for layout and text rasterization
- uses manual blitting for runtime viewport updates

## Constraints

- ESP32-S3 with limited internal RAM
- 8 MB PSRAM shared with frame buffers and caches
- 16 MB flash for firmware, assets, and text content
- long chapters must not require one monolithic bitmap

## Design Summary

### Persistent storage

Store chapter source text as UTF-8 in flash.

Per chapter store:

- chapter id
- byte length
- paragraph offsets
- optional navigation metadata

Do not store rendered tiles in flash.

### Runtime layout state

Build and keep:

- paragraph index
- wrapped line index
- total virtual content height
- rotating tile cache state

### Tile cache

Start with 3 PSRAM tiles:

- previous
- current
- next

Recommended first tile height: 512 px.

Also keep one screen-sized scratch buffer for composition when the viewport crosses tile boundaries.

## Split Rules

1. Never split on raw byte count.
2. Never require a paragraph to fit inside a tile.
3. Use LVGL-compatible layout measurement to determine wrapped lines.
4. Only split tiles between complete wrapped lines.

This guarantees:

- no broken UTF-8 sequences
- no cut-off letters
- no cut words
- paragraph spans can cross tile boundaries safely

## Implementation Steps

### Phase 1: Scaffolding

- Add document, line, and tile structs.
- Add paragraph indexing for the current sample text.
- Add tile cache metadata without changing runtime rendering yet.
- Keep the current single-buffer renderer working.

### Phase 2: Layout index

- Build a wrapped line index using LVGL-compatible measurement.
- Replace fixed chapter height with computed virtual content height.
- Map scroll range to line y positions.

### Phase 3: Tile rendering

- Allocate 3 tile buffers in PSRAM.
- Render a tile from a range of wrapped lines.
- Track tile y range and line range.

### Phase 4: Viewport composition

- If the viewport is inside one tile, blit directly.
- If the viewport crosses two tiles, compose into the scratch buffer and blit once.

### Phase 5: Rotation and prefetch

- Rotate tiles when scroll nears tile boundaries.
- Prefetch in the active scroll direction.
- Preserve backward scrolling by keeping one previous tile warm.

## Initial Data Structures

- paragraph_span_t: byte start and end for each paragraph
- wrapped_line_t: byte range and y metrics for each rendered line
- render_tile_t: PSRAM tile buffer and covered line/y range
- document_layout_t: chapter-level runtime state

## First Coding Milestone

Complete these without changing visible behavior:

- document structs
- paragraph indexing
- tile cache metadata and placeholder initialization
- plan for replacing CANVAS_HEIGHT with computed document height

## Validation Checklist

- current build still works after scaffolding
- idle FPS remains 0
- idle CPU remains low
- no regression in current single-buffer scroll path
- paragraph count and content length look correct in diagnostics
