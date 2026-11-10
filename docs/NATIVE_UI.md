# Native UI: drawing the menu the way the game draws its own

Goal: the menu, the watches and the input display look like the game's own UI (its font, its button glyphs, its dark
info boxes and bands), instead of gz's pixel font in opaque boxes. Work in progress on the `native-ui` branch.

## Approach

Draw with the GPU, just before the game's present (where `nvn.c` copies the panels in today), using the game's own UI
shader, font and textures, all loaded from the game's RomFS at start-up:

- **Same pixels as the game's UI**: the shader the game draws its menus with, its textures and its font.
- **Works when the game doesn't run**: drawn at present time, not in the game's frame, so it can keep working while the
  game is held (pause, frame advance), in loading screens and cutscenes.
- **No new game addresses**: the shader, font and textures come from files; NVN comes through the `nvnBootstrapLoader`
  hook `got.c` already has. The same code serves 1.0.0, 1.1.1 and later updates.
- **Nothing in the game changes**: no state in the game's UI system. Memory comes from the game's heap at start-up
  (fonts, shader, GPU buffers, and the game's pictures: about 10 MB while they load, 4 MB kept).
- **Anything can be drawn**: gz's checkboxes, the memory viewer, the keyboard, the collision view (lines, which the
  copy path can't draw).

Driving the game's own UI library (Layout2) instead was looked at and set aside: it has ready-made parts
(`PaOptionBtn`, `PaOptionBtnLevel`, `PaNumberArrow`, `PaScrollBar`, `PaGuideKey`...), but main has no symbols or RTTI
for it, its layouts (`.buil`, magic `BFFH`) have no public tools, every function would need an address per game
version, it allocates in the game, and it updates and draws inside the game's frame, so it would freeze exactly when
the menu is used during pause and frame advance.

Nothing from the game goes into this repository or a release: the module reads the user's own game files.

## What the game provides

All under RomFS. `.zs` files are zstd with the dictionaries in `Pack/ZsDic.pack.zs` (a zstd SARC of `*.zsdic`;
the frame header names the dictionary: 1 = `zs.zsdic` for the files below).

| What | File | Format |
|---|---|---|
| UI font | `Font2/Scalable/ClarimoUDPE-Regular.Nin_Ounce_NVN2.bfotf.zs`, `-DemiBold`, `-Light` | scrambled OpenType (CFF, `OTTO`) |
| Button glyphs | `Font2/Scalable/External.Nin_Ounce_NVN2.bfotf.zs` | scrambled OpenType, private use area |
| Zelda glyphs | `Font2/Scalable/Nin-ZeldaGlyphs-v3-Plain.Nin_Ounce_NVN2.bfotf.zs` | scrambled OpenType |
| UI shader | `Layout2Shader/MergeShader.Product.100.Nin_Ounce_NVN2.bfsha.zs` | BFSHA 9.1.1, one BNSH |
| UI textures | `UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs` (65 MB, 244 MB unpacked, no dictionary, 8 MiB window) | BNTX, platform tag `Ounc`, 878 textures |
| UI layouts | `UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs` | SARC of `.buil` (`BFFH`), 157 layouts and parts |

**Font scramble**: `u32 magic, u32 size, font`, all big-endian words XORed with one key. The key is
`word1 ^ (file size - 8)`; `word0 ^ key` is then `0x7F9A0218`. For this game's fonts the key is `0xA6018502`.

**Button glyphs** (`External`): A `U+E040`, B `E041`, X `E042`, Y `E043`, L `E044`, R `E045`, ZL `E046`, ZR `E047`,
power `E048`, D-pad `E049`, HOME `E04A`, + `E04B`, − `E04C`; the sticks with a direction, left then right:
down `E050`/`E051`, up `E052`/`E053`, left-right `E054`/`E055`, press `E056`/`E057`, right `E058`/`E059`,
left `E05A`/`E05B`, up-down `E05C`/`E05D`, plain `E05E`/`E05F`; D-pad up, down, left, right, up-down, left-right
`E060`-`E065`; arrows `E083`-`E08E`, close `E08F`, Triforce badge `E090`. That is the whole set: the font has 261
glyphs, but the unmapped ones are empty. There is one set for every controller (L and R as wide shoulder shapes, the
D-pad as four circles even on the Pro Controller), as the game shows them.

**Controller pictures** (in `Common.bntx`): `Controller_Pro`, `Controller_Grip` (two Joy-Con 2 in the grip) and
`Controller_Console` (handheld), 1160x680 ASTC 4x4 (sRGB), each with a darker, warm-tinted `...1` variant of the same
picture; `ControllerSh_*1` (BC4 shadows) and `ControllerGuide_*_00`-`04` (BC4, the controls screen's leader lines).
All Switch 2 hardware. The options menu's parts: `OptionBtnThinCursorLine_00` / `Glow_00` (BC4 masks, 640x173;
the game draws them round the selected control). The line's picture is a distance field: a bar whose alpha climbs
evenly from 30 px in from its edge to full 29 px further in, and the game cuts a crisp line out of it at a level.
So does this: the alpha mapped steeply round level 0.3 (`gpuui_image_mapped`, the shader clamps it), the picture
cut in nine at 48 px with its middle left out, scaled so the line is 1.6 font pixels thick, its middle on the
row's edges, with the same room round what's selected left and right as the row leaves above and below its
capitals. The glow's picture is a soft bar (nothing 14 px in, full at 72): cut at 72 it's a halo from the line
out, drawn blue-white as the game's. The options' cursor on full-size buttons (`PaOptionBtn_00`, a Layout2
`.buil` in `Common.blarc`) also has a ray effect (`CursorCmnRay_00` over `Kumo64` noise) left out here., `ScrollBar_00` / `ScrollBarBase_00` (ASTC; the thumb,
a grey grain the game maps to orange, on a dark track left of the list); `OptionCheckMark_00` is a corner mark, not a
tick. `SystemLine_00` (BC4 mask, 240x43): a curl, then a line tapering in by about 0.6 of it, the ornament the
game's info boxes have under their title (mirrored at the other end). The info box itself (an item's description
on the pause screen) is near black at about 78% over the game, with hard edges; `MenuMessageBase_00` is its mask. In the BNTX, a
texture record (`BRTI`) has its format at +0x1c, size at
+0x24, packaged layout (8 bytes, for `nvnTextureBuilderSetPackagedTextureLayout`) at +0x34, data size at +0x50, name
at +0x60, levels' data offsets at +0x70, channel sources at +0x58 (NVN's swizzle values); the data is
block-linear as the GPU reads it.

