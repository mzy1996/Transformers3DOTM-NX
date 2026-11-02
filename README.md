# Transformers3DOTM-NX

A native Nintendo Switch port of the Xbox360 version of Transformers: Dark of the Moon by static recompile with ReXGlue

**English** | [中文](README_zh.md)

> This repository is a **source snapshot** of the porting project (first sync 2026-11-02, fix42 stage). Technical route: `rexglue-sdk` / `XenosRecomp` static recompilation + `mesa-switch` graphics backend, built into a Switch NRO via devkitPro + libnx.

## Repository Contents and Exclusions

### Included

| Path | Contents |
| --- | --- |
| `recomp/` | Build configuration and project-owned sources: `CMakeLists.txt`, `CMakePresets.json`, `src/` (app headers, startup markers, stdio lock stubs), `templates/`, `assets/`, `overrides.toml` (recompiled-function override table), `t3dotm_manifest.toml` (codegen manifest), `codegen_t3dotm.sh` |
| `repos/nfsmw-nx/sdk/` | Porting-layer SDK sources (landing points of fix36–fix42: `src/core/guest_memory_switch.cpp`, `src/core/exception_handler_switch.cpp`, etc.); `out/` and `thirdparty/` removed |
| `tools/` | Porting and forensics tools: `xex_analyze.py`, `xex_dump.py`, `xenia_live_unpack.py`, `verify_nro.py`, `pe_verify.py`, patching scripts, `xex_delta_patch/`, `xex_patcher_cli/`, `switch/` (linker scripts and NRO rodata patches) |
| `scripts/` | Build, packaging and batch-patching scripts (`build_and_verify.sh`, `build_switch.sh`, `package.sh`, `setup_update.sh`, `patch_fix39*.py`, etc.) |
| `README_dispatch_fix36.md` … `README_dispatch_fix42.md` | Per-round fix delivery notes: on-device log measurements, changed-line numbers and SHA-256 fingerprints, retest steps and decision tables |
| `README.md` (this file, English), `README_zh.md` (Chinese) | Project description in both languages |
| `T3DOTM.toml`, `icon_T3.png` | Build configuration and icon |

### Excluded (hard-excluded by `.gitignore`)

- **Original game assets and extracted artifacts**: `game/`, `extracted/`, `patched/`, `patched_flat.bin`, `update/` (TU / DLC data `default.xexp`, `TransGame`)
- **Build and codegen artifacts**: `recomp/out/`, `recomp/generated/` (regenerated from the game XEX by `codegen_t3dotm.sh`), `repos/nfsmw-nx/sdk/out/`, `*.nro`, `*.elf`, `*.o`
- **Vendored third-party dependencies**: `repos/nfsmw-nx/sdk/thirdparty/` (FFmpeg, catch2, cli11, dxc, etc., restorable from upstream)
- **Delivery archives and logs**: `*.zip`, `*.log`, `*.bak*`, `.dumate/`

> Line endings: files are stored **byte-for-byte** (`.gitattributes: * -text`, no EOL normalization), so the SHA-256 of committed files matches the in-project baseline fingerprint; `repos/nfsmw-nx/sdk/src/core/*.cpp` are CRLF.

---

# T3DOTM — Transformers: Dark of the Moon (Xbox 360) → Nintendo Switch

The ReXGlue recompile port project of Transformers: Dark of the Moon (Xbox 360) to Nintendo Switch.

- Platform: Nintendo Switch (custom firmware, Homebrew Menu / direct NRO launch)
- Graphics backend: Mesa NVK (Vulkan driver on Switch, based on the official NVN path)
- Content layout: original game/ + TU title update (update/) + **install-time fused xex (patched/)** sibling directories,
  layered loading at runtime, without overwriting base files
- Performance target: no overclocking (official APM performance profile only), stable 60 FPS (see Section 5 for the definition)

## 1. Directory Layout (SD card)

Place the following contents under `sdmc:/switch/Transformers3DOTM/` on the SD card:

```
sdmc:/switch/Transformers3DOTM/
├── T3DOTM.nro          # the executable (recomp/out/sw8/T3DOTM.nro)
├── game/               # complete game files extracted from the retail ISO (default.xex, TransGame/, etc.)
├── update/             # TU data overlay (optional; serves as a placeholder when no TU data is present)
└── patched/            # TU code patch fused at install time (default.xex, produced by setup_update.sh)
```

