from __future__ import annotations

import argparse
import json
import posixpath
import re
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable
import xml.etree.ElementTree as ET

from bs4 import BeautifulSoup


CONTAINER_PATH = "META-INF/container.xml"
OPF_NS = {"opf": "http://www.idpf.org/2007/opf"}
CONTAINER_NS = {"container": "urn:oasis:names:tc:opendocument:xmlns:container"}
HTML_ITEM_EXTENSIONS = (".xhtml", ".html", ".htm")
FRONT_MATTER_MARKERS = {
    "cover",
    "titlepage",
    "halftitlepage",
    "imprint",
    "copyright",
    "dedication",
    "acknowledgements",
    "acknowledgments",
    "contents",
    "toc",
    "nav",
    "foreword",
    "preface",
    "introduction",
}
CHAPTER_HINT_RE = re.compile(r"(?:^|[-_\s])(chapter|part|prologue|epilogue|book)[-_\s]*\d*", re.IGNORECASE)

ASCII_PUNCT_TRANSLATIONS = str.maketrans(
    {
        "\u2018": "'",
        "\u2019": "'",
        "\u201a": "'",
        "\u201b": "'",
        "\u201c": '"',
        "\u201d": '"',
        "\u201e": '"',
        "\u201f": '"',
        "\u2013": "-",
        "\u2014": "--",
        "\u2015": "--",
        "\u2026": "...",
    }
)

UNICODE_WHITESPACE_TRANSLATIONS = str.maketrans(
    {
        "\u00a0": " ",
        "\u2009": " ",
        "\u200a": " ",
        "\u202f": " ",
        "\ufeff": "",
    }
)


@dataclass
class SpineItem:
    item_id: str
    href: str
    media_type: str


@dataclass
class EpubBook:
    epub_path: Path
    opf_path: str
    title: str
    manifest: dict[str, SpineItem]
    spine: list[str]


def read_xml(zf: zipfile.ZipFile, path: str) -> ET.Element:
    with zf.open(path) as handle:
        return ET.fromstring(handle.read())


def find_opf_path(zf: zipfile.ZipFile) -> str:
    root = read_xml(zf, CONTAINER_PATH)
    node = root.find("container:rootfiles/container:rootfile", CONTAINER_NS)
    if node is None:
        raise ValueError("EPUB container.xml does not declare a package document")
    full_path = node.attrib.get("full-path")
    if not full_path:
        raise ValueError("EPUB rootfile is missing full-path")
    return full_path


def parse_book(zf: zipfile.ZipFile, epub_path: Path) -> EpubBook:
    opf_path = find_opf_path(zf)
    root = read_xml(zf, opf_path)

    title_node = root.find("opf:metadata/*[{http://purl.org/dc/elements/1.1/}title]", OPF_NS)
    if title_node is None:
        title_node = root.find("opf:metadata/{http://purl.org/dc/elements/1.1/}title", OPF_NS)
    title = title_node.text.strip() if title_node is not None and title_node.text else epub_path.stem

    manifest: dict[str, SpineItem] = {}
    for item in root.findall("opf:manifest/opf:item", OPF_NS):
        item_id = item.attrib.get("id")
        href = item.attrib.get("href")
        media_type = item.attrib.get("media-type", "")
        if not item_id or not href:
            continue
        manifest[item_id] = SpineItem(item_id=item_id, href=href, media_type=media_type)

    spine: list[str] = []
    for itemref in root.findall("opf:spine/opf:itemref", OPF_NS):
        idref = itemref.attrib.get("idref")
        if idref:
            spine.append(idref)

    return EpubBook(epub_path=epub_path, opf_path=opf_path, title=title, manifest=manifest, spine=spine)


def opf_dir(opf_path: str) -> str:
    directory = posixpath.dirname(opf_path)
    return directory if directory else ""


def resolve_href(base_dir: str, href: str) -> str:
    if not base_dir:
        return posixpath.normpath(href)
    return posixpath.normpath(posixpath.join(base_dir, href))


def is_probable_front_matter(item: SpineItem) -> bool:
    basename = posixpath.splitext(posixpath.basename(item.href))[0].lower()
    return basename in FRONT_MATTER_MARKERS


def is_probable_chapter(item: SpineItem) -> bool:
    basename = posixpath.splitext(posixpath.basename(item.href))[0]
    return bool(CHAPTER_HINT_RE.search(basename))


