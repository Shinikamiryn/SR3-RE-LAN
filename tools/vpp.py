"""Read-only parser / integrity checker for Saints Row: The Third Remastered .vpp_pc (packfile v6, 64-bit fields)."""
import sys, os, struct, zlib

MAGIC = b'\xce\x0a\x89\x51'

def al(x, a=0x1000):
    return (x + a - 1) // a * a

def parse(p):
    f = open(p, 'rb')
    h = f.read(0x1000)
    if h[:4] != MAGIC:
        raise ValueError('bad magic')
    ver = struct.unpack_from('<I', h, 4)[0]
    flags = struct.unpack_from('<I', h, 0x14C)[0]
    n, fsize, dsz, nsz, usz, csz = struct.unpack_from('<6Q', h, 0x158)
    f.seek(0x1000); d = f.read(dsz)
    noff = al(0x1000 + dsz); f.seek(noff); names = f.read(nsz)
    doff = al(noff + nsz)
    ents = []
    for i in range(n):
        no, po, do, us, cs, fl = struct.unpack_from('<6Q', d, i * 0x30)
        e = names.find(b'\0', no)
        nm = names[no:(e if e >= 0 else len(names))].decode('latin1')
        ents.append(dict(name=nm, off=do, size=us, csize=cs, flags=fl, path_off=po))
    return dict(path=p, ver=ver, flags=flags, n=n, fsize=fsize, dsz=dsz, nsz=nsz,
                usz=usz, csz=csz, doff=doff, ents=ents)

def check(r, deep=False):
    probs = []
    real = os.path.getsize(r['path'])
    if r['fsize'] != real:
        probs.append(f"header size {r['fsize']} != file size {real}")
    compressed = bool(r['flags'] & 1)
    condensed = bool(r['flags'] & 2)
    seen = set()
    for e in r['ents']:
        if not e['name']:
            probs.append('empty filename')
        k = e['name'].lower()
        if k in seen:
            probs.append(f"duplicate entry {e['name']}")
        seen.add(k)
    if not compressed:
        for e in r['ents']:
            end = r['doff'] + e['off'] + e['size']
            if end > real:
                probs.append(f"{e['name']} runs past EOF ({end} > {real})")
    elif r['n']:
        if r['doff'] + r['csz'] > real:
            probs.append(f"compressed data runs past EOF")
        f = open(r['path'], 'rb')
        for e, pos, blocks, err in walk_compressed(r, f):
            if err:
                probs.append(f"{e['name']}: {err}")
                break
            if sum(b[1] for b in blocks) != e['size']:
                probs.append(f"{e['name']}: block usize sum {sum(b[1] for b in blocks)} != {e['size']}")
            if deep:
                try:
                    out = read_entry_blocks(f, blocks)
                    if len(out) != e['size']:
                        probs.append(f"{e['name']}: decoded {len(out)} != {e['size']}")
                except Exception as ex:
                    probs.append(f"{e['name']}: LZ4 decode error {ex}")
    return probs

BLK_MAGIC = b'\xee\xdb\xee\x0f\xee\xdb\xba\x00'

def walk_compressed(r, f):
    """Yield (entry, start, [(file_pos_of_payload, usize, csize)], error) for LZ4 packs."""
    pos = r['doff']
    for e in r['ents']:
        if e['size'] == 0 or e['csize'] == 0xFFFFFFFFFFFFFFFF:
            if e['size'] != 0:
                yield e, pos, [], f"stored entry with no compressed size (unhandled)"; return
            yield e, pos, [], None
            continue
        f.seek(pos); hdr = f.read(16)
        if hdr[:8] != BLK_MAGIC:
            # tolerate alignment padding between entries
            f.seek(pos); look = f.read(0x20000)
            k = len(look) - len(look.lstrip(b'\0'))  # skip zero padding only
            if look[k:k + 8] != BLK_MAGIC:
                yield e, pos, [], f"no LZ4 block header near {pos:#x}"; return
            pos += k
        start = pos; remain = e['csize']; blocks = []
        while remain > 0:
            f.seek(pos); hdr = f.read(16)
            if hdr[:8] != BLK_MAGIC:
                yield e, start, blocks, f"bad block header at {pos:#x}"; return
            c, u = struct.unpack_from('<II', hdr, 8)
            blocks.append((pos + 16, u, c))
            pos += 16 + c; remain -= 16 + c
        if remain != 0:
            yield e, start, blocks, f"block sizes overrun entry csize by {-remain}"; return
        yield e, start, blocks, None

def lz4_block_decode(src, usize):
    dst = bytearray(); i = 0; n = len(src)
    while i < n:
        tok = src[i]; i += 1
        ll = tok >> 4
        if ll == 15:
            while True:
                b = src[i]; i += 1; ll += b
                if b != 255: break
        dst += src[i:i + ll]; i += ll
        if i >= n: break
        off = src[i] | (src[i + 1] << 8); i += 2
        if off == 0 or off > len(dst): raise ValueError(f'bad match offset {off}')
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255: break
        ml += 4
        s = len(dst) - off
        if off >= ml:
            dst += dst[s:s + ml]
        else:
            # overlapping match = repeat the last `off` bytes; build it with slice multiplication
            pat = bytes(dst[s:])
            dst += (pat * (ml // off + 1))[:ml]
    if len(dst) != usize: raise ValueError(f'decoded {len(dst)} expected {usize}')
    return bytes(dst)

def read_entry_blocks(f, blocks):
    out = bytearray()
    for p, u, c in blocks:
        f.seek(p); out += lz4_block_decode(f.read(c), u)
    return bytes(out)

def extract(r, outdir, pattern=None):
    import fnmatch
    os.makedirs(outdir, exist_ok=True)
    f = open(r['path'], 'rb')
    want = lambda n: pattern is None or fnmatch.fnmatch(n.lower(), pattern.lower())
    if r['flags'] & 1:
        for e, pos, blocks, err in walk_compressed(r, f):
            if err: raise ValueError(err)
            if want(e['name']):
                open(os.path.join(outdir, e['name']), 'wb').write(read_entry_blocks(f, blocks))
    else:
        for e in r['ents']:
            if want(e['name']):
                f.seek(r['doff'] + e['off'])
                open(os.path.join(outdir, e['name']), 'wb').write(f.read(e['size']))

if __name__ == '__main__':
    deep = '--deep' in sys.argv
    dump = '--list' in sys.argv
    for p in [a for a in sys.argv[1:] if not a.startswith('--')]:
        try:
            r = parse(p)
            probs = check(r, deep)
        except Exception as ex:
            print(f"{os.path.basename(p):30} PARSE ERROR {ex!r}"); continue
        print(f"{os.path.basename(p):30} v{r['ver']} flags={r['flags']:#06x} files={r['n']:5} "
              f"{'OK' if not probs else 'PROBLEMS: ' + '; '.join(probs[:4]) + (f' (+{len(probs)-4} more)' if len(probs) > 4 else '')}", flush=True)
        if dump:
            for e in r['ents']:
                print(f"    {e['name']}\t{e['size']}\t{e['csize']}")
