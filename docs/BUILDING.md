# Building ootgz (the native, in-game version)

sloperinaGZ (source: `src/ootgz`) is ootmenu rebuilt the way gz is built: C code that the game loads
and runs itself, on its own thread, once per frame. No emulator attachment, no Python at run time.
The Python overlay it replaced (`ootmenu.pyw`, `game.py`, `patch.py` ...) is archived outside this
folder (`ootmenu-python-overlay-2026-11-09.tar.gz`); the tables below still name its files.

## Prerequisites

- [devkitA64](https://devkitpro.org/wiki/Getting_Started) (`DEVKITPRO` set; provides
  `aarch64-none-elf-gcc` and `elf2nso`) and a host C compiler, **or**
- Docker: `./docker-make` runs the Makefile in the `devkitpro/devkita64` image.

## Building

```
make            # or ./docker-make
make dist       # or ./docker-make dist
```

Outputs:

| File | What |
|---|---|
| `bin/subsdk9` | the shared module (NSO), added to the game's exefs |
| `bin/33FA6C2C23848282A77E8B88A0A93F4887C55920.ips` | 1.0.0 main patch |
| `bin/E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips` | 1.1.1 main patch |
| `dist/` | install trees, see [INSTALLATION.md](../INSTALLATION.md) |

The link fails if the module imports a symbol that isn't in `lib/imports.txt` (the functions main
and sdk are known to export). A function we call but forgot to define would otherwise link as an
import and only fail when the game loads the module. Add a symbol there only after checking that
main or sdk exports it.


## Layout