- `game/`, `update/`, and `patched/` are **sibling directories**; none is the parent of another.
- The runtime layered lookup order is **update/ > patched/ > DLC > game/**:
  - TU data overrides (camera.lxb, tuning.lxb, text/, ui/, etc.) are matched first in the `update/` layer by the runtime layered VFS;
  - TU code patches **need not be applied at runtime**: during the PC install-time, `setup_update.sh` has already used
    `xex_patcher` to **fuse** the base `default.xex` with the TU `default.xexp` into `patched/default.xex`
    (decrypted, decompressed, TU code in effect); at runtime, `UserModule` detects the fused XEX
    (FILE_FORMAT_INFO is NONE/NONE) and skips sibling patch lookup.
  - **The original files in `game/` are read-only throughout and are never modified or overwritten**.
- In this project, `recomp/T3DOTM_manifest.toml` has `game_root = "../game"`, and codegen is invoked with
  `--update_data_root="../update"`, so that the manifest, codegen, and runtime all work with the same layout.
- The NRO automatically discovers the `game/`, `update/`, and `patched/` in the same directory (see `OnConfigurePaths`
  in `recomp/src/T3DOTM_app.h`); just place the directory names on the SD card according to the layout above.

> The game data (game/, update/) is on the order of 7 GB and is not part of the project package; extract it from your existing Xbox 360 image/title update.
> Unpack the TU container with `python tools/xenia_live_unpack.py` into `extracted/TU`,
> then run `bash scripts/setup_update.sh` to generate `update/` and `patched/`.

## 2. Build Steps

### Prerequisites

- Windows (this project is built under Windows + Git Bash), or any environment that can run devkitPro
- devkitPro + devkitA64 + libnx (`DEVKITPRO` environment variable valid; `C:\devkitPro` on this machine)
- CMake ≥ 3.25, Ninja, Python 3
- Dependencies (already in place under `repos/`):
  - `repos/nfsmw-nx/` — Switch version rexglue-sdk base (layered VFS, input mapping, profiling, etc.),
    with `sdk/out/win-amd64/rexglue.exe` as the host-side codegen tool
  - `repos/mesa-switch/mesa-unified-install/opt/devkitpro/portlibs/switch` — prebuilt NVK Horizon SDK
- `recomp/generated/` is the codegen output (run codegen the first time; see below)

### Codegen (first time / after TU changes)

```bash
bash recomp/codegen_t3dotm.sh    # requires rexglue.exe (the script defaults to repos/nfsmw-nx/...)
```

### Build

```bash
bash scripts/build_switch.sh -j 8          # LTO build (60 FPS performance target)
bash scripts/build_switch.sh --no-lto -j 8 # for diagnostics / quick verification only
```

Artifact: `recomp/out/sw8/T3DOTM.nro`

Notes:

- LTO is enabled by default (`-flto=2 -flto-partition=balanced`, linked with
  `--gc-sections` per `switch.specs`). **Do not use `--no-lto` as a regular build**: its artifact is used only for quick verification flows
  and does not meet the 60 FPS performance target.
- LTO linking has been fixed: under GCC LTO with large partitions (127+ partitions), the asm
  `.weak /.set` alias symbols of the original `DEFINE_REX_FUNC` are trimmed away by partitioning, causing `PPCFuncMappings` references to dangle and the linker to report
  `undefined reference to 'sub_...'`. It is now changed to a **thunk scheme**: `name` is a real IR weak function
  (`used, weak, noinline`), whose body is a `[[gnu::musttail]]` tail call to `__imp__name`,
  ensuring the symbol is emitted with the LTO partition and can be overridden by hooks (the template `pch_h.inja` and the generated file
  `T3DOTM_pch.h` must be modified in sync). Post-fix toggle verification: 111,011 `sub_*` defined,
  0 undefined, and all registry (`T3DOTM_register.cpp`) entries matched.
- LTO linking must carry `-z notext` (the `_SWITCH_LTO_DIAG` option in `tools/switch/cmake/switch-devkitA64.cmake`
  is enabled by default): relative relocations produced during the LTO whole-program phase land in read-only segments
  (596 measured in `.rodata`, originating from the GCC switch jump table `CSWTCH.*` and SDK-internal
  iconv encoding tables; `.text` itself has 0), and a strict `-z text` reports
  `read-only segment has dynamic relocations` and fails the link. The artifact therefore carries the
  `DT_TEXTREL` flag: it only affects a small number of pointer writes at load time and does not affect the runtime frame rate; if a strict artifact with no textrel
  is desired, these tables must be moved further out of read-only segments (not done at present; a known item).
- `T3DOTM_register.cpp` (about 110k lines, pure symbol registry, no hot code) is compiled separately with `-O0`
  (`set_property(... COMPILE_OPTIONS "-O0")` in `recomp/CMakeLists.txt`),
  to avoid GCC spending hours on it under `-O3`; it does not affect artifact behavior.
- Icon/title/author: `icon_T3.png` (optional; when missing, elf2nro ships without an icon),
  `Transformers: Dark of the Moon`, `mzy1996` (see
  `rexglue_switch_add_nro` in `recomp/CMakeLists.txt`).

## 3. Project Structure

| Path | Description |
|---|---|
| `recomp/` | Main project: manifest (`T3DOTM_manifest.toml`), codegen script, application code (`src/`), codegen output (`generated/`), build output (`out/`) |
| `recomp/src/T3DOTM_app.h` | App hooks: automatic path discovery (game/, update/, patched/ next to the NRO), APM performance profile request (GPU 460.8 MHz, memory 1331.2 MHz, official API, non-overclocked) |
| `repos/nfsmw-nx/` | Switch version rexglue-sdk base (layered VFS, etc.; `user_module.cpp` includes fused XEX runtime patch skip detection) |
| `repos/mesa-switch/` | NVK Horizon SDK (built) |
| `scripts/` | build_switch.sh (build), setup_update.sh (TU unpacked data + fused xex → update/, patched/), package.sh (rebuild the delivery zip in place) |
| `tools/` | Helper tools (xenia_live_unpack.py, xex_analyze.py, pe_verify.py, xex_patcher_cli, xex_delta_patch, switch/cmake toolchain files, etc.) |
| `docs/` | Project documentation (XEX/TU analysis records, performance verification methods, etc., generated as needed) |

## 4. Current Status

- [x] Input inventory (ISO, TU, toolchain)
- [x] Reference repo and dependency research (rexglue-sdk / nfsmw-nx / mesa-switch in place)
- [x] Switch project skeleton (directory structure, CMake, toolchain, layer layout, update/ runtime loading design)
- [x] codegen (default.xex → C++; TU included in codegen)
- [x] TU title update unpacking and update/ + patched/ materialization (install-time fusion flow run through;
      `patched/default.xex` 28,393,472 B, XEX2/PE verification passed)
- [x] Rendering/performance integration and build (NVK link passed; NRO produced)
- [x] LTO link fix (thunk scheme: `DEFINE_REX_FUNC` changed to IR weak function + musttail tail call;
      the LTO NRO has been produced and passed link verification, pending on-device verification of crashes and 60 FPS)
- [x] Packaging and delivery (README + project package zip)
- [x] On-device startup crash localization (dispatch thread TLS/EH issue)
- [x] On-device re-launch verification (TLS fix effective: game entered normal mode and rendered for about 33 seconds,
      new crash localized to unregistered guest address 0x8328B5C8)
- [x] Round 18 crash fix (0x8328B5C8 this-adjustment thunk registered into overrides.toml;
      after codegen rebuild the NRO has been produced and passed verify_nro.py)
- [x] Round 19 on-device retest (confirmed passing the 0x8328B5C8 indirect dispatch point; but stopped at a
      new crash `0x8255B3D8` after the first scene; see the 2026-10-19 batch 15 record below)
- [x] Round 19 crash fix (0x8255B3D8 vtable indirect dispatch entry registered into overrides.toml;
      after codegen rebuild the NRO has been produced and passed verify_nro.py)
- [x] Round 20 on-device retest (after passing 0x8255B3D8, uniformly stopped at unregistered 0x830EA420;
      root cause was that overrides mistakenly declared a mid-body segment as a stub entry, leaving that address unregistered;
      overrides corrected and codegen + NRO rebuilt, verify_nro.py PASS)
- [x] Round 21 on-device retest (after passing 0x830EA420, a new crash `0x833CF038` triggered after the first scene; see the 2026-10-19 batch 19 record below)
- [x] Round 22 on-device retest and fix (after passing 0x833CF038, uniformly stopped at a new crash `0x8334D6A0`; registered per batch 20 and rebuilt; see the 2026-10-19 batch 20 record below)
- [x] Round 23 on-device retest and fix (after passing 0x8334D6A0, uniformly stopped at a new crash `0x832F7068`; registered per batch 21 and rebuilt; see the 2026-10-19 batch 21 record below)
- [x] Round 24 on-device retest and fix (after passing 0x832F7068, uniformly stopped at a new crash `0x8334D5C8`; registered per batch 22 and rebuilt; see the 2026-10-19 batch 22 record below)
- [x] Round 25 on-device retest and fix (after passing 0x8334D5C8, uniformly stopped at a new crash `0x833D6FC8`; registered per batch 23 and rebuilt; see the 2026-10-19 batch 23 record below)
- [x] Round 26 crash fix (batch 24: 48 unresolved branches inside the large function 0x832F5648;
      removed the inline declarations of 0x832F5810/0x832F5884 to let Discover extend the large function; see the below
      2026-10-20 batch 24 record; NRO verified and build passes)
- [x] Video playback logic research (confirmed T3DOTM uses Bink (.bik) video rather than WMV; referring to
      the conclusions and optional approaches of nfsmw-nx WMV+FFmpeg and the UnleashedRecomp-NX rendering patch;
      see the 2026-10-20 video playback research record below)
- [x] Round 27/28 log analysis (Round 27's FATAL 0x833D6FC8 was a reproduction of an old build; the current
      batch 24 has it registered; Round 28's access violation reading guest 0x5660 is a new crash type, not a video
      path, requiring a batch 24 on-device retest to confirm whether it reproduces; see the 2026-10-20 record below)
- [x] Video loading fix (filled in the language-variant
      .bik aliases the game looks up under update/TransGame/Movies/, eliminating the startup/loading screen NtCreateFile FAILED, so that the opening and loading screen
      videos can actually load and be measured; see the 2026-10-20 record below)
- [x] Round 29 on-device retest and fix (batch 25: after passing 0x82E869A0, uniformly stopped at
      a new crash `0x827DC208`; registered per batch 26 and rebuilt; see the 2026-10-21
      batch 26 record below / README_dispatch_fix2.md)

### Crash Fix Record (2026-10-18)

**Symptom**: Data Abort after startup with `modo: normal`, PC `imagen+0x556a48c`,
fault address `0xffffffcf2c6ae551` (invalid kernel region). Crashing thread "Kernel Dispatch" (`dispatch_thread_`).

**Root cause**: `std::condition_variable_any::wait` internally calls `std::uncaught_exception()`,
which goes through `__cxa_get_globals()` → `pthread_getspecific()` to read the EH TLS slot. In libnx's
TLS implementation, the key allocation limit is 0..26, and only the **calling thread**'s slot is initialized; the dispatch thread is created only after that key
is allocated, so its slot holds garbage → accessing `0xffffffcf2c6ae551` crashes.

**Fix**: Replace `std::condition_variable_any dispatch_cond_` at `kernel_state.h:396`
with a plain `std::condition_variable dispatch_cond_` plus a dedicated `std::mutex dispatch_mutex_`;
correspondingly modify `kernel_state.cpp:650-679` (consumer; the lock switched to `unique_lock<std::mutex>`) and
`1223-1246` (producer; `push_back` wrapped in `lock_guard<std::mutex>`, `notify_all` outside the lock).
Disassembly has confirmed that the plain `std::condition_variable::wait` directly `b pthread_cond_wait`s and does not call
`uncaught_exception()`, fully bypassing the EH TLS read.

**Artifact**: `recomp/out/sw8/T3DOTM.nro` (2026-10-18 23:58, non-LTO diagnostic build, verification passed).
**Measured conclusion**: the TLS fix is effective; the game enters `modo: normal`, 20 draws, and after rendering for about 33 seconds triggers
a new round of crash `[FATAL] Call to invalid or unregistered function at guest address 0x8328B5C8`.

### Crash Fix Record (2026-10-19)

**Symptom**: `[FATAL] Call to invalid or unregistered function at guest address 0x8328B5C8`
(Round 18 on-device log; the game had entered normal mode and rendered for about 33 seconds).

**Root cause**: 0x8328B5C8 is a 12-byte this-adjustment thunk
(`addi r5,r5,-16; addi r3,r3,-16; b 0x82F2FA70`), located between the already-registered functions
`sub_8328B5C0` (ending at 0xC8) and `sub_8328B5D8` (starting at 0xD8);
codegen did not recognize it as a separate function. When the game calls this address via UE3 vtable indirect dispatch (bctr),
there is no matching entry in the runtime dispatch table → FATAL. Similar issues had previously been
fixed round by round via overrides.toml batch 1-13.

**Fix**: Append batch 14 to `recomp/overrides.toml`:
`"0x8328B5C8" = { size = 0x10 }` (covering the 12-byte thunk + padding).
After re-running `bash recomp/codegen_t3dotm.sh`, codegen generates
`DEFINE_REX_FUNC(sub_8328B5C8)` (`T3DOTM_recomp.58.cpp`) and registers
`SetFunction(0x8328B5C8, sub_8328B5C8)` in the registry. Its real tail-call target `sub_82F2FA70`
was already registered, with no new unresolved branches.

**Artifact**: `recomp/out/sw8/T3DOTM.nro` (2026-10-19 01:37, non-LTO diagnostic build,
verify_nro.py Verdict: PASS).
**Pending verification**: On-device retest to confirm progression past the 0x8328B5C8 indirect dispatch point; if a new
unregistered indirect dispatch address appears, append an overrides.toml batch and rebuild following the same procedure.

### Crash Fix Record (2026-10-19, batch 16)

**Symptom**: Two independent on-device retests both passed the previously fixed 0x8255B3D8, but uniformly stopped after the first scene
with `[FATAL] Call to invalid or unregistered function at guest address 0x830EA420`
(recorded in the `T3DOTM_001(20)` and `T3DOTM_002(18)` logs; the Atmosphere crash reports are both `Type: User Break`,
PC = `T3DOTM + 0x5559e34`, i.e., the recomp runtime REX_FATAL breakpoint, which is expected behavior,
confirming both crashes share the same root).

**Root cause**: 0x830EA420 lies in the middle of the body of the function `sub_830EA3B0` (`mflr r12`/`stwu` entry).
Previously overrides mistakenly declared `0x830EA410` (the `rlwinm` instruction) as a separate stub entry
(`"0x830EA410" = { size = 0x18 }`), causing codegen to register only up to 0x830EA410,
leaving 0x830EA420 in an unregistered region. When the runtime calls 0x830EA420 via indirect dispatch (bctr),
the dispatch table has no matching entry → FATAL.

Meanwhile, `0x830EA378` (`lwz r11,80(r31)`) lies in the middle of the body of `sub_830EA368`
and is referenced by an unconditional tail call `b 0x830EA378` at `0x83269FB8`; when this declaration had previously been mistakenly removed,
codegen Validate reported `UnresolvedCall` and aborted; it has been added back.

**Fix**:
- Remove the erroneous declaration `"0x830EA410" = { size = 0x18 }` from `recomp/overrides.toml`
  (that address is a mid-body instruction, not an entry).
- Keep `"0x830EA378" = { size = 0x40 }` (referenced by the `b` jump at `0x83269FB8`,
  needing to be registered as a separate function entry).
- Re-run `bash recomp/codegen_t3dotm.sh` (the erroneous `--update_data_root`
  parameter has been removed, confirming codegen uses `patched/default.xex` as the sole entry). codegen generates
  `DEFINE_REX_FUNC(sub_830EA420)` (`T3DOTM_recomp.77.cpp`) and registers
  `SetFunction(0x830EA420, sub_830EA420)` in the registry; Validate passes ("6 written, 157 unchanged").

**Artifact**: `recomp/out/sw8/T3DOTM.nro` (2026-10-19 05:30, non-LTO diagnostic build,
verify_nro.py Verdict: PASS).
**Pending verification**: On-device retest to confirm progression past the 0x830EA420 indirect dispatch point; if a new
unregistered indirect dispatch address appears, append an overrides.toml batch and rebuild following the same procedure.

### Crash Fix Record (2026-10-19, batch 19)

**Symptom**: In the 21st round of on-device retest, after passing 0x830EA420, it triggers after the first act
`[FATAL] Call to invalid or unregistered function at guest address 0x833CF038`
(recorded in the `T3DOTM_001(23)`/`T3DOTM_002(21)` logs; the Atmosphere crash reports are all
`Type: User Break`, PC = `T3DOTM + 0x5559870`, confirming a common origin).

**Root cause**: 0x833CF038 is the entry of a this-adjustment thunk, located in the 20-byte gap
between the registered function `sub_833CF030` (ends at 0x833CF038, 8 bytes: `mr r3,r4; b 0x833ce938`) and
`sub_833CF04C` (starts at 0x833CF04C, 12 bytes: `mr r3,r4; lwz r4,4(r5); b 0x833cef78`),
and codegen does not recognize it as a standalone function. When the runtime calls this address via indirect dispatch,
the dispatch table has no matching entry → FATAL. The same class of problem was previously fixed with the
small-thunk pattern of overrides.toml batch 8 (0x82AF9408).

**Fix**: Append batch 19 to `recomp/overrides.toml`:
`"0x833CF038" = { size = 0x14 }` (covering the 20-byte gap up to the next function entry).
After re-running `bash recomp/codegen_t3dotm.sh`, codegen generates
`DEFINE_REX_FUNC(sub_833CF038)` (`T3DOTM_recomp.63.cpp`) and registers in the registry
`SetFunction(0x833CF038, sub_833CF038)`.

**Artifact**: `recomp/out/sw8/T3DOTM.nro` (2026-10-19 18:53, non-LTO diagnostic build,
verify_nro.py Verdict: PASS).
**Pending verification**: on-device retest to confirm that it proceeds past the 0x833CF038 indirect dispatch point; if a new
unregistered indirect dispatch address appears, append an overrides.toml batch and rebuild via the same procedure.

### Crash Fix Record (2026-10-19, batch 20)

**Symptom**: The 22nd round of on-device retest confirms passing past 0x833CF038 fixed in the previous round (this address no longer appears in the
crash report), but it uniformly stops at a new `[FATAL] Call to invalid or unregistered function
at guest address 0x8334D6A0` (recorded in the `T3DOTM_001(24)`/`T3DOTM_002(22)` logs;
the Atmosphere crash reports are all `Type: User Break`, PC = `T3DOTM + 0x5559770`,
LR = `T3DOTM + 0x555506c`, confirming a common origin).

**Root cause**: 0x8334D6A0 lies in the 0x50-byte gap between the registered function `sub_8334D670` (ends at 0x8334D670 + body length) and
`sub_8334D6C0` (starts at 0x8334D6C0), and codegen does not recognize it
as a standalone function. When the runtime calls this address via UE3 vtable indirect dispatch (bctr), the dispatch table has no matching entry → FATAL.
The same class of problem is consistent with the small-thunk/gap function pattern of the previous overrides.toml batches 8/14/19.

**Fix**: Append batch 20 to `recomp/overrides.toml`:
`"0x8334D6A0" = { size = 0x20 }` (covering up to just before the next registered function `sub_8334D6C0`).
After re-running `bash recomp/codegen_t3dotm.sh`, codegen generates the corresponding
`DEFINE_REX_FUNC(sub_8334D6A0)` (`T3DOTM_recomp.47.cpp`) and registers in the registry
`SetFunction(0x8334D6A0, sub_8334D6A0)` ("5 written, 158 unchanged", Validate passed).

**Artifact**: `recomp/out/sw8/T3DOTM.nro` (2026-10-19 20:16, non-LTO diagnostic build,
verify_nro.py Verdict: PASS).
**Pending verification**: on-device retest to confirm that it proceeds past the 0x8334D6A0 indirect dispatch point; if a new
unregistered indirect dispatch address appears, append an overrides.toml batch and rebuild via the same procedure.

### Crash Fix Record (2026-10-19, batch 21)

**Symptom**: The 23rd round of on-device retest confirms passing past 0x8334D6A0 fixed in the previous round (this address no longer appears in the
crash report), but it uniformly stops at a new `[FATAL] Call to invalid or unregistered function
at guest address 0x832F7068` (recorded in the `T3DOTM_001(25)`/`T3DOTM_002(23)` logs;
the Atmosphere crash reports are all `Type: User Break`, PC = host recompiled address
`T3DOTM + 0x55597f4`, confirming a common origin).

**Root cause**: 0x832F7068 lies exactly in the 0x08-byte gap between the registered function `sub_832F7058`
(classic vtable dispatch thunk: `lwz r11,0(r3); lwz r11,148(r11); mtctr r11; bctr`,
ends at 0x832F7068) and the next registered function `sub_832F7070` (starts at 0x832F7070),
and codegen does not recognize it as a standalone function. When the runtime calls this address via UE3 vtable
indirect dispatch (bctr), the dispatch table has no matching entry → FATAL. The same class of problem
is consistent with the small-thunk/gap function pattern of the previous overrides.toml batches 8/14/19/20.

**Fix**: Append batch 21 to `recomp/overrides.toml`:
`"0x832F7068" = { size = 0x08 }` (covering the 8-byte gap up to just before the next registered function
`sub_832F7070`; it must not be enlarged, otherwise it overlaps with sub_832F7070).
After re-running `bash recomp/codegen_t3dotm.sh`, codegen generates the corresponding
`DEFINE_REX_FUNC(sub_832F7068)` (`T3DOTM_recomp.42.cpp`) and registers in the registry
`SetFunction(0x832F7068, sub_832F7068)` ("5 written, 158 unchanged", Validate passed).

**Artifact**: `recomp/out/sw8/T3DOTM.nro` (2026-10-19 21:39, non-LTO diagnostic build,
verify_nro.py Verdict: PASS).
**Pending verification**: on-device retest to confirm that it proceeds past the 0x832F7068 indirect dispatch point; if a new
unregistered indirect dispatch address appears, append an overrides.toml batch and rebuild via the same procedure.

### Crash Fix Record (2026-10-19, batch 22)

**Symptom**: The 24th round of on-device retest confirms passing past 0x832F7068 fixed in the previous round (this address no longer appears in the
crash report), but it uniformly stops at a new `[FATAL] Call to invalid or unregistered function
at guest address 0x8334D5C8` (recorded in the `T3DOTM_001(26)`/`T3DOTM_002(24)` logs;
the Atmosphere crash reports are all `Type: User Break`, PC = host recompiled address
`T3DOTM + 0x55597f4`, confirming a common origin).

**Root cause**: 0x8334D5C8 lies exactly in the 0x18-byte gap between the registered function `sub_8334D5B0`
(classic vtable dispatch thunk: `lwz r11,28(r3); lwz r3,12(r11); lwz r11,0(r3);
lwz r11,40(r11); mtctr r11; bctr`, ends at 0x8334D5C8) and the next registered function
`sub_8334D5E0` (starts at 0x8334D5E0), and codegen
does not recognize it as a standalone function. When the runtime calls this address via UE3 vtable indirect dispatch (bctr),
the dispatch table has no matching entry → FATAL. The same class of problem is consistent with the small-thunk/gap function pattern of the previous
overrides.toml batches 8/14/19/20/21.

**Fix**: Append batch 22 to `recomp/overrides.toml`:
`"0x8334D5C8" = { size = 0x18 }` (covering the 24-byte gap up to just before the next registered function
`sub_8334D5E0`). After re-running `bash recomp/codegen_t3dotm.sh`, codegen
generates the corresponding `DEFINE_REX_FUNC(sub_8334D5C8)` (`T3DOTM_recomp.45.cpp`) and registers in the registry
`SetFunction(0x8334D5C8, sub_8334D5C8)`. Validate passed.

**Artifact**: `recomp/out/sw8/T3DOTM.nro` (2026-10-19 23:44, non-LTO diagnostic build,
verify_nro.py Verdict: PASS).
**Pending verification**: on-device retest to confirm that it proceeds past the 0x8334D5C8 indirect dispatch point; if a new
unregistered indirect dispatch address appears, append an overrides.toml batch and rebuild via the same procedure.

### Crash Fix Record (2026-10-19, batch 23)

**Symptom**: The 25th round of on-device retest confirms passing past 0x8334D5C8 fixed in the previous round (this address no longer appears in the
crash report), but it uniformly stops at a new `[FATAL] Call to invalid or unregistered function
at guest address 0x833D6FC8` (recorded in the `T3DOTM_001(27)`/`T3DOTM_002(25)` logs;
the Atmosphere crash reports are all `Type: User Break`, PC = host recompiled address
`T3DOTM + 0x55597f4`, confirming a common origin).

**Root cause**: 0x833D6FC8 lies exactly in the 0x20-byte gap between the registered function `sub_833D6FB0`
(classic vtable dispatch thunk, ends at 0x833D6FC8) and the next registered function
`sub_833D6FE8` (starts at 0x833D6FE8), and codegen
does not recognize it as a standalone function. When the runtime calls this address via UE3 vtable indirect dispatch (bctr),
the dispatch table has no matching entry → FATAL. The same class of problem is consistent with the small-thunk/gap function pattern of the previous
overrides.toml batches 8/14/19/20/21/22.

**Fix**: Append batch 23 to `recomp/overrides.toml`:
`"0x833D6FC8" = { size = 0x20 }` (covering the 32-byte gap up to just before the next registered function
`sub_833D6FE8`). After re-running `bash recomp/codegen_t3dotm.sh`, codegen
generates the corresponding `DEFINE_REX_FUNC(sub_833D6FC8)` (`T3DOTM_recomp.58.cpp`) and registers in the registry
`SetFunction(0x833D6FC8, sub_833D6FC8)`. Validate passed.

**Artifact**: `recomp/out/sw8/T3DOTM.nro` (2026-10-20 00:38, non-LTO diagnostic build,
verify_nro.py Verdict: PASS).
**Pending verification**: on-device retest to confirm that it proceeds past the 0x833D6FC8 indirect dispatch point; if a new
unregistered indirect dispatch address appears, append an overrides.toml batch and rebuild via the same procedure.

### Crash Fix Record (2026-10-19, batch 15)

**Symptom**: Two independent on-device retests both pass past 0x8328B5C8 fixed in the previous round, but both uniformly stop after the first act
`[FATAL] Call to invalid or unregistered function at guest address 0x8255B3D8`
(recorded in the `T3DOTM_001/002` logs; the Atmosphere crash reports are all `Type: User Break`,
PC = `T3DOTM + 0x5559e34`, i.e. the recomp runtime REX_FATAL breakpoint, which is expected behavior,
confirming the two crashes have a common origin).

**Root cause**: 0x8255B3D8 is a complete function entry (`mr r4,r3; cmpwi r3,0; beqlr; lis/lwz;
lwz r10,0(r3); lwz r9,0x14(r10); mtctr r9; bctr; blr`, a null-pointer guard + vtable
indirect dispatch pattern), located between the registered `sub_8255B388` (ends at ~0x8255B3A8, bctr tail)
and `sub_8255B400`, and codegen does not recognize it as a standalone function. When the game calls this address via UE3 vtable indirect dispatch
(bctr), the dispatch table has no matching entry → FATAL.

**Fix**: Append batch 15 to `recomp/overrides.toml`:
`"0x8255B3D8" = { size = 0x28 }` (covering this indirect dispatch tail, up to `0x8255B400`).
After re-running `bash recomp/codegen_t3dotm.sh`, codegen generates
`DEFINE_REX_FUNC(sub_8255B3D8)` and registers in the registry
`SetFunction(0x8255B3D8, sub_8255B3D8)`.

**Artifact**: `recomp/out/sw8/T3DOTM.nro` (2026-10-19 02:53, non-LTO diagnostic build,
verify_nro.py Verdict: PASS).
**Pending verification**: on-device retest to confirm that it proceeds past the 0x8255B3D8 indirect dispatch point; if a new
unregistered indirect dispatch address appears, append an overrides.toml batch and rebuild via the same procedure.

### Crash Fix Record (2026-10-20, batch 24)

**Symptom**: The 26th round, 7 logs (`T3DOTM_002(26).log` etc.) confirm passing past
0x833D6FC8 fixed in the previous round (this address no longer appears in the crash report), but all uniformly stop at 48 unresolved branches inside the large function 0x832F5648
(first hit: `FATAL: Unresolved branch from 0x832F5838
to 0x832F5840`, plus internal targets such as 0x832F583C/0x832F5884/0x832F5888/0x832F5890/0x832F59C0
/0x832F58D8; the Atmosphere crash reports are all `Type: User Break`,
PC = host recompiled address REX_FATAL breakpoint, confirming a common origin).

**Root cause**: 0x832F5648 is a large function that never declares a size, and codegen does not treat it as a whole
coverage, causing multiple internal branch targets (0x832F5xxx) to be treated as unknown addresses, so that the generated code retains 48
`REX_FATAL("Unresolved branch from ...")`. The embedded declarations `0x832F5810` (`b 0x832F5884` thin wrapper) and `0x832F5884` previously retained in
overrides.toml overlap with the large function span or fragment the parent function,
so that Discover cannot resolve the internal targets to in-function
`loc_` labels.

**Fix**: **Completely delete** the declarations of `"0x832F5810"` and
`"0x832F5884"` from `recomp/overrides.toml` (including the early batch 24 comments), keeping only
the empty declaration `"0x832F5648" = { }`, letting Discover extend it to the next registered function
`0x832F5CBC` (`{ size = 0x40 }` retained). After codegen, all 48 FATALs in the 0x832F5xxx region
disappear: `sub_832F5648` (`T3DOTM_recomp.57.cpp` ~line 110164)
internal 0x832F5814/0x832F5828/0x832F583C/0x832F5840/0x832F5884/0x832F5888/
0x832F5890/0x832F59BC/0x832F59C0/0x832F59C8/0x832F5A1C/0x832F5A38/
0x832F5A3C/0x832F5A64 etc. all resolve to `loc_` labels; in register.cpp
`SetFunction(0x832F5648, sub_832F5648)` is retained, and 0x832F5810/0x832F5884
are absorbed by the large function and are no longer registered separately.

> Note (3 failed attempts in this batch, to avoid regressions):
> 1. `0x832F5810 = { size = 0x20 }` + `0x832F5648 = { size = 0x674 }` →
>    the manifest reports an `Overlapping boundaries` failure;
> 2. empty declaration of 0x832F5810 + fixed size for 0x832F5648 → still overlaps and fails (an empty declaration with size=0
>    also participates in the overlap check);
> 3. both declared empty → codegen passes, but Discover splits 0x832F5810 into a standalone node,
>    and the generated code retains 48 unresolved branches. The embedded declarations must be deleted entirely.

**Artifact**: `recomp/out/sw8/T3DOTM.nro` (2026-10-20 07:44, non-LTO diagnostic build,
109,557,774 B / 0x687B80E, verify_nro.py Verdict: PASS; `build_b24.log`).
**Known remaining issues**: after being absorbed by the large function, 0x832F5810 is no longer a standalone registration entry; a static grep finds no
direct `bl 0x832F5810` jump, but if a `bctr` dispatches indirectly to this address at runtime, it will cause a
"Call to invalid or unregistered function" FATAL, and this needs on-device verification.
**Pending verification**: on-device retest to confirm that it proceeds past the internal region of the large function 0x832F5648; if a new
unregistered indirect dispatch address appears, append an overrides.toml batch and rebuild via the same procedure.

### Video Playback Investigation Record (2026-10-20)

**Background**: the crash possibly caused by video playback requires reference to the
video playback logic of other XBOX360→Switch porting projects. Local reference repositories:
- `repos/nfsmw-nx` (`https://github.com/StevensND/nfsmw-nx`, SDK base only)
- `repos/unleashednx-tu-ref-chansey` (`https://github.com/ChanseyIsTheBest/UnleashedRecomp-NX`)
- `repos/masseffect-nx` (same rexglue-sdk architecture, UE3 + Bink, includes complete app-layer source)

**Conclusion: T3DOTM uses Bink (.bik) video. The format differs from nfsmw-nx's WMV approach,
so the WMV+FFmpeg path cannot be applied directly; the best reference is masseffect-nx's "keep the recompiled
Bink decoder and have the SDK adapt only rendering" route.**

1. **nfsmw-nx (WMV)**: the recompiled XDK WMV decoder cannot run in real time on Switch,
   so at the IO layer (the
   `NfsmwAnotarApertura`/`NfsmwAnotarLectura` in `sdk/src/kernel/xboxkrnl/xboxkrnl_io.cpp`) it records the `.wmv`
   paths the game opens/reads, and the app layer reopens them with FFmpeg and decodes natively (the configuration enables
   the `wmav2, wmv3, xmaframes` decoders).
2. **UnleashedRecomp-NX**: `UnleashedRecomp/patches/video_patches.cpp` handles the
   **rendering** side (`CPlayMovieWrapper::CRender` aspect ratio, motion blur, material fixes),
   while `gpu/video.cpp` is the NVK video renderer implementation on Switch; it is not responsible for decoding.
3. **masseffect-nx (the closest working reference)**: `docs/audio-and-video.md`
   states explicitly that Mass Effect's movies are also Bink (`Movies/*.bik`), **played by the game's
   own recompiled player, with the SDK not on the video path** (the decoder runs on
   Switch after being recompiled along with the XEX; the opening logo measures about 30 fps on device). What the SDK needs to handle is **rendering-side**
   texture adaptation: Bink outputs LINEAR texture planes rewritten by the CPU every frame
   (k_8 1280x720 / 640x360), and `masseffect_native_draws.cpp` keeps a full hash/upload every frame for these
   "CPU-written video planes without coherency events",
   not participating in texture-coherency skipping; `me_resolution.cpp` states that Bink movies do not use internal resolution
   scaling. This "keep the original decoder + adapt texture upload" route shares the same architecture and engine as T3DOTM,
   and is the most directly reusable option.
4. **T3DOTM video assets**: 38 files under `game/TransGame/Movies/` (about 30
   `.bik`): opening logos (Activision/Hasbro/HighMoon/Paramount), startup screens
   (TF_InitialStartup + multiple languages), cutscene FMVs FMV_* and L6_OptimusBroadcast,
   and loading screens (LoadingScreenAlpha / TF_LoadingScreen + multiple languages).
   The generated code and registry contain **no** recognizable symbols such as Bink/WMV/PlayMovie/ShowMovie/MoviePlayer/
   CutScene/Cinematic (only `sub_xxxxxxxx` and
   display-mode APIs such as `VdSetDisplayMode`/`VdQueryVideoMode`/`XGetVideoMode`),
   and the Bink decoder exists as unsymbolized internal functions.

**Options (not implemented yet; to be chosen after on-device confirmation of whether video playback really crashes)**:
- **A (recommended, following masseffect-nx): keep the recompiled Bink decoder and add rendering-side adaptation**:
  video decoding will most likely run on Switch along with the recompiled code; if on-device Bink texture upload/draw
  behaves abnormally, follow `masseffect_native_draws.cpp` to force hash/upload for the linear video planes rewritten every frame,
  and check video plane clipping and viewport (pixel-space draws).
- **B. Skip/disable video playback**: after locating the Movies playback entry point (UE3 MoviePlayer/Bink
  calls), skip it directly — the fastest way to avoid decoding performance and compatibility issues; first confirm these calls are not on
  the loading critical path (masseffect's Coalesced.ini skipping the intro is the same kind of approach).
- **C. Native Bink decoding replacement**: following the nfsmw-nx idea, intercept
  `.bik` opens/reads at the IO layer or app layer, decode with FFmpeg's Bink decoder on the Switch side and write back frames;
  this requires introducing FFmpeg (`BINK_DECODER=0` is not enabled in the current config) and implementing a rendering bridge,
  it is the largest effort and is considered only if route A fails.
- All FATALs in round 26 so far are ordinary unregistered indirect dispatch points (batch 24 already covers the 0x832F5648
  region), and no crash evidence clearly pointing at the `.bik` decoding path has appeared; video-related crashes need to be located with
  concrete logs from video playback scenes (opening logo / cutscene / loading screen).

### Round 27/28 Log Analysis and Video Loading Fix (2026-10-20)

**Round 27 (T3DOTM_001(27).log, 2026-10-19 23:58)**: FATAL
`Call to invalid or unregistered function at guest address 0x833D6FC8`.
This address was already registered in batch 23 (`overrides.toml` line 166,
`SetFunction(0x833D6FC8, sub_833D6FC8)`, `T3DOTM_recomp.58.cpp`), and
the round 26 (22:10) log already passed this point — so round 27 should be a **reproduction on an old build** (an NRO from before batch 23 was still
placed on the SD card); the current batch 24 build already covers it, with no new declaration needed.

**Round 28 (T3DOTM_001(28).log, 2026-10-20 05:47)**: new crash type
`Unhandled guest access violation: read of guest 0x00005660 (host
0x0000003275FD3660) on thread 0xF8000028`. This is guest code reading a near-NULL
address (0x5660 = NULL + a 0x5660-type field offset), not an unregistered-function FATAL, so overrides
cannot cover it. Timeline: video load attempts (05:47:20-21, all FAILED) → streaming load of
game assets (WL_* / TR_* / WEP_*, 05:47:22-29) → 15-second IO statistics (:35) →
crash (:37), **the crash point is unrelated to the .bik video decoding path** (the video files never opened successfully).
The root cause is most likely a null-pointer/heap-state problem on the front-end loading path, consistent with rounds 26/27 showing different crash points
within the same window (17-30 seconds after UI load), suggesting a multithreaded race or heap
corruption; whether the current batch 24 build and the alias fix eliminate this path requires on-device retest confirmation.
If it reproduces, the next step is to add guest PC recording to the SDK's `Memory::AccessViolationCallback`
(`sdk/src/system/xmemory.cpp:558`) and then rebuild, so that the
specific function can be located.

**Video loading fix (update/-layer .bik aliases)**: on-device logs show the game searches for opening/loading videos on startup in the
order `TF_InitialStartup_360_INT.bik` → `_NA_INT` → `_INT` → `_360` (loading screens follow
the same rule), but the disk `game/TransGame/Movies/` contains only the base name
and DEU/ESN/FRA/ITA variants, with no INT/360 variants, resulting in about 250+ `NtCreateFile FAILED`
per startup (the game skips them tolerantly, so videos never actually played before, nor
did they ever enter the Bink decoding path). This batch adds 8 alias
files under `update/TransGame/Movies/` (TF_InitialStartup / TF_LoadingScreen × 360_INT / NA_INT / INT /
360; content is a copy of the base-name .bik, identical md5, about 11.4 MB total). The VFS is a layered
overlay (update/ → patched/ → DLC/ → game/, see
`sdk/src/system/runtime.cpp:297` SetupVfs), so a hit in the update/ layer lets
the game actually load and play the opening and loading-screen videos: this both eliminates the FAILED log noise and lets
the masseffect-nx conclusion (the recompiled Bink decoder can run on Switch, ~30 fps) be
tested on this game. If on-device video playback crashes, continue with option A above (rendering-side texture adaptation).
To temporarily disable video (back to the skipped state), just delete the `update/TransGame/Movies/`
directory on the SD card, without touching the game/ data.

## 7. On-Device Test Guide (NRO Retest)

The current NRO (`recomp/out/sw8/T3DOTM.nro`, non-LTO diagnostic build, 2026-10-20 07:44,
batch 24 fixed version, 109,557,774 B / 0x687B80E, verify_nro.py Verdict: PASS)
is the version after the round 26 crash fix; **the round 27/28 logs are all earlier than this build** (
23:58 last night / 05:47 this morning, while the NRO was built at 07:44), so be sure to retest with the NRO in this package,
and do not reuse the old NRO on the SD card.

- Put `recomp/out/sw8/T3DOTM.nro` into the SD card's `sdmc:/switch/Transformers3DOTM/`,
  alongside the `game/`, `update/`, `patched/` sibling directories (see the layout in section 1).
  `update/` also contains the `TransGame/Movies/` alias .bik added in this batch (the opening/loading video
  fix); if you do not want video playback, just delete that directory.
- Launch via Homebrew Menu; expected: enter rendering (`modo: normal`) and pass the 0x833CF038,
  0x8334D6A0, 0x832F7068, 0x8334D5C8, 0x833D6FC8, 0x832F5648 large-function areas.
- **Video playback (new verification point in this batch)**: the opening Publisher Logo / `TF_InitialStartup`
  and the loading screen `TF_LoadingScreen` will actually load and play (previously all
  skipped due to filename mismatch). Watch whether the video is black/ crashes / stutters; masseffect-nx, same architecture, measures Bink
  playback at about 30 fps on device; if T3DOTM playback is abnormal, adapt rendering per option A in "Video Playback Investigation Record".
- If a round 28-type crash reproduces (`Unhandled guest access violation: read of guest
  0x00005660`), report the log and note the screen/stage seen before the crash; if the new crash is
  an ordinary unregistered-function FATAL (`Call to invalid or unregistered function at guest
  address 0xXXXXXXXX`), just add the address to `overrides.toml` and rebuild.
- Pay special attention to video playback scenes such as the opening Logo / cutscenes / loading screens: if
  a crash occurs at these moments, record the crash scene and report the log (see "Video Playback Investigation Record" above).
- Retest report contents:
  - new `rex_crash.log` / `rex_perfil.log` / `rex_stderr.log` /
    `T3DOTM_*.log` in the program's run directory;
  - whether `sdmc:/atmosphere/crash_reports/` and `sdmc:/atmosphere/fatal_errors/`
    have new reports (the faulting PC directly determines the root cause).
- If the new crash is still `Call to invalid or unregistered function at guest address 0xXXXXXXXX`,
  it means a new unregistered indirect dispatch address: add that address to `recomp/overrides.toml`
  (refer to the judgment method in the batch 14 comment: whether that address in the patched xex is a real thunk/function body),
  then rerun `bash recomp/codegen_t3dotm.sh` + `bash scripts/build_and_verify.sh --no-lto -j 4`.

## 5. Performance Verification Criteria

- **No overclocking**: only request the handheld-mode performance profile through the official APM API (GPU 307.2 → 460.8 MHz,
  RAM stays at 1331.2 MHz, config 0x92220008), without touching KIP / sysmodule; memory frequency is not raised.
- **Internal resolution**: 1280x720 (the guest video mode defaults to `video_mode_width=1280`,
  `video_mode_height=720`); output follows the actual display resolution (docked 1920x1080 / handheld 1280x720).
- **Verification method**: on-device frame time statistics, targeting a stable 60 FPS — watch the proportion of frames
  >33.3 ms (dropping to the 30 FPS profile) and >50 ms (dropping to the 20 FPS profile), to judge whether performance meets the target.

## 6. Known Notes

- The TU container is in LIVE STFS format; the project ships `tools/xenia_live_unpack.py` to unpack it (xextool cannot).
- XEX decryption parameters must be based on the actual results from this project's `tools/xex_analyze.py` run against `game/default.xex`
  and the TU container; do not reuse pre-analyzed parameters from other sources.
- xex_patcher is a native Windows program, with the path passed in by `setup_update.sh` via `cygpath -w`;
  the patch length of a header delta patch must use `size - offsetof(Xex2OptDeltaPatchDescriptor, info)`
  (see `tools/xex_patcher_cli/src/xex_patcher.cpp`), otherwise fusion fails.
- Characteristics of the xex after fusion: XEX2 magic; headerSize=0x4000; FILE_FORMAT_INFO is encryption/compression
  NONE/NONE; PE DOS header `MZ\x90\x00`, machine 0x1F2 (PPC), 15 sections;
  `security.image_size + header_size == file_size`.

### Crash Fix Record (2026-10-21, batch 26)

**Symptom**: round 29's 7 logs (`T3DOTM_001(35).log` 17:58:42,
`T3DOTM_002(33).log` 18:02:20 + 2 Atmosphere crash reports + rex_crash +
rex_perfil + rex_stderr) confirm passing batch 25's 0x82E869A0, but all stop at the
new address `0x827DC208`: both T3DOTM logs record
`[FATAL] Call to invalid or unregistered function at guest address 0x827DC208`;
the two Atmosphere reports (Program ID `0540c4720b100000` / `05e2b6d3eb447000`)
Type `User Break`, PC = `T3DOTM + 0x547dea4`, ReturnAddress[01] =
`T3DOTM + 0x2a61c8`, consistent with rex_crash's stack top `imagen+0x2a61c8`, breakpoint 4 bytes
(`svcBreak(0x0, addr, 4)` → runtime handle_fatal), confirming it is still an unregistered indirect
dispatch FATAL with the address advancing.

**Root cause**: `0x827DC208` falls in the gap between the registered `sub_827DC1D8` (a UE3 object reference/count thunk,
ending in a bctr indirect dispatch) and `sub_827DC2D8`; in patched/default.xex
this address is a standalone `blr` (a 4-byte empty-return stub, immediately after the tail of sub_827DC1D8)
, an empty slot entry (empty virtual method slot) in a UE3 vtable/dispatch table,
of the same kind as 0x833D6FC8 registered in batch 23. A runtime bctr indirect dispatch landing here triggers
a FATAL.

**Fix**: append the batch 26 declaration to the end of `recomp/overrides.toml`:
`"0x827DC208" = { size = 0x04 }`; after codegen it generates
`DECLARE_REX_FUNC(sub_827DC208)` / `SetFunction(0x827DC208, sub_827DC208)` /
`DEFINE_REX_FUNC(sub_827DC208)` (a blr empty return).

**Artifact**: `recomp/out/sw8/T3DOTM.nro` (2026-10-21 21:11, 108,668,942 B,
verify_nro.py Verdict: PASS; aarch64-none-elf-nm confirms the sub_827DC208 /
sub_827DC1D8 / sub_827DC2D8 symbols are all compiled into the ELF). Delivered copies:
`T3DOTM_dispatch_fix2.nro` + `README_dispatch_fix2.md`.
**Known observation**: in rex_stderr, `RexGmInit: Direction F map FAILED
(test_dst=0x1e92cf9000, rc=0x0000D401)` is the existing fallback for when whole-segment mapping of code-memory is unavailable,
degrading to per-page fault-in; it has no direct causality with this FATAL and is listed as a performance
observation.
**Pending verification**: on-device retest confirms passing 0x827DC208; if a new unregistered indirect dispatch address appears,
append an overrides.toml batch by the same procedure and rebuild.
