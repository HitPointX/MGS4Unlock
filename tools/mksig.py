#!/usr/bin/env python3
"""Derive a minimal unique byte signature for a function in the decrypted image.

Takes the bytes at a function's entry point, wildcards every operand that moves
when the binary is relinked (RIP-relative displacements, rel32 branch targets),
and then reports the shortest prefix that still matches exactly once across the
whole of .text.

Run it against the output of rebuild_dump.py:

    mksig.py work/mgs4_decrypted.exe 0x14064bb70
"""

import argparse
import re
import struct
import sys

TEXT_OFF = 0x400
TEXT_VA = 0x140001000
TEXT_SIZE = 0x165CF80


def wildcard_operands(image: bytes, va: int, length: int):
    """Returns a list of byte-or-None, using Zydis-free heuristics.

    Only the operand forms that actually appear in MSVC function prologues are
    handled; anything unrecognised is kept literal, which is the safe direction
    (an over-specific signature fails loudly rather than matching the wrong
    function).
    """
    off = TEXT_OFF + (va - TEXT_VA)
    raw = image[off : off + length]
    out: list = list(raw)

    def blank(start: int, count: int) -> None:
        for k in range(start, min(start + count, len(out))):
            out[k] = None

    def rip_displacement_at(i: int):
        """If a RIP-relative operand starts at i, returns its disp32 offset."""
        j = i
        if 0x40 <= raw[j] <= 0x4F:  # optional REX prefix
            j += 1
        if j >= len(raw):
            return None

        if raw[j] in (0xF2, 0xF3) and j + 1 < len(raw) and raw[j + 1] == 0x0F:
            j += 3  # SSE: prefix + 0F + opcode
        elif raw[j] == 0x0F:
            j += 2  # two-byte opcode
        else:
            j += 1  # one-byte opcode

        # modrm.mod == 00 and modrm.rm == 101 means [rip + disp32]
        if j < len(raw) and (raw[j] & 0xC7) == 0x05:
            return j + 1
        return None

    i = 0
    while i < len(raw):
        b = raw[i]
        nxt = raw[i + 1] if i + 1 < len(raw) else 0

        if b in (0xE8, 0xE9):  # call/jmp rel32
            blank(i + 1, 4)
            i += 5
        elif b == 0x0F and 0x80 <= nxt <= 0x8F:  # jcc rel32
            blank(i + 2, 4)
            i += 6
        elif (disp := rip_displacement_at(i)) is not None:
            blank(disp, 4)
            i = disp + 4
        else:
            i += 1

    return out


def render(sig) -> str:
    return " ".join("??" if b is None else f"{b:02X}" for b in sig)


def count_matches(image: bytes, sig) -> int:
    pattern = b"".join(b"." if b is None else re.escape(bytes([b])) for b in sig)
    text = image[TEXT_OFF : TEXT_OFF + TEXT_SIZE]
    return len(re.findall(pattern, text, re.S))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("va", type=lambda s: int(s, 0))
    ap.add_argument("--max", type=int, default=64, help="bytes to consider")
    ap.add_argument("--min", type=int, default=8, help="shortest prefix to try")
    args = ap.parse_args()

    image = open(args.image, "rb").read()
    full = wildcard_operands(image, args.va, args.max)

    for length in range(args.min, len(full) + 1):
        sig = full[:length]
        # A signature must not end on a wildcard -- it adds length without
        # adding selectivity.
        while sig and sig[-1] is None:
            sig = sig[:-1]
        if not sig:
            continue
        n = count_matches(image, sig)
        if n == 1:
            print(f"unique at {length} bytes:\n{render(sig)}")
            return 0

    print(f"no unique signature within {args.max} bytes "
          f"(best match count {count_matches(image, full)})", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
