# MGS4Unlock

A framerate unlocker for *METAL GEAR SOLID 4: Guns of the Patriots, Master Collection Version*.

The game ships with a "Max Frame Rate" setting offering 30, 40 and 60. This mod
rewrites that list so 120 is selectable natively, and corrects the one engine
subsystem that does not scale correctly above 60.

Current version: **0.6a** (alpha)

## Status

| Area | State |
|---|---|
| Injection via proxy DLL | Working |
| Framerate picker showing 30 / 60 / 120 | Working, verified in game |
| 120 fps rendering | Working, verified in game |
| Gameplay, animation and audio at 120 | Correct without intervention |
| Cutscene playback at 120 | Gated to native rate, under test |
| Cloth, OctoCamo and ragdoll at 120 | Not yet formally verified |
| 240 fps | Blocked, see [Known limits](#known-limits) |

This is alpha software. It writes to another process's memory and installs code
hooks. Do not use it in any online mode.

## Requirements

- METAL GEAR SOLID 4: Guns of the Patriots, Master Collection Version (Steam app 2492670)
- A display capable of the framerate you select. Selecting 120 on a 60 Hz panel
  will not produce 120 fps.

Verified against game build `[Code]a84606af` (2026-08-25). Signatures are
version sensitive; see [Game updates](#game-updates).

## Installation

Copy `dbghelp.dll` into the directory containing `mgs4.exe`:

```
.../steamapps/common/METAL GEAR SOLID 4/MGS4/dbghelp.dll
```

`MGS4Unlock.ini` is created next to it on first launch, and the log is written
to `logs/MGS4Unlock.log`.

### Linux and Steam Deck

Wine will not load a native DLL over one of its builtins unless told to. Add
this to the game's Steam launch options:

```
WINEDLLOVERRIDES="dbghelp=n,b" %command%
```

Without it the mod loads nothing and the log file will not appear.

### Verifying it loaded

`logs/MGS4Unlock.log` should open with something like:

```
[I] MGS4Unlock v0.6a loaded, impersonating dbghelp.dll
[I] proxy: forwarding to C:\windows\system32\dbghelp.dll (9 exports resolved, 0 missing)
[I] picker: options are now {30, 60, 120}
[I] timing: cutscene playback gated to its native 60 Hz tick
```

## Configuration

`MGS4Unlock.ini`, written next to the DLL:

```ini
[Settings]
; The three rates the in-game picker should offer, replacing the stock
; 30, 40, 60. The menu formats its labels from these numbers, so this is
; literally what you will see. Keep them ascending so the menu reads in order.
PickerValues = 30, 60, 120

; Set to false to leave the picker untouched.
PatchPicker = true

; Gates cutscene playback to the engine's native 60 Hz tick. Without this,
; cutscenes run at double speed above 60 fps.
GateCutscenes = true

; Writes the decrypted .text/.rdata/.data to dump/ after the DRM stub runs.
; Only needed when developing new signatures; costs about 30 MB per launch.
DumpSections = false

; Milliseconds to wait for the Steam DRM stub to decrypt .text before giving up.
UnpackTimeoutMs = 30000
```

Every feature has its own toggle. When something misbehaves, turning off one
feature at a time is the fastest way to find out which is responsible.

## How it works

### Getting into the process

`mgs4.exe` imports nine functions from `dbghelp.dll`, all of them used only by
its crash handler. The mod ships as a DLL of that name, exporting exactly those
nine as tail jumps through pointers that are bound, on first call, to the real
system DLL loaded by absolute path. The game's own import table therefore loads
the mod before the executable's entry point runs, and no external loader is
needed.

`dbghelp` is chosen over the alternatives because nothing else in the process
depends on it. `winmm` is a smaller export surface but sits in front of the
audio path, so a proxy that got it wrong would break sound.

The impersonated DLL is a build option (`-DPROXY_TARGET=dbghelp|winmm`) rather
than being hardcoded.

### The framerate picker

The selectable rates live in a terminated array of 32-bit integers in the
executable's `.data` section. The section is not encrypted, so the array can be
read and rewritten directly.

The menu holds no label strings for these values. The localization data provides
labels for every enumerated setting (Screen Mode, V-Sync, Graphics Quality,
DirectX Version) and none for the numeric ones, which means the labels are
formatted from the integers themselves. Rewriting the array is therefore the
entire feature: the menu renders the new numbers, and the choice persists
through the game's own settings file as a plain integer.

The array is located by byte signature rather than by a fixed address, its
contents are validated for shape before anything is written, and the write is
read back to confirm it took.

### Cutscene timing

Above 60 fps, gameplay, animation and audio all behave correctly. The engine
drives them from a real frame delta, so they scale on their own.

Cutscene playback does not. It advances one whole 60 Hz tick per call, so at
120 fps it plays at double speed and then stalls periodically as it
resynchronises against the audio.

The engine already tracks how many 60 Hz ticks each frame covered. Gating the
cutscene update on that counter lets cutscenes advance at their native rate
while everything else continues to render at full speed. The framerate is not
capped, and no other subsystem is affected.

The consequence is that cutscenes animate at 60 while the game renders at 120.
The engine offers no interpolation between cutscene states, so this is inherent
to the approach.

### Working with an encrypted executable

`mgs4.exe` is wrapped by Steam's DRM stub. Its `.text` section is ciphertext on
disk and is only decrypted in memory after the process starts, which means code
signatures cannot be developed against the shipped file and the mod cannot scan
for anything until decryption has happened.

`.rdata`, `.data` and `.pdata` are not encrypted, and the exception directory in
`.pdata` gives the exact entry point of every function in the image. The mod
uses this to detect decryption: it samples known function starts and asks
whether they currently hold bytes that can legitimately begin a function.
Ciphertext scores around 0.10 by chance, real code scores above 0.97, and the
transition is unambiguous. In practice it completes about 200 ms after the mod's
worker thread starts.

## Building

The build produces a Windows x64 DLL. On Linux this is a cross compile.

### Linux

SteamOS and other immutable distributions cannot install a toolchain into the
root filesystem, so the build runs in a container:

```bash
distrobox create --name mgs4dev --image archlinux:latest
distrobox enter mgs4dev -- sudo pacman -S --noconfirm mingw-w64-gcc cmake ninja git
```

Then, from the project root:

```bash
./build.sh                # configure and build
./build.sh clean          # wipe the build directory first
./build.sh install        # build, then copy into the game directory
```

`build.sh` re-enters the container automatically if the cross compiler is not
already on `PATH`, so it can be run from either side.

Override the impersonated DLL with `PROXY_TARGET=winmm ./build.sh`.

### Windows

Requires a C++20 compiler and CMake. The `cmake/mingw-w64.cmake` toolchain file
is for cross compilation only and should be omitted.

### Dependencies

`safetyhook` is a submodule and pulls in Zydis at configure time. Clone with:

```bash
git clone --recursive <url>
```

## Repository layout

```
src/
  dllmain.cpp        Entry point, bootstraps onto a worker thread
  proxy/             Export forwarding for the impersonated DLL
  core/              Logging, config, module and section resolution,
                     pattern scanning, memory protection, .pdata parsing
  dumper/            Writes decrypted sections for offline analysis
  mgs4/              Game specific work: the picker and the timing fixes
tools/
  ecf.py             Decrypt, encrypt and edit the game's config files
  rebuild_dump.py    Splice a dump into a copy of the executable
  mksig.py           Derive a minimal unique signature for a function
docs/
  00-findings.md     What is established about the target, with addresses
cmake/               Cross compile toolchain
build.sh             Build entry point
```

## Versioning

Releases run `0.1a` through `0.6a` in alpha, `0.7b` onward in beta, and `1.0r`
at release.

| Suffix | Stage | Meaning |
|---|---|---|
| `a` | Alpha | Features are still being built. Expect breakage. |
| `b` | Beta | Feature complete for the supported framerates. Stabilising. |
| `r` | Release | Verified across a full playthrough. |

The number before the suffix increments once per meaningful chunk of work, not
per commit. Alpha runs to `0.6a`; anything from `0.7` onward is beta.

Promotion to beta requires all of:

- No unexplained crashes across a sustained session
- A completed regression pass on cloth, OctoCamo, hair and ragdoll behaviour
- Packaging and an install script

See [CHANGELOG.md](CHANGELOG.md) for what landed in each version.

## Known limits

**240 fps is not currently reachable.** Two separate obstacles:

1. The engine holds a maximum framerate cap of 128 in the same structure as the
   picker's option list. Values above it are unlikely to be accepted without
   further work.
2. Character control is driven by a 300 Hz tick counter. 240 fps gives 1.25
   ticks per frame, which is workable, but above 300 fps the count drops below
   one tick per frame and the model breaks down entirely. 300 fps is a hard
   ceiling regardless of anything else.

**Cutscenes animate at 60** while rendering at the selected rate, as described
above.

**One unexplained crash** was seen during testing at 120 fps. It has not been
reproduced or attributed. If you hit one, launching with `PROTON_LOG=1` on Linux
will capture detail.

## Game updates

Signatures are tied to the executable's code layout, and the picker's array is
found by content. A game update will likely invalidate the code signatures.

Failures are designed to be loud rather than silent. Every lookup logs whether
it found exactly one match, the array's contents are validated before any write,
and a signature that matches in the wrong place is caught by re-validating the
instruction it points at. If an update breaks something, the log will say which
lookup failed rather than the mod misbehaving quietly.

## License

Not yet chosen.
