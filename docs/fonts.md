# Custom LVGL Font Pipeline

This project supports runtime-selectable reader font sizes and optional custom LVGL fonts.

## Runtime Profiles

The reader runtime has 3 profiles:

- small
- medium (default)
- large

Body/title font selection is handled by `main/reader_font.c`.

## Generate Custom Fonts

Prerequisites:

- Node.js available on PATH (`npx` must work)
- Python environment (`.venv`) already created
- A `.ttf` or `.otf` font file with punctuation coverage (e.g. curly quotes and em dashes)

Run:

```powershell
.\.venv\Scripts\python.exe tools\generate_lvgl_fonts.py --font "C:\path\to\YourFont.ttf"
```

Outputs:

- `main/fonts/book_font_body_16.c`
- `main/fonts/book_font_body_20.c`
- `main/fonts/book_font_body_24.c`
- `main/fonts/book_font_title_18.c`
- `main/fonts/book_font_title_22.c`
- `main/fonts/book_font_title_26.c`
- `main/book_font.h`

`main/CMakeLists.txt` automatically compiles any `main/fonts/*.c` files, so generated fonts are picked up on next build.

## Fallback Behavior

If `main/book_font.h` is missing, runtime falls back to built-in LVGL fonts.

## EPUB Extraction

Smart punctuation is preserved by default in `tools/epub_extract.py`.

If needed for limited glyph sets:

```powershell
.\.venv\Scripts\python.exe tools\epub_extract.py book\your.epub --ascii-punctuation
```
