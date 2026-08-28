# MGS4: verified findings

Target: `METAL GEAR SOLID 4: Guns of the Patriots, Master Collection Version`, Steam AppID 2492670.
Build `Pela_[MPA]_x64_BGFX_0.0.3_Release_ww_[Code]a84606af_[DataNew]d06ab525_2026_0825`,
`mgs4.exe` md5 `48e656dae7fb7e85ec162d25aa7a311f`.

Everything below was confirmed first-hand, either by static analysis of the shipped
binary or by a logged run in-game. Addresses are given against the preferred image
base `0x140000000`; the loader does apply ASLR (observed base `0x6fffd13f0000`), so
all runtime use goes through an RVA rebase.

## Binary layout

Steam DRM wraps the executable. The entry point is `0x164184310`, inside a `.bind`
section, and **`.text` on disk is ciphertext**: zero standard prologues, zero `int3`
padding, entropy 8.0000 bits/byte.

**`.rdata`, `.data` and `.pdata` are not encrypted.** This is the single most useful
fact about the target: strings, constant pools, data tables and the entire exception
directory can be read statically, and the only thing that needs a runtime dump is
code.

| Section | VA | Size | State |
|---|---|---|---|
| `.text` | `0x140001000` | `0x165cf80` | encrypted on disk |
| `.rdata` | `0x14165e000` | `0x4a1002` | plaintext |
| `.data` | `0x141b00000` | `0x26d200` raw | plaintext (virtual size ~575 MB, mostly BSS) |
| `.pdata` | `0x163ffc000` | `0x109614` | plaintext |
| `.bind` | `0x164184000` | `0x39248` | Steam stub |

`.pdata` holds **90,583** `RUNTIME_FUNCTION` records (first RVA `0x1000`, last
`0x165df60`), with `UNWIND_INFO` in plaintext `.rdata`.

## The framerate control struct

At `.data:0x141b08de8` (RVA `0x1b08de8`), found by unique byte signature:

```
0x141b08de8   int32[3]  { 30, 40, 60 }   selectable rates
0x141b08df4   int32     -1               cached target fps, -1 until resolved
0x141b08df8   int32     1                flag
0x141b08dfc   int32     0
0x141b08e00   int32     128              unrelated to fps, see below
```

The three functions that matter, all in one cluster:

| Address | Role |
|---|---|
| `0x140140a50` | `mov eax,3 / ret`. Returns the option count. One caller. |
| `0x140140a60` | Resolves and memoizes the target framerate into `0x141b08df4`. |
| `0x140140c70` | Copies the three options into a caller-supplied buffer. Unrolled, no loop. One caller. |
| `0x1414d95a0` | Menu populate. Calls all of the above. |

**`128` is not an fps cap.** It is a platform identifier. `0x140140a60` calls a
platform query and dispatches on the result (`0x80`, `1`, `8`, `0x38`) to choose
which `FPSLimiter.FPS_*` config variable to read:

```
mov  eax, [0x141b08df4]      ; cached target
cmp  eax, -1
jne  <return it unchanged>   ; already resolved
call <platform id>
cmp  eax, 0x80 / 1 / 8 / 0x38
lea  rcx, ["FPSLimiter.FPS_PC"]
call <cvar lookup>
test al, al
jne  <mov eax, 0x3c>         ; lookup failed: fall back to 60
...
mov  [0x141b08df4], eax      ; memoize
```

**The player's saved choice is never read back into this path.** The game writes
`fpsLimiter` to its settings file on quit, but on the next launch this function
recomputes from the config variable, the lookup fails, and it settles on 60. That
is why a rate above the stock list did not survive a relaunch.

Because the function returns any non-`-1` cached value untouched, writing the
wanted rate into `0x141b08df4` before the game first asks is sufficient, and
needs no hook.