## The UI shader (`MergeShader`, shading model `ui`)

121 programs, chosen by 55 static options (`PaneType`, `Texture_ActiveCount`, `Texture_<n>_<n+1>_BlendColor`,
`SlicingType<n>`, `o_IsProceduralShapeEnabled`, `o_IsProceduralShapeRoundedCornerEnabled`,
`o_IsProceduralShapeInnerShadowEnabled`, `o_IsCaptureEffectBlurType`, `o_TextureMappingType<n>`,
`o_OpacityToColorMode`...). Its interface:

- attributes `_p0` (position), `_u0` (texture coordinate);
- uniform blocks `view` (144 bytes), `UiMaterial` (64), `proceduralshapeshadow` (64: params, colour, attenuation),
  `CaptureEffectBlur` (256), `UiMaterialFont` (16);
- storage buffers `UiMaterialBuffer`, `textCharacterParam` (text is drawn per character from it);
- samplers `sysTextureSampler0`-`5`, `sysTextureSamplerOptional`, `sysTextureSamplerOptionalBitmap`.

Read with [KillzXGaming/ShaderLibrary](https://github.com/KillzXGaming/ShaderLibrary) (its loader handles BFSHA 9).

## Status

**settings > style: the game's** (the default; `menu_style=0` in `settings.ini`, gz's pixel font is `1`).