def select_content_items(book: EpubBook) -> list[SpineItem]:
    chapter_candidates: list[SpineItem] = []
    fallback_candidates: list[SpineItem] = []

    for item_id in book.spine:
        item = book.manifest.get(item_id)
        if item is None:
            continue
        href_lower = item.href.lower()
        if item.media_type in {"application/xhtml+xml", "text/html"} or href_lower.endswith(HTML_ITEM_EXTENSIONS):
            if is_probable_front_matter(item):
                continue
            if is_probable_chapter(item):
                chapter_candidates.append(item)
            else:
                fallback_candidates.append(item)

    if chapter_candidates:
        return chapter_candidates
    if fallback_candidates:
        return fallback_candidates
    raise ValueError("EPUB spine does not contain an XHTML/HTML document")


def normalize_text_blocks(blocks: Iterable[str]) -> str:
    cleaned: list[str] = []
    whitespace_re = re.compile(r"\s+")
    for block in blocks:
        compact = whitespace_re.sub(" ", block).strip()
        if compact:
            cleaned.append(compact)
    return "\n\n".join(cleaned)


def normalize_for_lvgl_font(text: str, ascii_punctuation: bool) -> str:
    normalized = text.translate(UNICODE_WHITESPACE_TRANSLATIONS)
    if ascii_punctuation:
        normalized = normalized.translate(ASCII_PUNCT_TRANSLATIONS)
    return normalized


def to_c_string_literal(text: str, wrap_width: int = 96) -> str:
    utf8 = text.encode("utf-8")
    chunks: list[str] = []
    current = ""

    def flush_current() -> None:
        nonlocal current
        if current:
            chunks.append(current)
            current = ""

    for byte in utf8:
        if byte == 0x0A:
            token = r"\n"
        elif byte == 0x09:
            token = r"\t"
        elif byte == 0x0D:
            token = r"\r"
        elif byte == 0x5C:
            token = r"\\"
        elif byte == 0x22:
            token = r"\""
        elif 0x20 <= byte <= 0x7E:
            token = chr(byte)
        else:
            token = f"\\{byte:03o}"

        if len(current) + len(token) > wrap_width:
            flush_current()
        current += token

    flush_current()

    if not chunks:
        return '""'

    lines = [f'    "{chunk}"' for chunk in chunks]
    return "\n".join(lines)


def extract_document_text(zf: zipfile.ZipFile, book: EpubBook, item: SpineItem) -> str:
    chapter_path = resolve_href(opf_dir(book.opf_path), item.href)
    with zf.open(chapter_path) as handle:
        raw_bytes = handle.read()

    chapter_markup = raw_bytes.decode("utf-8")
    soup = BeautifulSoup(chapter_markup, "xml")

    body = soup.find("body")
    if body is None:
        raise ValueError(f"Spine document has no body: {chapter_path}")

    blocks: list[str] = []
    for node in body.find_all(["h1", "h2", "h3", "h4", "h5", "h6", "p", "li", "blockquote"], recursive=True):
        text = node.get_text(" ", strip=True)
        if text:
            blocks.append(text)

    if not blocks:
        fallback = body.get_text(" ", strip=True)
        if fallback:
            blocks.append(fallback)

    return normalize_text_blocks(blocks)


def derive_chapter_title(item: SpineItem, chapter_text: str, chapter_number: int) -> str:
    lines = [line.strip() for line in chapter_text.split("\n\n") if line.strip()]
    first_line = lines[0] if lines else ""

    roman_re = re.compile(r"^[IVXLCDM]+$", re.IGNORECASE)
    if first_line and len(first_line) <= 12 and roman_re.match(first_line) and len(lines) >= 2:
        second_line = lines[1]
        if second_line and len(second_line) <= 100:
            return f"{first_line} - {second_line}"

    if first_line and len(first_line) <= 100:
        return first_line

    stem = posixpath.splitext(posixpath.basename(item.href))[0]
    stem_clean = re.sub(r"[_\-]+", " ", stem).strip()
    if stem_clean:
        stem_clean = re.sub(r"\s+", " ", stem_clean)
        return stem_clean.title()

    return f"Chapter {chapter_number}"


def strip_title_from_content(chapter_text: str, chapter_title: str) -> str:
    """Remove the chapter title from the beginning of the content text.
    
    If the content starts with the title lines (which are used to derive chapter_title),
    remove them to avoid displaying the title twice in the renderer.
    """
    lines = [line.strip() for line in chapter_text.split("\n\n") if line.strip()]
    if not lines:
        return chapter_text

    # Check if the title is a "Roman - Subtitle" pattern
    roman_re = re.compile(r"^[IVXLCDM]+$", re.IGNORECASE)
    if (len(lines) >= 2 and 
        roman_re.match(lines[0]) and 
        chapter_title.startswith(lines[0] + " - ") and
        chapter_title.endswith(lines[1])):
        # Remove first two paragraphs (roman numeral and subtitle)
        remaining_text = "\n\n".join(lines[2:])
        return remaining_text if remaining_text.strip() else chapter_text
    
    # Check if the title matches the first line
    if lines and lines[0] == chapter_title:
        # Remove first paragraph
        remaining_text = "\n\n".join(lines[1:])
        return remaining_text if remaining_text.strip() else chapter_text
    
    return chapter_text