**The menu has no numeric label strings.** The binary contains no `"30"`/`"40"`/`"60"`
cluster, and `common/localization/lang/lang_en` supplies value labels for every
*enumerated* option (Screen Mode, V-Sync, Quality, DirectX Version) and for none of
the numeric ones. `"Max Frame Rate"` is localization id `0xCCD903`. The labels are
therefore formatted from the integers above. **Rewriting the array changes what the
player sees**, which is confirmed in-game.

The picker persists as a literal integer, not an index, to
`mgs4_savedata_win/<steamid>/mgs4/mgs4.savedsettings` (plain text, CRLF) as
`fpsLimiter=<n>`.

## Config files: `.ecf` is an obfuscated INI, and the cipher is broken

The key is in cleartext in `.rdata` at `0x141660d70`:

```
MGS4ConfigFileSecureKey@2024
```

The transform is XOR with a repeating key whose index is skewed one byte per
key-length block, and it is an involution (encrypt == decrypt):

```
plain[i] = cipher[i] ^ KEY[(i % L + i / L) % L]        L = 28
```

All five shipped `.ecf` files decrypt to 100% printable text and re-encrypt
byte-identically. `tools/ecf.py` implements this with a `verify` subcommand.

> A single flat repeating XOR decrypts only the first 28 bytes and then desynchronises,
> which is why the file can look like a stream cipher on a first pass. It is not.

Recovered contents of note:

- `mgs4.scalability_PC.ecf` → `[FPSLimiter] FPS_PC=60` (plus `FPS_XSX/XSS/PS5/NX`)
- `mgs4.ecf` → `[render] vsync/fullscreen/api/…`, `[scalability]`, `[gameplay]`
- `mgs4.dev.ecf` → `[debug] showImGuiOnLog`, `enableMouseCursor`, `skipSimForPauseMenu`;
  `[cheat] invincible`, `oneHitKill`, `invisibility`, `silence`, `infiniteAmmo`;
  `[fastLoad] stage`
- `mgs4.steam.ecf` → `[steam] appID = 2492670`

Config load order, from `.rdata:0x141660d90`. Note the user override is checked first:

```
config\mgs4.user.ini
config\mgs4.ini
config\mgs4.steam.ini
config\mgs4.input.ini
```

Real cvar namespaces: `render.*` (19 keys), `FPSLimiter.FPS_{PC,XBS,XSX,XSS,PS5,NX}`,
`scalability.scalabilityLevel_{PC,SteamDeck,XSX,XBS,XSS,PS5,NX}`, `engine.*`,
`gameplay.*`, `debug.enableMouseCursor`.

The engine also ships Dear ImGui (`"Dear ImGui Metrics/Debugger"` in `.rdata`), and
`.rdata:0x141678498` already enumerates `30fps` / `60fps` / `120fps` alongside
`Budget frame time (ms)` and `GPU Headroom (ms)`.

## Injection

`mgs4.exe` imports `dbghelp.dll` directly and uses exactly nine functions, all
crash-handler-only: `StackWalk64`, `SymFunctionTableAccess64`, `SymGetModuleInfo64`,
`MiniDumpWriteDump`, `SymGetSymFromAddr64`, `SymInitialize`, `SymGetLineFromAddr64`,
`SymCleanup`, `SymGetModuleBase64`.

That makes it the best proxy anchor: statically imported so we load before the Steam
stub runs, and a small blast radius if forwarding ever fails. `winmm` (2 imports) is
smaller but sits in front of Wine's audio path and is also imported by `bink2w64.dll`.

Nothing in the import graph pulls in `winhttp` or `wininet`, so a proxy built on
either of those names would never be loaded under Proton.

Confirmed working: `WINEDLLOVERRIDES="dbghelp=n,b"`, 9/9 exports forwarded to
`C:\windows\system32\dbghelp.dll`.

## Detecting the DRM unpack

Sampling `.text` for `int3` padding does **not** work. Even on fully decrypted code
a strided 4-byte `0xCC` probe scores 0.0000.