- `gamefile.c` reads `content:` files and decompresses `.zs` with the game's dictionaries (`zstd.c`: zstd's
  educational decoder, errors jump back instead of exiting; big files a block at a time through their window,
  `zstd_stream_t`).
- `gamefont.c` loads Clarimo UD PE (regular, DemiBold) and External at start-up (game thread, about 4 ms) and
  rasterises glyphs with stb_truetype on the render thread.
- `uishader.c` loads `MergeShader` and finds two programs by their options' values (a quad in its corner colours;
  a texture times them). `gpuui.c` draws with them in nvn.c's present hook: quads, glyphs from an atlas, shapes
  painted once into the atlas, and the game's pictures as textures from their packaged data. The interface it found
  is at the top of `gpuui.c`.
- `nativeui.c` lays the panels out. The menu by what its rows are (`gfx_row`):
  - text set with its capitals centred in its row, as the game sets a button's (descenders hang below), and
    checkboxes, chevrons and glyphs on that middle;
  - the breadcrumb in the game's gold over the game's ornament line (`SystemLine`, a curl at each end); headings
    with a rule; taller item rows;
  - values in a column after the longest label, between chevrons; a chevron for what opens a menu; buttons named
    in labels and values ("(R + d-up)", "< L >") and in the hint as the game's glyphs;
  - the hint (`gfx_hint`) wrapped by its own type in the rows menu.c keeps for it, and no hint area in a menu
    that has no hints; a heading never ends the visible list;
  - the game's options cursor round the selected item, or the selected part of it (`OptionBtnThinCursorLine` /
    `Glow` as frames); the game's scroll bar left of a list that scrolls (`ScrollBar` / `ScrollBarBase`), its
    thumb inside the track with its grain mapped to two golds (`gpuui_image_mapped`, as the game's materials map a
    picture's black and white to two colours);
  - the controls at its foot as the game's guides have them, "select [L]  return [R]+[D-left]  close [R]+[R-stick
    down]";
  - lines whose columns matter (the memory viewer, the keyboard, typed digits: `menu_item_t.grid`) keep gz's grid.
  The menu sits in a box as the game's info boxes: near black, solid, hard edges, a padding round what's in it.
  Its opacity is the background opacity setting on a curve (1 - (1 - o)^2) that puts the default 60% at 84%,
  about the game's box. The menu is as wide as what's in it.
  The watches: labels dim, values right-aligned. The log fades row by row. The watches sit on a band as the game's
  control guide (`CtrlGuideBG`): fading out left and right, its top and bottom straight, at the background
  opacity setting; the panel is the band, and what's in it keeps a padding to where the band starts to fade.
- Motion: the menu keeps its top-left corner. Going into a menu or back, its box grows or shrinks to the new
  size (180 ms) while the old menu fades out (90 ms) and the new one fades in with an 8-pixel slide from where it
  came (170 ms), on a strong ease-out, taken up from where it is when the next change comes first. Opening and
  closing the menu and moving in a list don't move (they're done all the time).
- The input display (controller style) shows the game's picture of the controller in use (`input_controller`: the
  npad style a button or a stick well off centre came from last), on the watches' band when its background
  option is on (`input_display_background`), else on the game's picture alone. The sticks' caps move
  with the tilt (cut from the picture a row of pixels at a time, their place filled with the grey under them) and
  look pushed in while clicked, their tilt written under them; what's held is lit in its own shape (the Pro
  Controller's cross arms and shoulder bands, round buttons as discs); ZL and ZR, which the pictures don't show,
  are the game's glyphs above it, and so are the Joy-Cons' L and R (a thin line at their top edge in the
  pictures). GL and GR are the system's to assign (`nn::hid::GetGlGrButtonConfigState`): pressed, they light the
  button they're set to. Buttons this module doesn't know are logged once, to learn on a console whether a game
  ever sees them as their own.
