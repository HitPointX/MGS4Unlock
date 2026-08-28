# MGS4Unlock

A framerate unlocker for *METAL GEAR SOLID 4: Guns of the Patriots, Master Collection Version*.

The game ships with a "Max Frame Rate" setting offering 30, 40 and 60. This mod
replaces that list with 30, 60, 120 and 240, makes the choice stick, and
corrects the engine systems that do not scale correctly above 60.

Current version: **0.7c** (beta)

## Status

| Area | State |
|---|---|
| Injection via proxy DLL | Working |
| Picker showing 30 / 60 / 120 / 240 | Working, verified in game |
| Selection persists across relaunches | Working, verified in game |
| Changing the selection without a relaunch | Working, verified in game |
| 120 and 240 fps rendering | Working, verified in game |
| Gameplay, animation and audio | Correct at 120 and 240 |
| Character movement and animation speed | Corrected, verified at 240 |
| Cutscene playback | Corrected, verified in game |
| Cloth, including the headdress and scarf | Corrected, verified at 120 and 240 |
| Snake's bandana | Corrected, verified at 120 and 240 |
| Stability | No crash seen in normal gameplay, testing ongoing |

This is beta software. It writes to another process's memory and installs code
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

### Timing above 60 fps

Most of the game scales on its own. Gameplay, audio and general animation are
driven from a real frame delta and behave correctly at 120 and 240 with no
intervention. Four systems do not, and each needs different treatment.

**Character control** advances in whole ticks of a 300 Hz counter. 300 divides
evenly by 60, so the framerate the game shipped with loses nothing, but 120
gives 2.5 ticks per frame and 240 gives 1.25, and the fractional part was
discarded every frame. Animation ran slow, subtly at 120 and at roughly four
fifths speed at 240. The remainder is now carried between frames, so the counts
stay whole while their long-run average matches elapsed time.

**Cutscene playback** advances a whole 60 Hz tick per call, so above 60 it plays
at double speed and then stalls to resynchronise against the audio. It is gated
to run only on native 60 Hz ticks. The framerate is not capped; only that one
system is rate-limited.

**Simulation task timing** is centralised. One function converts frame time into
a step size and a substep count, and every simulation task consults it. It now
receives the real frame delta rather than a step sized for 60 Hz.

**Cloth** is simulated every frame using that same real delta.

**Snake's bandana** runs through a separate hair solver that is stiff and
integrates gravity per step. Given a shorter step, gravity scales down while the
chain constraints keep their stiffness, so the chain never settles and the
bandana floats above his head. It gets a fixed step near 1/60 instead.

That last one is worth explaining, because the intuitive approach is wrong.
Gating cloth to 60 Hz, the way cutscenes are gated, does fix the sway rate, but
it leaves a doubled, semi-transparent copy of the garment on screen. A garment
whose solver runs on only half the frames ends up inconsistent with the parts of
the pipeline that run on all of them, and no amount of adjusting what happens on
the skipped frame changes that. Simulating every frame removes the skipped frame
entirely. The solver is stiff and was tuned for a fixed step, but it tolerates a
shorter step far better than it tolerates the mismatch.

The general rule: gating suits a system that owns its own timeline end to end,
and breaks a system that is one stage of a per-frame pipeline.

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

Releases run `0.1a` through `0.7a` in alpha, `0.7b` onward in beta, and `1.0r`
at release.

| Suffix | Stage | Meaning |
|---|---|---|
| `a` | Alpha | Features are still being built. Expect breakage. |
| `b` | Beta | Feature complete for the supported framerates. Stabilising. |
| `r` | Release | Verified across a full playthrough. |

The number before the suffix increments once per meaningful chunk of work, not
per commit.

Promotion to beta requires all of:

- No unexplained crashes across a sustained session
- A completed regression pass on cloth, OctoCamo, hair and ragdoll behaviour
- Packaging and an install script

See [CHANGELOG.md](CHANGELOG.md) for what landed in each version.

## Known limits

**300 fps is the hard ceiling.** Character control advances in whole ticks of a
300 Hz counter, so above 300 fps a frame is worth less than a single tick and
the character update would have to be called with nothing to advance. 240 sits
comfortably under this at 1.25 ticks per frame. There is no path past 300
without a different model.

An earlier version of these notes described a value of 128 near the picker's
option table as a maximum framerate cap. That was wrong: it is a platform
identifier used to select which per-platform config value to read. No cap stands
in the way of 240.

**Cutscenes animate at 60** while the game renders at the selected rate. The
engine offers no interpolation between cutscene states, so this is inherent to
gating that system.

**Stability has had limited testing.** No crash has been seen in normal
gameplay so far, across most of Act 1 and Act 2 at 120 and 240.

One real crash was found and fixed during development: a mid-function redirect
left a callee-saved register holding a stale value, which surfaced as a fault
over a megabyte away in unrelated code, because the bad value propagated
outward until something used it as a pointer.

Most of the other crash dumps produced during development came from replacing
the DLL while the game was still running, which is a development habit rather
than a fault in the mod. If you are iterating on a build, close the game first.

If you do hit a crash, the game writes a dump under `MGS4/crash_dumps/`, and
launching with `PROTON_LOG=1` on Linux captures more detail.

## Game updates

Signatures are tied to the executable's code layout, and the picker's array is
found by content. A game update will likely invalidate the code signatures.

Failures are designed to be loud rather than silent. Every lookup logs whether
it found exactly one match, the array's contents are validated before any write,
and a signature that matches in the wrong place is caught by re-validating the
instruction it points at. If an update breaks something, the log will say which
lookup failed rather than the mod misbehaving quietly.

## License

MIT. See [LICENSE](LICENSE).
