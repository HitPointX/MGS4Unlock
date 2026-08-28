#!/usr/bin/env python3
"""Splice dumped section contents into a copy of mgs4.exe.

mgs4.exe ships with .text encrypted by the Steam DRM stub, so the file on disk
cannot be disassembled. The in-game dumper writes out the decrypted section
contents; this script drops them back into a copy of the shipped executable at
the matching file offsets.

Every field other than the section bytes is already correct in the shipped file,
so the result is a genuine, fully valid PE that objdump, Ghidra and IDA will
load at the right addresses -- with no header surgery required.

Usage:
    rebuild_dump.py <dump-dir> [-o mgs4_decrypted.exe] [--exe path/to/mgs4.exe]
"""

import argparse
import struct
import sys
from pathlib import Path

DEFAULT_EXE = Path(
    "/home/hit/.local/share/Steam/steamapps/common/METAL GEAR SOLID 4/MGS4/mgs4.exe"
)


def read_sections(exe: bytes):
    """Yields (name, virtual_address, virtual_size, raw_offset, raw_size)."""
    e_lfanew = struct.unpack_from("<I", exe, 0x3C)[0]
    n_sections = struct.unpack_from("<H", exe, e_lfanew + 6)[0]
    opt_size = struct.unpack_from("<H", exe, e_lfanew + 20)[0]
    table = e_lfanew + 24 + opt_size

    for i in range(n_sections):
        off = table + i * 40
        name = exe[off : off + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", exe, off + 8)
        yield name, vaddr, vsize, raddr, rsize


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump_dir", type=Path)
    ap.add_argument("-o", "--output", type=Path, default=Path("mgs4_decrypted.exe"))
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    args = ap.parse_args()

    if not args.exe.is_file():
        print(f"error: no such executable: {args.exe}", file=sys.stderr)
        return 1

    image = bytearray(args.exe.read_bytes())
    sections = {name: info for name, *info in ((n, v, vs, r, rs) for n, v, vs, r, rs in read_sections(image))}

    manifest = args.dump_dir / "manifest.txt"
    if manifest.is_file():
        print(f"manifest:\n{manifest.read_text().strip()}\n")

    spliced = 0
    for name, blob in ((".text", "text.bin"), (".rdata", "rdata.bin"), (".data", "data.bin")):
        path = args.dump_dir / blob
        if not path.is_file():
            print(f"  skip {name}: {blob} not present")
            continue
        if name not in sections:
            print(f"  skip {name}: not in the section table")
            continue

        vaddr, vsize, raddr, rsize = sections[name]
        payload = path.read_bytes()

        # The dump holds VirtualSize bytes; the file only has room for
        # SizeOfRawData. Writing the overlap is enough -- the remainder is BSS.
        count = min(len(payload), rsize)
        image[raddr : raddr + count] = payload[:count]
        spliced += 1

        status = "" if count == len(payload) else f" (truncated from {len(payload)})"
        print(f"  {name:8} -> file offset {raddr:#x}, {count:#x} bytes{status}")

    if not spliced:
        print("error: nothing was spliced", file=sys.stderr)
        return 1

    args.output.write_bytes(image)
    print(f"\nwrote {args.output} ({len(image)} bytes)")
    print(f"disassemble with:  objdump -d --start-address=0x140001000 {args.output} | less")
    return 0


if __name__ == "__main__":
    sys.exit(main())
