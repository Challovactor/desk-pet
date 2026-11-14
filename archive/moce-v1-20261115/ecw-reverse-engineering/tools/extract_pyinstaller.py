import struct, sys, zlib, pathlib

MAGIC = b'MEI\x0c\x0b\x0a\x0b\x0e'

def main(exe_path, out_dir):
    blob = pathlib.Path(exe_path).read_bytes()
    pos = blob.rfind(MAGIC)
    if pos < 0:
        raise SystemExit('cookie magic not found')
    cookie = blob[pos:pos+88]
    magic, pkg_len, toc_off, toc_len, pyver, pylib = struct.unpack('!8sIIII64s', cookie)
    print('cookie:', magic, 'pkg_len', pkg_len, 'toc_off', toc_off, 'toc_len', toc_len, 'pyver', pyver, 'pylib', pylib.rstrip(b'\0'))
    out = pathlib.Path(out_dir); out.mkdir(parents=True, exist_ok=True)
    pkg_start = len(blob) - pkg_len
    end = pkg_start + toc_off + toc_len
    p = pkg_start + toc_off
    entries = []
    while p < end:
        (entry_len,) = struct.unpack('!i', blob[p:p+4])
        if entry_len <= 0 or p + entry_len > end:
            print('entry parse stop at', p, 'len', entry_len); break
        body = blob[p+4:p+entry_len]
        # classic: offset(4) comp(4) uncom(4) flag(1) typecode(1) name
        if len(body) >= 18:
            off, comp_len, uncom_len = struct.unpack('!III', body[:12])
            flag = body[12:13]
            tc = body[13:14]
            name = body[14:].rstrip(b'\0').decode('utf-8', 'replace')
        else:
            print('short entry', entry_len); break
        entries.append((name, off, comp_len, uncom_len, flag, tc, entry_len))
        p += entry_len
    print('entries:', len(entries))
    for name, off, comp_len, uncom_len, flag, tc, entry_len in entries:
        data = blob[pkg_start+off:pkg_start+off+comp_len]
        if flag and flag != b'\0':
            try: data = zlib.decompress(data)
            except Exception as e: print('inflate fail', name, e)
        safe = name.replace('/', '_').replace('\\', '_')
        suffix = '.pyc' if tc in (b's', b'm', b'M') else ''
        (out / (safe + suffix)).write_bytes(data)
        print(f'{tc!r} {flag!r} {name} -> {len(data)} bytes')
    print('done', out)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
