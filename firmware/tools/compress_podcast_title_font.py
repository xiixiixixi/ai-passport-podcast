#!/usr/bin/env python3
"""Pack lv_font_conv's uncompressed 4bpp output without changing any pixels.

Canonical byte-Huffman streams are independently byte-aligned per glyph.
The glyph bitmap_index high bit marks compressed data; original cmap and
metrics remain untouched. Raw reconstruction is checked for every glyph.
"""
import argparse
from collections import Counter
import hashlib
import heapq
import json
from pathlib import Path
import re

FLAG = 0x80000000
LIMIT = 512


def canonical_codes(raw):
    frequency = Counter(raw)
    queue = [(count, [symbol]) for symbol, count in sorted(frequency.items())]
    heapq.heapify(queue)
    lengths = {symbol: 0 for symbol in frequency}
    while len(queue) > 1:
        a, left = heapq.heappop(queue)
        b, right = heapq.heappop(queue)
        for symbol in left + right:
            lengths[symbol] += 1
        heapq.heappush(queue, (a + b, left + right))
    if len(lengths) == 1:
        lengths[next(iter(lengths))] = 1
    symbols = sorted(lengths, key=lambda symbol: (lengths[symbol], symbol))
    codes, code, previous = {}, 0, 0
    for symbol in symbols:
        bits = lengths[symbol]
        code <<= bits - previous
        codes[symbol] = (code, bits)
        code += 1
        previous = bits
    maximum = max(lengths.values())
    if maximum > 16:
        raise ValueError("Byte Huffman code exceeds the bounded decoder's 16 bits")
    counts, first_codes, first_symbols = ([0] * (maximum + 1) for _ in range(3))
    for n in range(1, maximum + 1):
        row = [symbol for symbol in symbols if lengths[symbol] == n]
        counts[n] = len(row)
        if row:
            first_codes[n] = codes[row[0]][0]
            first_symbols[n] = symbols.index(row[0])
    return codes, counts, first_codes, first_symbols, symbols


def encode(data, codes):
    out, bits, used = bytearray(), 0, 0
    for byte in data:
        code, length = codes[byte]
        bits = (bits << length) | code
        used += length
        while used >= 8:
            used -= 8
            out.append((bits >> used) & 255)
        bits &= (1 << used) - 1
    if used:
        out.append(bits << (8 - used))
    return bytes(out)


