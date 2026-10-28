#!/usr/bin/env python3
"""Lists or extracts files from a Descent 1 HOG archive ("DHF", then per
file a 13-byte NUL-padded name, a 32-bit little-endian length, the data).

Usage: hogx.py HOG [OUTDIR [NAME...]]   no OUTDIR: list; NAMEs: only those
(case-insensitive, shell-style patterns like '*.hmp')."""
import fnmatch, os, struct, sys

def entries(path):
    with open(path, "rb") as f:
        if f.read(3) != b"DHF":
            sys.exit(f"{path}: not a HOG file")
        while True:
            head = f.read(17)
            if len(head) < 17:
                return
            name = head[:13].split(b"\0", 1)[0].decode("latin-1")
            (size,) = struct.unpack("<I", head[13:])
            yield name, f.read(size)

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    pats = [p.lower() for p in sys.argv[3:]] or ["*"]
    for name, data in entries(sys.argv[1]):
        if not any(fnmatch.fnmatch(name.lower(), p) for p in pats):
            continue
        if len(sys.argv) < 3:
            print(f"{len(data):8d}  {name}")
        else:
            os.makedirs(sys.argv[2], exist_ok=True)
            with open(os.path.join(sys.argv[2], name.lower()), "wb") as out:
                out.write(data)

main()
