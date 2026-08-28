# Changelog

Versions run `0.1a` through `0.7a` in alpha, `0.7b` onward in beta, and `1.0r`
at release. Each version covers a chunk of work rather than a single commit.

All work targets game build `[Code]a84606af` (2026-08-25),
`mgs4.exe` md5 `48e656dae7fb7e85ec162d25aa7a311f`.

---

## 0.7c, hair solver

- Fixed Snake's bandana floating above his head instead of draping, at any
  framerate above 60. Visible for most of the game.

  The bandana runs through a hair solver, separate from the cloth solver, and it
  was receiving the real frame delta along with every other simulation task.
  That solver integrates gravity per step while its chain constraints keep their
  stiffness regardless, so a shorter step scales gravity down and the chain never
  settles. It now gets a fixed step near 1/60 whatever the framerate, and a
  single step so the engine does not compensate by running several. Switchable
  via `HairFixedStep`.

  Measured afterwards at 240: the engine already calls the hair solver at around
  76 times a second rather than once per frame, so the fixed step gives the right
  speed without also needing to be rate-limited.

- Removed the remains of an abandoned approach to cloth timing, and a duplicate
  hook on the hair solver left over from when it was only being observed.

Note on the crash dumps produced during development: most were caused by
replacing the DLL while the game was still running, not by the mod. One was a
genuine bug and is fixed, in 0.7b. No crash has been seen in normal gameplay.

This completes the timing work. Three systems needed three different treatments,
which is worth recording because a single approach applied to all of them does
not work:

| System | Treatment | Reason |
|---|---|---|
| Cutscenes | Gate to native 60 Hz | Owns its own timeline end to end |
| Cloth | Simulate every frame on the real delta | One stage of a per-frame pipeline |
| Hair | Fixed step near 1/60, every call | Stiff solver, unstable on a short step |

---

## 0.7b, cloth and simulation timing

First beta. Everything the mod sets out to correct is now correct at both 120
and 240, confirmed in game.

- Corrected the engine's shared simulation task timing. All simulation work is
  scheduled through one function that converts frame time into a step size and a
  substep count; it now receives the real frame delta rather than a step sized
  for 60 Hz. This alone fixed the headdress and scarf.
- Cloth is now simulated every frame using that same real frame delta.
- Added `ClothMode`, which selects between simulating every frame (`delta`, the
  default) and running the solver only on native 60 Hz frames (`gate`).
- Fixed a crash caused by a mid-function redirect that left a callee-saved
  register holding a stale value. The fault surfaced over a megabyte away from
  the hook, in unrelated code, because the bad value propagated outward until
  something used it as a pointer.
- Added observation-only hooks that report which cloth solver handles which
  garment, so a specific garment can be matched to the code that drives it.
- Fixed the periodic counters returning early when one counter was zero, which
  had been suppressing every other line during normal gameplay.

The reasoning worth keeping: gating a system to its native rate is correct when
that system owns its own timeline end to end, which is why it works for
cutscenes. It is wrong for a system that is one stage of a per-frame pipeline.
A garment whose solver runs on only half the frames ends up inconsistent with
everything around it that runs on all of them, which showed on screen as a
doubled, semi-transparent copy of the garment. Several attempts to fix that by
changing what a skipped frame did all failed for the same reason: the premise,
not the detail, was wrong.

---

## 0.7a, framerate selection and 240 fps

- Added a fourth entry to the in-game picker, giving 30 / 60 / 120 / 240. The
  stock count of three came from a six-byte helper returning 3 and an unrolled
  filler; the menu's own list is already sized for 30 entries, so this is a
  count patch plus a hook that supplies the values.
- `PickerValues` now accepts up to eight rates.
- Fixed character movement and animation running slow above 60 fps. Character
  control advances in whole ticks of a 300 Hz counter, and 300 divides evenly by
  60 but not by 120 or 240, so the fractional part of every frame was discarded.
  The remainder is now carried between frames. The error was subtle at 120 and
  around four fifths speed at 240.
- Fixed the chosen framerate not surviving a relaunch. The engine resolves its
  target once from a config lookup that finds nothing here, falls back to a
  hardcoded 60 and memoizes that, never consulting the value the game itself
  saved on quit. The saved choice is now read and seeded before the engine first
  asks.
- Bypassed a clamp in the config-apply path that snapped any requested rate
  above 60 back down to 60, which had been undoing the seed.
- The target framerate is also reasserted on a timer, because at least one other
  code path writes it directly and bypasses that clamp entirely.
- Fixed a live menu change being fought back to the startup value. The desired
  rate is now re-read each second, so selecting a different rate applies within
  about a second instead of requiring a relaunch.
- Corrected the earlier reading of a nearby constant as a maximum framerate cap.
  It is a platform identifier. No cap stands in the way of 240; the real ceiling
  is the 300 Hz character tick, which puts the hard limit at 300 fps.

---

## 0.6a, cutscene timing

The one subsystem that does not scale above 60 fps.

- Integrated `safetyhook` for inline hooking, cross compiled under mingw-w64
  alongside Zydis.
- Located the engine's frame timing structure by two independent derivations
  that agree: a `lea` inside the cutscene update resolves to its base, and a
  `movss` inside the camera update resolves 0x18 into the same structure.
- Hooked the cutscene ("polygon demo") update and gated it on the engine's
  60 Hz tick counter, so cutscenes advance at their native rate while the game
  continues rendering at the selected framerate. The framerate is not capped.
