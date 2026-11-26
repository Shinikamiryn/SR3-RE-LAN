"""Writer for Saints Row: The Third Remastered packfiles (v6, 64-bit fields).

Modes (header flags):
  0x0000  uncompressed      entries stored raw, 0x1000-aligned relative to the data start
  0x4801  compressed        each entry LZ4-compressed, entries 0x1000-aligned in file and in uncompressed space
  0x4803  compressed+condensed  entries LZ4-compressed and packed back-to-back (used by .str2_pc)

Each entry is (name, data_bytes) or (name, data_bytes, [raw_lz4_block_bytes...]) to reuse original
compressed blocks (used for byte-exact round-trip tests).
"""
import struct

MAGIC = b'\xce\x0a\x89\x51'
BLK_MAGIC = b'\xee\xdb\xee\x0f\xee\xdb\xba\x00'
NONE64 = 0xFFFFFFFFFFFFFFFF


def al(x, a=0x1000):
    return (x + a - 1) // a * a


def lz4_literal_block(data):
    """A valid LZ4 block containing only literals (no compression, always decodable)."""
    n = len(data)
    if n == 0:
        return b''
    out = bytearray()
    if n < 15:
        out.append(n << 4)
    else:
        out.append(0xF0)
        r = n - 15
        while r >= 255:
            out.append(255); r -= 255
        out.append(r)
    out += data
    return bytes(out)


def lz4_compress(data, depth=256):
    """LZ4 block compressor with hash chains (searches up to `depth` earlier positions for the
    longest match, plus one-step lazy matching). Output is standard LZ4 block format."""
    n = len(data)
    if n < 13:
        return lz4_literal_block(data)
    out = bytearray()
    chains = {}             # 4-byte key -> list of positions (most recent last)
    anchor = 0
    i = 0
    limit = n - 12          # LZ4: last match must start >= 12 bytes before end
    mflimit = n - 5         # last 5 bytes are always literals

    def best_match(pos):
        key = data[pos:pos + 4]
        lst = chains.get(key)
        bl, bc = 0, -1
        if lst:
            for cand in reversed(lst[-depth:]):
                if pos - cand > 0xFFFF:
                    break
                m = 4
                while pos + m < mflimit and data[cand + m] == data[pos + m]:
                    m += 1
                if m > bl:
                    bl, bc = m, cand
        return bl, bc

    def insert(pos):
        chains.setdefault(data[pos:pos + 4], []).append(pos)

    while i < limit:
        m, cand = best_match(i)
        if m >= 4 and i + 1 < limit:
            insert(i)
            m2, c2 = best_match(i + 1)
            if m2 > m + 1:      # lazy: a better match starts one byte later
                i += 1
                m, cand = m2, c2
        if m >= 4:
            lit = data[anchor:i]
            ll = len(lit); ml = m - 4
            tok = (min(ll, 15) << 4) | min(ml, 15)
            out.append(tok)
            if ll >= 15:
                r = ll - 15
                while r >= 255:
                    out.append(255); r -= 255
                out.append(r)
            out += lit
            out += struct.pack('<H', i - cand)
            if ml >= 15:
                r = ml - 15
                while r >= 255:
                    out.append(255); r -= 255
                out.append(r)
            end = i + m
            for p in range(i, min(end, limit)):
                if p != i or data[p:p + 4] not in chains or chains[data[p:p + 4]][-1] != p:
                    insert(p)
            i = end
            anchor = i
        else:
            insert(i)
            i += 1
    out += lz4_literal_block(data[anchor:])
    return bytes(out)


def make_blocks(data, compress=True):
    if not data:
        return []
    payload = lz4_compress(data) if compress else lz4_literal_block(data)
    return [BLK_MAGIC + struct.pack('<II', len(payload), len(data)) + payload]


def build(entries, flags):
    compressed = bool(flags & 1)
    condensed = bool(flags & 2)
    norm = []
    for e in entries:
        name, data = e[0], e[1]
        blocks = e[2] if len(e) > 2 else (make_blocks(data) if compressed else None)
        norm.append((name, data, blocks))

    names = bytearray(); name_off = []
    for name, _, _ in norm:
        name_off.append(len(names)); names += name.encode('latin1') + b'\0'
    dsz = 0x30 * len(norm)
    noff = al(0x1000 + dsz)
    doff = al(noff + len(names))

    body = bytearray(); dir_ = bytearray()
    uoff = 0
    for k, (name, data, blocks) in enumerate(norm):
        if compressed:
            if condensed:
                off = uoff
                uoff += len(data)
            else:
                off = al(uoff) if k else 0
                uoff = off + len(data)
                if body:
                    body += b'\0' * (al(len(body)) - len(body))
            if not data:
                csize = NONE64
            else:
                chunk = b''.join(blocks)
                csize = len(chunk)
                body += chunk
        else:
            # uncompressed packs: entries aligned to 0x1000 relative to the data start
            if body:
                body += b'\0' * (al(len(body)) - len(body))
            off = len(body)
            body += data
            csize = NONE64
            uoff = off + len(data)
        dir_ += struct.pack('<6Q', name_off[k], 0, off, len(data), csize, 0)

    usz = uoff if compressed else len(body)
    csz = len(body) if compressed else NONE64
    fsize = doff + len(body)
    h = bytearray(0x1000)
    h[0:4] = MAGIC
    struct.pack_into('<I', h, 4, 6)
    struct.pack_into('<I', h, 0x14C, flags)
    struct.pack_into('<6Q', h, 0x158, len(norm), fsize, dsz, len(names), usz, csz)
    out = bytearray(h)
    out += dir_; out += b'\0' * (noff - len(out))
    out += names; out += b'\0' * (doff - len(out))
    out += body
    return bytes(out)