What does work is anchoring on `.pdata`: sample known function starts and ask whether
the byte there can legitimately begin an MSVC x86-64 function. Measured in-game:

| State | Prologue agreement |
|---|---|
| encrypted | 0.101 |
| decrypted | 0.977 |

The transition happens ~200 ms after our worker thread starts. The same anchoring
makes signature scanning ~250× cheaper and structurally unable to match inside
another function's body.

## Environment

Desktop (ASRock B850M-X, Ryzen 9 9950X), SteamOS 3.8.25 holo, read-only root.
Game runs under GE-Proton11-5. Primary display DP-3 at 3840x2160 **@ 239.89 Hz**,
secondary HDMI-A-1 @ 59.96 Hz, so both 120 and 240 are observable here.

Toolchain lives in an Arch distrobox (`mgs4dev`): mingw-w64 g++ 16.2.0, cmake, ninja.

## Timing above 60 fps

Most of the game scales on its own: gameplay, audio and general animation are
driven from a real frame delta and behave correctly at 120 and 240 untouched.
Four systems do not, each for a different reason.

**Character control** advances in whole ticks of a 300 Hz counter. 300 divides
evenly by 60, so the shipping framerate loses nothing, but 120 gives 2.5 ticks
per frame and 240 gives 1.25, and the fraction is discarded. Animation runs
slow: subtle at 120, roughly four fifths speed at 240. Carrying the remainder
between frames fixes it. This also sets the ceiling for the whole approach:
above 300 fps a frame is worth less than one tick and the model collapses.

**Cutscene playback** advances a whole 60 Hz tick per call, so it plays at
double speed and stalls to resynchronise against audio. Gating it to native
ticks is correct here.

**Simulation task timing** is centralised in one function that converts the
frame delta into a step size and substep count (it multiplies by 59.94 to
express the frame in 60 Hz ticks). Every simulation task consults it. Feeding it
the real frame delta, rather than a step sized for 60 Hz, is what fixes the
scarf and general simulation.

**Cloth** must be simulated every frame using that same real delta. The
intuitive fix -- keep cloth's native fixed step and run the solver only on
60 Hz frames -- was tried extensively and always produced a doubled,
semi-transparent copy of the garment, regardless of what the skipped frame did
(publish, don't publish, which exit to take, what state to restore). A garment
whose solver runs on only some frames ends up inconsistent with the parts of
the pipeline that run on all of them. The stiff solver tolerates a shorter step
better than it tolerates that mismatch.

The general lesson: gating is the right tool for a system that owns its own
timeline end to end (cutscenes), and the wrong tool for one that is part of a
per-frame pipeline (cloth).

## Status

- Picker offers 30 / 60 / 120, selectable and persisted. **Confirmed in-game.**
- Selecting 120 renders at 120 fps. **Confirmed in-game.**
- Cutscene timing is gated to the native 60 Hz tick as of 0.6a.
- The picker offers 30 / 60 / 120 / 240. The stock count of three came from a
  six-byte helper returning 3 and an unrolled filler; the menu's own buffer is
  sized for 30 entries, so raising it is a count patch plus a filler hook.
- The saved choice is restored at startup by seeding the cached target, so a rate
  above the stock list now survives a relaunch. A separate clamp in the
  config-apply path snaps anything above 60 back down and has to be bypassed,
  and at least one other code path writes the target directly, so it is also
  reasserted on a timer.
- Cloth, cutscenes, character animation and the scarf are all correct at 120 and
  240. Confirmed in game.
- 240 fps works. The earlier reading of `128` as a maximum was wrong; it is a
  platform identifier. The real ceiling is the 300 Hz character tick, so 300 fps
  is the hard limit for this approach.
- Crashes have been observed in several distinct signatures, one of which
  recurred before any timing work existed. Only one has been root-caused (a
  callee-saved register clobbered by a mid-hook redirect). Stability is not yet
  characterised.