def build_reader_header(title: str,
                        chapter_titles: list[str],
                        chapter_texts: list[str],
                        source_name: str) -> str:
    escaped_book_title = to_c_string_literal(title)

    chapter_entries: list[str] = []
    chapter_title_decl: list[str] = []
    chapter_text_decl: list[str] = []

    for i, (chapter_title, chapter_text) in enumerate(zip(chapter_titles, chapter_texts)):
        chapter_title_decl.append(
            f"static const char generated_chapter_title_{i:03d}[] =\n"
            f"{to_c_string_literal(chapter_title)};\n"
        )
        chapter_text_decl.append(
            f"static const char generated_chapter_content_{i:03d}[] =\n"
            f"{to_c_string_literal(chapter_text)};\n"
        )
        chapter_entries.append(
            f"    generated_chapter_title_{i:03d}"
        )

    chapter_content_entries: list[str] = []
    for i in range(len(chapter_texts)):
        chapter_content_entries.append(f"    generated_chapter_content_{i:03d}")

    parts: list[str] = [
        "#ifndef CONTENT_GENERATED_H\n",
        "#define CONTENT_GENERATED_H\n\n",
        "#include <stdint.h>\n\n",
        "#define CONTENT_GENERATED_HAS_CHAPTERS 1\n\n",
        f"// Auto-generated from {source_name}. Do not edit manually.\n",
        "static const char book_title[] =\n",
        f"{escaped_book_title};\n\n",
        f"static const uint16_t generated_chapter_count = {len(chapter_texts)};\n\n",
        "\n".join(chapter_title_decl),
        "\n",
        "\n".join(chapter_text_decl),
        "\n",
        "static const char *const generated_chapter_titles[] = {\n",
        ",\n".join(chapter_entries),
        "\n};\n\n",
        "static const char *const generated_chapter_contents[] = {\n",
        ",\n".join(chapter_content_entries),
        "\n};\n\n",
        "#endif\n",
    ]
    return "".join(parts)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Inspect an EPUB and extract all content chapters")
    parser.add_argument("epub", type=Path, help="Path to the EPUB file")
    parser.add_argument("--text-out", type=Path, default=Path("generated/book_preview.txt"))
    parser.add_argument("--header-out", type=Path, default=Path("main/content_generated.h"))
    parser.add_argument(
        "--ascii-punctuation",
        action="store_true",
        help="Normalize smart punctuation to ASCII for limited font sets",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    epub_path = args.epub
    if not epub_path.exists():
        raise FileNotFoundError(epub_path)

    with zipfile.ZipFile(epub_path) as zf:
        book = parse_book(zf, epub_path)
        selected_items = select_content_items(book)

        chapter_titles: list[str] = []
        chapter_texts: list[str] = []
        chapter_hrefs: list[str] = []

        for i, item in enumerate(selected_items, start=1):
            chapter_text = extract_document_text(zf, book, item)
            chapter_text = normalize_for_lvgl_font(chapter_text, args.ascii_punctuation)
            if not chapter_text:
                continue

            chapter_title = derive_chapter_title(item, chapter_text, i)
            chapter_text = strip_title_from_content(chapter_text, chapter_title)
            chapter_titles.append(chapter_title)
            chapter_texts.append(chapter_text)
            chapter_hrefs.append(item.href)

        if not chapter_texts:
            raise ValueError("No chapter content extracted from EPUB")

    args.text_out.parent.mkdir(parents=True, exist_ok=True)
    args.header_out.parent.mkdir(parents=True, exist_ok=True)
    preview_lines: list[str] = []
    for i, (chapter_title, chapter_text) in enumerate(zip(chapter_titles, chapter_texts), start=1):
        preview_lines.append(f"[{i}] {chapter_title}")
        preview_lines.append(chapter_text)
        preview_lines.append("")
    args.text_out.write_text("\n\n".join(preview_lines), encoding="utf-8")
    args.header_out.write_text(
        build_reader_header(book.title, chapter_titles, chapter_texts, epub_path.name),
        encoding="utf-8",
    )

    summary = {
        "epub": str(epub_path),
        "title": book.title,
        "opf_path": book.opf_path,
        "chapter_count": len(chapter_texts),
        "chapter_hrefs": chapter_hrefs,
        "text_output": str(args.text_out),
        "header_output": str(args.header_out),
        "ascii_punctuation": args.ascii_punctuation,
        "character_count": sum(len(text) for text in chapter_texts),
        "paragraph_count": sum(len([part for part in text.split("\n\n") if part.strip()]) for text in chapter_texts),
    }
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())