def decode(data, size, counts, first_codes, first_symbols, symbols):
    out, bit = bytearray(), 0
    for _ in range(size):
        code = 0
        for n in range(1, len(counts)):
            if bit >= len(data) * 8:
                raise ValueError("Truncated glyph stream")
            code = (code << 1) | ((data[bit // 8] >> (7 - bit % 8)) & 1)
            bit += 1
            delta = code - first_codes[n]
            if 0 <= delta < counts[n]:
                out.append(symbols[first_symbols[n] + delta])
                break
        else:
            raise ValueError("Invalid glyph Huffman code")
    return bytes(out)


def c_array(name, c_type, values, hexadecimal=False):
    def value(x):
        return f"0x{x:02x}" if hexadecimal else str(x)
    lines = ["    " + ", ".join(value(x) for x in values[i:i + 16]) + ","
             for i in range(0, len(values), 16)]
    return f"static LV_ATTRIBUTE_LARGE_CONST const {c_type} {name}[] = {{\n" + "\n".join(lines) + "\n};"


def compress_source(source):
    if "PODCAST_TITLE_FONT_LOSSLESS" in source:
        raise ValueError("Font is already losslessly packed; regenerate raw converter output first")
    if ".bpp = 4" not in source or ".bitmap_format = 0" not in source:
        raise ValueError("Expected uncompressed 4bpp lv_font_conv output")
    start = source.index("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
    end = source.index("};", start) + 2
    block = re.sub(r"/\*.*?\*/", "", source[start:end], flags=re.S)
    raw = bytes(int(token, 16) for token in re.findall(r"0x[0-9a-fA-F]+", block))
    glyph_start = source.index("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    glyph_end = source.index("};", glyph_start) + 2
    pattern = re.compile(r"\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)")
    matches = list(pattern.finditer(source[glyph_start:glyph_end]))
    if not matches:
        raise ValueError("No supported glyph descriptors")
    codes, counts, first_codes, first_symbols, symbols = canonical_codes(raw)
    packed, rebuilt, indexes = bytearray(), bytearray(), []
    compressed, maximum = 0, 0
    for match in matches:
        old_at, _, width, height, _, _ = map(int, match.groups())
        size = (width * height + 1) // 2
        maximum = max(maximum, size)
        if size > LIMIT or old_at != len(rebuilt) or old_at + size > len(raw):
            raise ValueError("Glyph bytes exceed the workspace or are not contiguous")
        original = raw[old_at:old_at + size]
        encoded = encode(original, codes)
        at = len(packed)
        if len(encoded) < size:
            restored = decode(encoded, size, counts, first_codes, first_symbols, symbols)
            at |= FLAG
            compressed += 1
        else:
            encoded, restored = original, original
        if restored != original:
            raise ValueError("A glyph's reconstructed 4bpp bytes differ")
        indexes.append(at)
        packed.extend(encoded)
        rebuilt.extend(restored)
    if bytes(rebuilt) != raw or len(packed) >= FLAG:
        raise ValueError("Reconstruction or offset limit failed")
    replacements = iter(indexes)
    descriptors = pattern.sub(lambda m: ".bitmap_index = " + str(next(replacements)) +
        ", .adv_w = " + m[2] + ", .box_w = " + m[3] + ", .box_h = " + m[4] +
        ", .ofs_x = " + m[5] + ", .ofs_y = " + m[6], source[glyph_start:glyph_end])
    bitmap = c_array("glyph_bitmap", "uint8_t", packed, True)
    table = "\n\n".join([
        "/* PODCAST_TITLE_FONT_LOSSLESS: exact original 16-level 4bpp pixels. */",
        c_array("codec_counts", "uint16_t", counts),
        c_array("codec_first_codes", "uint16_t", first_codes),
        c_array("codec_first_symbols", "uint16_t", first_symbols),
        c_array("codec_symbols", "uint8_t", symbols, True),
        "static const podcast_font_codec_t font_codec = {\n"
        f"    .glyph_count={len(matches)}, .bitmap_bytes={len(packed)},\n"
        f"    .symbol_count={len(symbols)}, .max_code_bits={len(counts)-1},\n"
        "    .counts=codec_counts, .first_codes=codec_first_codes,\n"
        "    .first_symbols=codec_first_symbols, .symbols=codec_symbols\n};"])
    output = source[:start] + bitmap + "\n\n" + table + source[end:glyph_start] + descriptors + source[glyph_end:]
    output = output.replace("#ifndef APP_CJK_18", '#include "podcast_font_codec.h"\n\n#ifndef APP_CJK_18', 1)
    output = output.replace(".get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,", ".get_glyph_bitmap = podcast_font_get_bitmap_lossless,", 1)
    output = output.replace(".user_data = NULL,", ".user_data = (void *)&font_codec,", 1)
    metadata = {"glyphs": len(matches), "raw_bitmap_bytes": len(raw),
        "packed_bitmap_bytes": len(packed), "bitmap_bytes_saved": len(raw) - len(packed),
        "compressed_glyphs": compressed, "raw_glyphs": len(matches) - compressed,
        "max_raw_glyph_bytes": maximum, "max_code_bits": len(counts) - 1,
        "codebook_array_bytes": 2 * (len(counts) + len(first_codes) + len(first_symbols)) + len(symbols),
        "bpp": 4, "gray_levels": 16, "all_glyphs_lossless": True,
        "raw_sha256": hashlib.sha256(raw).hexdigest(),
        "reconstructed_sha256": hashlib.sha256(rebuilt).hexdigest(),
        "packed_sha256": hashlib.sha256(packed).hexdigest(),
        "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
        "output_sha256": hashlib.sha256(output.encode()).hexdigest()}
    return output, metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()
    output, metadata = compress_source(args.input.read_text())
    temporary = args.output.with_suffix(args.output.suffix + ".tmp")
    temporary.write_text(output)
    temporary.replace(args.output)
    if args.manifest:
        args.manifest.write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