- Added periodic call counters for the hook, so the log alone shows whether it
  is firing on the frames expected. At 120 fps roughly half of calls should be
  gated out; at 60 fps, none.
- New `GateCutscenes` setting.

Established during testing at 120 fps: gameplay, animation and audio are all
correct without intervention, because the engine drives them from a real frame
delta. Cutscene playback was the only clearly broken system.

---

## 0.5a, code analysis pipeline

`mgs4.exe` is wrapped by Steam's DRM stub, so `.text` is ciphertext on disk and
signatures cannot be developed against the shipped file. This version builds the
tooling to work around that.

- Replaced the DRM unpack detector. The first attempt sampled `.text` for `int3`
  padding at fixed strides and never fired, because that probe almost never
  lands on inter-function padding; it read 0.0000 even on fully decrypted code.
- New detector anchors on `.pdata`, which is not encrypted and holds 90,583
  verified function entry points. It samples known function starts and tests
  whether the byte there can legitimately begin a function. Measured in game:
  0.101 while encrypted, 0.977 once decrypted, with the transition about 200 ms
  after the worker thread starts.
- Added a section dumper that writes decrypted `.text`, `.rdata` and `.data` to
  disk, off by default.
- `tools/rebuild_dump.py` splices a dump into a copy of the shipped executable
  at the matching file offsets. Every field other than the section bytes is
  already correct in the original, so the result is a valid PE that disassembles
  at the right addresses with no header surgery.
- `tools/mksig.py` derives a minimal unique signature for a function, wildcarding
  the operands that move on a relink (RIP-relative displacements, rel32 branch
  targets) and reporting the shortest prefix that still matches exactly once.
- New `DumpSections` setting.

---

## 0.4a, framerate picker

The headline feature.

- Located the picker's option array, a terminated list of 32-bit integers, in
  the executable's `.data` section. Found by byte signature rather than a fixed
  address.
- Established that the menu holds no label strings for these values. The
  localization data supplies labels for every enumerated setting and none for
  the numeric ones, so labels are formatted from the integers themselves.
  Rewriting the array is the whole feature.
- The picker now offers 30 / 60 / 120. Verified in game, including that the
  choice persists through the game's own settings file.
- The array is validated for shape before anything is written (plausible rates,
  strictly ascending, correct terminator) and the write is read back to confirm.
  A game update that moves the array will fail loudly rather than corrupt
  whatever now occupies the address.
- The whole array is rewritten rather than a single entry replaced, so the menu
  reads in ascending order.
- Logged the surrounding structure on every run. This is what surfaced the
  engine's maximum framerate cap of 128, which is the obstacle for 240 fps.
- New `PickerValues` and `PatchPicker` settings.

---

## 0.3a, configuration file tooling

The game's `config/*.ecf` files are obfuscated INI, and several settings worth
reaching are only present there.

- Recovered the obfuscation key from the executable's `.rdata`, where it is
  stored in cleartext.
- Determined the transform: a repeating XOR whose key index advances by one byte
  per key length block. A flat repeating XOR decrypts only the first block and
  then desynchronises, which is why the format can look like a stream cipher on
  a first pass.
- `tools/ecf.py` implements decrypt, encrypt, verify and a `set` subcommand for
  editing a single key in place. The transform is an involution, so encrypting
  and decrypting are the same operation.
- All five shipped config files decrypt to fully printable text and re-encrypt
  byte identically, checked by the `verify` subcommand.

---

## 0.2a, core runtime framework

Everything the game specific work is built on.

- File logger. Flushes every line, since a buffered log is useless when the
  process crashes. Logs addresses relative to the module base so entries can be
  pasted straight into a disassembler.
- INI reader with typed getters, defaults, and generation of a commented default
  file when none exists. Parse failures warn and keep the default rather than
  throwing inside the game's process.
- Module and section resolution, including rebasing of statically noted
  addresses onto the runtime image base. The loader does apply ASLR to this
  executable.
- Pattern scanner supporting wildcards, anchored on the first non-wildcard byte
  for a fast reject. Callers can count matches, so a signature that has become
  ambiguous is detected instead of silently resolving to the first hit.
- RAII guard for page protection changes, restoring the original protection on
  scope exit, plus typed read and write helpers.
- RIP-relative operand resolver taking an explicit instruction end, so it is
  correct for instructions with a trailing immediate.

---

## 0.1a, build system and injection

- Cross compile to a Windows x64 DLL with mingw-w64, driven by CMake and Ninja.
- `build.sh` with `clean` and `install` subcommands. It re-enters the build
  container automatically when the cross compiler is not on `PATH`, so it works
  from either inside or outside.
- Container recipe for immutable distributions, where a toolchain cannot be
  installed into the root filesystem.
- Proxy DLL impersonating a system DLL that the game imports directly, so the
  game's own import table loads the mod before the executable's entry point runs
  and no external loader is required.
- Export forwarding is bound lazily, on first call, to the real system DLL
  loaded by absolute path. Resolving under the loader lock would risk a deadlock,
  and the forwarded functions cannot be called before the worker thread runs.
- The impersonated DLL is a build option rather than hardcoded.
- Exports are restricted to exactly the forwarded set, so none of the mod's own
  symbols leak into the export table.
- Bootstrap moved off `DllMain` onto a worker thread.
