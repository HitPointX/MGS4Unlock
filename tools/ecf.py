#!/usr/bin/env python3
"""Decrypt / encrypt MGS4 .ecf config files.

The .ecf files in MGS4/config/ are plain INI text obfuscated with a repeating
XOR key whose index is skewed by one byte per key-length block:

    plain[i] = cipher[i] ^ KEY[(i % L + i // L) % L]      L = len(KEY)

The key itself sits in cleartext in mgs4.exe's .rdata at 0x141660d70,
immediately after the string "MGS4_Window_Ready".

The transform is an involution, so encrypting is the same operation as
decrypting. `verify` exercises that on every file it is given.

Usage:
    ecf.py decrypt <in.ecf> [-o out.ini]
    ecf.py encrypt <in.ini> [-o out.ecf]
    ecf.py verify  <file.ecf>...
    ecf.py set     <file.ecf> <Section> <Key> <Value> [-o out.ecf]
"""

import argparse
import re
import sys
from pathlib import Path

KEY = b"MGS4ConfigFileSecureKey@2024"


def transform(data: bytes, key: bytes = KEY) -> bytes:
    """XOR with the block-skewed repeating key. Self-inverse."""
    n = len(key)
    return bytes(b ^ key[(i % n + i // n) % n] for i, b in enumerate(data))


# Encryption and decryption are the same operation.
decrypt = transform
encrypt = transform


def looks_like_plaintext(data: bytes) -> bool:
    """Heuristic: decrypted config should be almost entirely printable ASCII."""
    if not data:
        return False
    printable = sum(1 for b in data if 9 <= b < 127)
    return printable / len(data) > 0.98


def set_value(plain: str, section: str, key: str, value: str) -> str:
    """Set Section/Key to value in INI text, appending the section or key if absent."""
    lines = plain.splitlines(keepends=True)
    newline = "\r\n" if "\r\n" in plain else "\n"

    sec_start = None
    sec_end = len(lines)
    for i, line in enumerate(lines):
        m = re.match(r"^\s*\[(.+?)\]", line)
        if not m:
            continue
        if m.group(1) == section:
            sec_start = i
        elif sec_start is not None:
            sec_end = i
            break

    if sec_start is None:
        return plain.rstrip("\r\n") + newline * 2 + f"[{section}]{newline}{key}={value}{newline}"

    pat = re.compile(rf"^(\s*{re.escape(key)}\s*=\s*).*?(\r?\n?)$")
    for i in range(sec_start + 1, sec_end):
        m = pat.match(lines[i])
        if m:
            lines[i] = f"{m.group(1)}{value}{m.group(2) or newline}"
            return "".join(lines)

    # Key absent: insert after the last non-blank line of the section.
    insert = sec_end
    while insert > sec_start + 1 and not lines[insert - 1].strip():
        insert -= 1
    lines.insert(insert, f"{key}={value}{newline}")
    return "".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    for name in ("decrypt", "encrypt"):
        p = sub.add_parser(name)
        p.add_argument("input", type=Path)
        p.add_argument("-o", "--output", type=Path)

    p = sub.add_parser("verify")
    p.add_argument("inputs", type=Path, nargs="+")

    p = sub.add_parser("set")
    p.add_argument("input", type=Path)
    p.add_argument("section")
    p.add_argument("key")
    p.add_argument("value")
    p.add_argument("-o", "--output", type=Path)

    args = ap.parse_args()

    if args.cmd in ("decrypt", "encrypt"):
        data = transform(args.input.read_bytes())
        if args.output:
            args.output.write_bytes(data)
            print(f"{args.input} -> {args.output} ({len(data)} bytes)")
        else:
            sys.stdout.write(data.decode("utf-8", "replace"))
        return 0

    if args.cmd == "verify":
        failed = False
        for path in args.inputs:
            raw = path.read_bytes()
            plain = decrypt(raw)
            roundtrip = encrypt(plain)
            ok_text = looks_like_plaintext(plain)
            ok_trip = roundtrip == raw
            failed |= not (ok_text and ok_trip)
            printable = sum(1 for b in plain if 9 <= b < 127) / max(len(plain), 1)
            print(
                f"{'PASS' if ok_text and ok_trip else 'FAIL'}  {path.name:28} "
                f"{len(raw):>8} B  printable={printable:.4f}  roundtrip={'exact' if ok_trip else 'MISMATCH'}"
            )
        return 1 if failed else 0

    if args.cmd == "set":
        raw = args.input.read_bytes()
        plain = decrypt(raw)
        if not looks_like_plaintext(plain):
            print(f"refusing: {args.input} did not decrypt to plausible text", file=sys.stderr)
            return 1
        updated = set_value(plain.decode("utf-8"), args.section, args.key, args.value)
        out = args.output or args.input
        out.write_bytes(encrypt(updated.encode("utf-8")))
        print(f"set [{args.section}] {args.key}={args.value} -> {out}")
        return 0

    return 1


if __name__ == "__main__":
    sys.exit(main())