- `gameimage.c` loads the game's pictures (the controllers, the cursor, the scroll bar, the ornament line) from `Common.bntx` a few
  milliseconds each frame from start-up (about 160 MB unpacked, until the last of them; 0.37 s in all on a Mac).
  Until then the menu has its own selection band, a plain line under its title and ^ / v marks, and the input
  display its glyphs.
- The items' icons (`UI/Tex/Icon/<name>.bntx.zs`, one ASTC 4x4 texture each, 341 to 1366 px, about 13 MB
  in all) would take more of the game's heap than it has to spare (it refused them past about 11 MB). So
  `gameimage.c` decodes each as it's read (`astc.c`, the LDR part of texture2ddecoder's ASTC decoder, checked
  pixel for pixel against it), puts its blocks in place and shrinks it to 64 px into one RGBA atlas (8 x 6
  slots, 1.6 MB with its texture), which gpuui brings up as slots fill. The packaged layout is block linear:
  GOBs of 64 bytes x 8 rows, 2^layout[0] of them stacked in each column; on the Switch 2 (BNTX platform
  "Ounc") a GOB's 16-byte sectors go row bits 0-1, column bit 0, row bit 2, column bit 1 (Switch 1: row 0,
  column 0, rows 1-2, column 1), found by matching decoded blocks against the GPU's own drawing of the bomb.
  Item menus (cheats, items, equipment, collection, capacities, magic, ammo) put the picture before the
  label (`menu_item_t.picture`, `GFX_PICTURE` + its number in the line); an option of items (ocarina,
  hookshot, big sword, gauntlets, scale, magic meter) shows the chosen one's (`choice_pictures`, "none"
  the first's). Each picture's covered part (its alpha) is fitted inside its row, within the cursor's line; it's drawn a few times a fraction
  of a pixel apart, each at 1/k (their average), so it doesn't break up into noise.
- The cursor's motion follows the game's `PaOptionBtnL_00` animations (60 frames a second): `Select` (8
  frames: shown at once, alpha 0 to 1 over 4, its line and glow from 0.4 and 0.6 thinner to full, brightness
  0.74 to 1 easing out), `UnSelect` on the item left (14 frames: alpha 1 to 0 over 8, thinning back), and
  `CursorLoop` while it stays (a 40-frame loop of its material's values, a few to 25 %: here the glow dims to
  0.88 and widens by 1/8 and back). Not moving within a menu: a menu change, or opening it. A whole item's
  text changes with it: its selected look (bold, white) fades in over 4 frames as the row grows from 0.96
  to full over 8 (`gpuui_scale` about its text's start), the one left fades back as it shrinks. The gap under
  the title is taller (11 font pixels, its ornament line 0.42 down it) so the cursor's glow on the first
  item stays clear of the line.
- The game's layouts (`Common.blarc`, a SARC of `.buil` files, `Nintendo.Ui.ResModels.LayoutResData`):
  a header of tables `{u32 count, u32 8, u64 a, u64 b, u64 c}` at 0x130 (panes, materials, ...), 0x230 the
  animations (names at the u64 at 0x238, records at 0x240). An animation record: name, targets (each a
  4-byte descriptor, then u16 curve / u16 constant index, then a byte of curves, a byte of constants, a
  property byte), curves (keys' frames and (value, slope) pairs, cubic between them) and constants. Its
  materials' values are a block of floats after the material's texture list. Tools in
  `sloperina-ui/scripts` (`sarc.py`, `buil_annot.py`, `buil_anim.py`).
- Without the GPU path (gpuui couldn't start), the game's type is drawn on the CPU, opaque, on gz's grid.

Check the CPU drawing on the PC with `make nativepreview` and `obj/host/nativepreview <RomFS folder> out.ppm [scale]`.
Tested in nxnative on 1.0.0. Not yet tested on an emulator or a console.

## Next

- Test on a console and on emulators.
- Drop the per-frame GPU waits (`slot_finished`, `wait_gpu_idle`) now that nothing is read back in the game's style.
