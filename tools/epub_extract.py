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


def select_first_content_item(book: EpubBook) -> SpineItem:
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
        return chapter_candidates[0]
    if fallback_candidates:
        return fallback_candidates[0]
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


def build_reader_header(title: str, chapter_text: str, source_name: str) -> str:
    escaped_title = to_c_string_literal(title)
    escaped_text = to_c_string_literal(chapter_text)
    return (
        "#ifndef CONTENT_GENERATED_H\n"
        "#define CONTENT_GENERATED_H\n\n"
        f"// Auto-generated from {source_name}. Do not edit manually.\n"
        "static const char chapter_title[] =\n"
        f"{escaped_title};\n\n"
        "static const char chapter_content[] =\n"
        f"{escaped_text};\n\n"
        "#endif\n"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Inspect an EPUB and extract the first spine chapter")
    parser.add_argument("epub", type=Path, help="Path to the EPUB file")
    parser.add_argument("--text-out", type=Path, default=Path("generated/first_chapter.txt"))
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
        first_item = select_first_content_item(book)
        first_text = extract_document_text(zf, book, first_item)
        first_text = normalize_for_lvgl_font(first_text, args.ascii_punctuation)

    args.text_out.parent.mkdir(parents=True, exist_ok=True)
    args.header_out.parent.mkdir(parents=True, exist_ok=True)
    args.text_out.write_text(first_text, encoding="utf-8")
    args.header_out.write_text(
        build_reader_header(book.title, first_text, epub_path.name),
        encoding="utf-8",
    )

    summary = {
        "epub": str(epub_path),
        "title": book.title,
        "opf_path": book.opf_path,
        "first_spine_item": first_item.item_id,
        "first_spine_href": first_item.href,
        "text_output": str(args.text_out),
        "header_output": str(args.header_out),
        "ascii_punctuation": args.ascii_punctuation,
        "character_count": len(first_text),
        "paragraph_count": len([part for part in first_text.split("\n\n") if part.strip()]),
    }
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())