Mirrors gz (`gz/src/gz`, `gz/lib`, the Makefile's version list):

| Path | gz equivalent | What |
|---|---|---|
| `src/ootgz/z64hd.h`, `z64hd_builds.h` | `z64.h`, `lib/liboot-*.a` | shared structure layouts and a table of main offsets per build |
| `src/ootgz/ootgz.c` | `gz.c` | module init, frame hook, menu tree |
| `src/ootgz/crt0.S`, `lib/ootgz.ld` | `hdr.S`, `ldr.S`, `gz.ld` | module header (MOD0) and linker script |
| `src/ootgz/menu.c`, `gfx.c` | `menu*.c`, `gfx.c` | menu model; text screen shared with the renderer (log fallback) |
| `src/ootgz/nvn.c`, `font.c` | `gfx.c`, `res/` fonts | drawing over the game's picture through its NVN; gz's Press Start 2P font |
| `src/ootgz/got.c` | | redirects main's imports (NVN, nn::hid) |
| `src/ootgz/input.c` | `input.c` | controller input via the game's `nn::hid` |
| `src/ootgz/gamedata.c` | (z64 save context) | GameData fields (port of `game.py`) |
| `src/ootgz/pouch.c`, `job.c` | `gz_inventory.c` | inventory through the game's own code (port of `patch.py`) |
| `src/ootgz/cheats.c`, `link.c` | `gz_cheats.c` | cheats; Link, camera, frame counter |
| `src/host/mkips.c` | `genhooks`, `gru` scripts | writes the IPS patch from the same address header |

## How it loads

The emulator or LayeredFS adds the shared `subsdk9` and applies the IPS whose filename matches
main's build ID. Both patches can be installed together.

1. **`subsdk9`** is linked by the game's rtld, which resolves the import of `nnMain` and runs
   `ootgz_module_init` via `DT_INIT`. The module identifies the build using the exported
   `nnMain` address and its matching MOD0 header offset.
2. **The IPS** writes a marker with the patch format and game revision, a stub in executable
   padding, and a branch from the GameData unlock site to that stub. The stub loads the hook
   pointer from the build's reserved slot and falls back to the original unlock until the
   pointer is set.
3. The module validates the marker, patched branch, and empty writable slot before installing
   the hook and using the selected table. An unknown build or a missing or mismatched patch
   leaves the hook inactive.

The module runs exactly where the old assembly hook ran: the game's thread, once per frame,
with GameData just flushed and unlocked. So everything `patch.py` did through the mailbox
(AddPouch, remove, gear, screen refreshes) is now a direct call; multi-step changes run one
step per frame through `job.c` because SetInt is queued until the next flush (confirmed from
the disassembly of `main+0x163b70`).

What changed from the Python version:

| Python (emulator) | Native |
|---|---|
| finds guest base + main by scanning Lichii's process | `&nnMain - build->nnmain`, checked against MOD0 |
| finds GameData Int/Bool/Enum/String tables by heap scans | type entries inside the GameData manager (Int at `+0xC8`, String64 at `+0x608`) |
| one mailbox request per frame, polled by a worker thread | direct calls; `job.c` keeps one step per frame |
| XInput/keyboard on Windows | `nn::hid::detail::GetNpadStates` from the game's sdk |
| Tk overlay window | `gfx.h`: log backend now, on-screen backend next |
| equipment component / magic meter: heap scan | same idea, in-process (`mem_find_u64`), after a cheap search near Link |

## Verified in Lichii (2026-11-09)

- rtld loads `subsdk9` and calls `DT_INIT`; the IPS applies; the frame hook runs
  (`ootgz: frame hook installed`, `ootgz: running (patch v1, main 0x805e3000)`).
- Input through `nn::hid::detail::GetNpadStates` works: R + right stick down opens the menu,
  A enters a submenu.
- String64 fields (map name `Market_Child`), Link's health (80/80) and drawn position read
  correctly. No crash.
- Not yet tried: upgrades, ocarinas, cheats, input interposition, the slot watchpoint.

## Verified statically

- The module builds with no warnings; three page-aligned segments, MOD0 at rodata start,
  `DT_INIT` = `ootgz_module_init`, only `RELATIVE`/`GLOB_DAT` relocations, nothing exported.
- All 9 imports exist in the game: `nnMain` in main; `memcpy memmove memset snprintf vsnprintf`
  and the three `nn::hid::detail::GetNpadStates` overloads in sdk (checked against the
  modules' dynamic symbol tables).
- The IPS disassembles to the intended stub; its site branch is byte-identical to the old
  patch's `SITE_BYTES`.
- `murmur3` matches the Python one (`SimpleIntForMessage3` = `e670579a`,
  `Sequence_CurrentBanc` = `1d6189da`).
- The slot `main+0x4e9a4d0` is past the bss end MOD0 declares (`0x4e9a4c8`), inside what the NSO
  header maps (`0x4ea0000`), and zero in two live dumps.

## Unverified: check these first, in Lichii

Lichii runs the game's own rtld, so a working load there is good evidence for hardware.

1. **The module loads and the hook runs.** Remove the old `load/0400486024DEC000/ootmenu`
   patch first (same site). Turn on Lichii's log with the emulated debug output category at
   Debug level (yuzu-family logs `svcOutputDebugString` under `Debug.Emulated`). Expect
   `sloperinaGZ: frame hook installed` at boot and `sloperinaGZ: running on game <version> (patch v2, main 0x…)` once in game.
   If nothing appears, check whether rtld calls `DT_INIT` for subsdk modules on this system,
   and whether `__nx_module_runtime` (0x200 bytes) is enough for the Switch 2 rtld.
2. **The slot is really unused.** Set a write watchpoint on `main+0x4e9a4d0`..`+8` with the
   GDB stub (as `tools/dpadwatch.py` does) and play a few minutes. Only ootgz should write it.
   If the game does, pick another address and change `Z64HD_HOOK_SLOT`.
3. **Input.** Open the menu with **R + right stick down**; the log prints the menu. If nothing
   reacts, check the `npad_state_t` layout in `input.c` (buttons at +8, attributes at +0x20).
4. **GameData types.** Bool/Enum/Float entries share the Int entry's layout (table size at
   `+0x24`). String64 uses the game's hash table and follows the value pointer from its
   object. The link menu shows the map name if strings work.
5. **Features.** Compare with ootmenu: upgrades (wallet first, it's the verified one), ocarina
   give/remove, the cheats.
6. **Drawing and hidden input** (see "Drawing" below): expect `NVN hook installed`,
   `input hook installed`, then on the first menu or notice `swapchain: N textures, WxH,
   format F` and `drawing on screen: ...`. Any `on-screen drawing off: <why>` line says what
   failed; the menu then keeps going to the log.

## Drawing

- **Hooks.** At module init, `got.c` rewrites main's GOT slots (found through main's MOD0 and
  dynamic section) for `nvnBootstrapLoader` and the five `nn::hid::GetNpadStates` overloads.
  Checked offline: the slot found for `nvnBootstrapLoader` is `main+0x4cbf710`, the one main's
  PLT stub at `main+0x3c42a90` jumps through. This doesn't depend on rtld's search order.
- **NVN.** The game gets `nvnDeviceGetProcAddress` from the bootstrap loader and fetches every
  function by name through it (loader at `main+0x129e000`), so the wrapped GetProcAddress
  swaps in our `nvnQueuePresentTexture`, `nvnWindowBuilderSetTextures` and
  `nvnDeviceInitialize`. Before each present the menu panel (rasterised on the CPU, in the
  swapchain's format) is copied into the texture being presented with
  `nvnCommandBufferCopyBufferToTexture`. No shaders, which Switch 2 would need built with
  non-public tools. The panel is opaque.
- **Checked against the game:** `WindowBuilderSetTextures` is called as `(builder, count,
  textures)`; `TextureBuilderSetFormat` gets 37 (RGBA8), 1, 13, 61 (RGB10A2) and
  `MemoryPoolBuilderSetFlags` 0x21/0x12/0x22/0x24/0xA1, all NVN 1 values. Not checked: the
  present call's arguments (no direct call site found) and the signatures of the pool,
  command buffer and sync functions (NVN 1's are used). Supported swapchain formats: RGBA8,
  RGBA8/RGBX8 sRGB, RGB10A2, RGBA16F.

## Ported from ootmenu

link (health, rupees, position), warps (all places, arrival cutscenes, cancel), cheats,
inventory (items, ammo, capacity, magic, bottles, upgrades, quest items, songs, dungeon items,
small keys, skulltulas), equips (worn gear, owned gear, masks), story (quest stages, tutorials),
watches (on-screen, timer, lag counter, gz's R + D-right / R + A + D-left / R + B + D-left /
R + B + D-right). The menus' data (field hashes, stages, warps) is `src/ootgz/data.c`,
generated from the JSON files by `python tools/gendata.py` and committed, so the build needs no
Python.

Warps start at once (unconfirmed in game): the module requests the event an exit requests,
`Dm_SY_BancChangeGo` with `NextBanc` / `NextStartPos` / `NextStartPosAir` / `BootEventName` /
`IsCheckAir` (found in the map layouts: exits are `FieldAndDungeonEntranceEventRequestActor`,
`FirstEventName` = `Dm_SY_BancChangeGo`). That flow's `GameSystemActor.EventReserveBancChange` is an
AI module (`romfs/Pack/Actor/GameSystemActor.pack` › `AI/ReserveBancChange.module.ainb`) that writes
the `Sequence_Reserved*` GameData (String64 indexes 0x20-0x23 are `ReservedBootEventName`,
`ReservedEventStarterName`, `ReservedNextBanc`, `ReservedNextStartPosName`) and sets
`Sequence_ControlInEvent` = `ChangeBancOnEvent`, which the Event sequence turns into the map change.
The request is built like the `SeqOneShotRequestEvent` node does (`main+0x32db1a0`): constructor
`main+0xd79f10`, params as `QueryBancChangeEventRequestArg` fills them (`main+0x29bf380`), submit
`main+0xd7a820(**[main+0x4c7e090], &request)` in 1.1.1. The layout is in `z64hd.h`. If the game hasn't
reserved the map within 90 frames, the warp falls back to swapping the next exit Link takes.

## Next steps

1. **Not ported yet**: x/y/z editing and teleport, steering while levitating (Link's physics
   record is now found by `levitate.c`: a pointer search from his actor plus a test lift; the path
   it logs can become a direct chain), the air wall view, custom memory watches.
2. **Levitate / teleport** need Link's physics record from a pointer chain, not a search
   (`game.py` "position").
3. **Air wall view**: needs 3D line drawing (a shader, or lines rasterised on the CPU); last.

## Installing

See [INSTALLATION.md](../INSTALLATION.md).
