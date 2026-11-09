# Collision view (shelved)

**Status: shelved (9th November 2026).** The collision files are fully decoded on the PC, but nothing is drawn in
the game yet. This page is everything needed to pick it up again: the plan, what it costs, what's unknown, and the
file format.

The goal: **scene > collision > show collision** draws the floors, walls and ceilings around Link (like gz's
collision view), coloured by kind, the way the air walls are drawn now.

## Contents

1. [The plan](#1-the-plan)
2. [Cost in the game](#2-cost-in-the-game)
3. [The unknowns](#3-the-unknowns)
4. [The decoder](#4-the-decoder)
5. [The file format](#5-the-file-format)

## 1. The plan

**Stage 1: a working view, drawn like the air walls (about a day, with a few test rounds).**

1. **Get the current map's collision.** Either find the `hknpCompoundShape` Havok already has in memory (the bytes
   are the same as the file's), or read `Phive/StaticCompoundBody/<Area>/<Map>[_Xn_Zn].bphsc` from romfs with the
   game's own file API when a map loads. The format has no pointer fix-ups (relative offsets only), so either way it
   can be used where it lies.
2. **Port the decoder to C:** `mesh_shape`, `compound_instances` and the ground-mesh offset from `tools/phive.py`,
   roughly 200 lines.
3. **Cull once per map load:** a per-instance bounding-box list (or the compound's `hkcdSimdTree`), and a coarse
   XZ grid of the triangles. Each frame only the cells around Link are looked at; the list of triangles to draw is
   rebuilt only when Link has moved about a metre.
4. **Filter and colour:** keep player collision only (the layer hit mask's Player bit), drop the Rough layer and
   sensors, skip AirWall material (the air wall view already shows it). Colour by the normal: floor (ny > 0.5),
   wall, ceiling (ny < -0.5).
5. **Draw the edges** with the air walls' method (rows of small scissored clears), with a small radius and a cap on
   the triangles drawn. Share each edge between neighbouring triangles once (deduplication roughly halves the lines).
6. **Menu:** under **scene > collision**, a "collision" heading with show, radius and perhaps floors / walls /
   ceilings checkboxes.

**Stage 2: fast drawing (several days, and it might not work out).** A real line or triangle draw on the GPU,
so thousands of triangles cost almost nothing. NVN needs shaders compiled by Nintendo's SDK compiler, which we
don't have. The routes, most likely first:

1. Reuse a simple shader the game has already loaded (a flat-colour one), with our own vertex buffer. Candidates:
   the game's own `debug_line_instanced`, `debug_triangle_instanced` and `debug_colored_mesh` (FINDINGS §9.2);
   whether they're loaded at run time isn't known.
2. Use the game's own debug drawing, if any survived in this build. Checked: compiled out. The debug draw nodes are
   empty and the physics debug draw manager is never created (FINDINGS §9.2).
3. Hand-build a compiled shader. Hard; not to be counted on.

Time-box the research for stage 2 before committing to it.

## 2. Cost in the game

- **Nothing when it's off**, like the air walls.
- **Finding the triangles (processor):** checking a whole map every frame would be far too slow (Kokiri Forest
  child has 1.56 million triangles). With the grid built at map load, each frame is cheap; the load costs a short
  burst of work and a few MB of memory.
- **Drawing (graphics):** the real limit. The density is high: around Link in Kokiri Forest there are about
  **4-9 thousand triangles within 5 m, and 14-22 thousand within 10 m** (the meshes are about as detailed as the
  graphics). The air walls' method is one tiny draw per dot, fine for dozens of boxes but too slow for thousands of
  triangles. So stage 1 needs a small radius (a few metres), a triangle cap and edge deduplication; a 10 m radius
  needs stage 2, or simplifying the meshes (vertex clustering at about 0.25 m is fine for a practice overlay).

## 3. The unknowns

- Whether the game keeps the compound in memory in a findable form, or sloperinaGZ must read the files. A place to
  start: the scene's StaticCompoundBodyMgr (FINDINGS §9.3).
- Which of the two versions of each mesh the player's character stands on: GroundHighResSmooth (simplified) or
  GroundHighResRough (detailed). See [the layers](#layers-what-to-draw). The decoder keeps Smooth for now.
- Moving collision (doors, blocks, lifts, chests) isn't in these files: it comes from actor packs (ShapeParam →
  Dcc `.bphsh`, or primitives as in `tools/airwalls.py`), and would need the actors' positions from memory.
- The sinking sand (desert maps) and water / lava volumes are in sections that aren't decoded.
- How to draw on the GPU (stage 2).

## 4. The decoder

`tools/phive.py` (standard library only; romfs paths go through `gamefiles.py`, so they need `zstandard`, see
`tools/requirements.txt`; already-decompressed local files work without it):

```
python3 tools/phive.py <romfs path | local file>      summary (.bphsh / .bphsc)
python3 tools/phive.py types <file>                    Havok type layouts of every tagfile in it
python3 tools/phive.py map <Area> <Map> [out.json]     world triangles of all tiles of a map (JSON)
python3 tools/phive.py obj <Area> <Map> out.obj        same, Wavefront .obj (opens in Blender)
   --all          keep sensors, enemy-only walls, the Rough layer and unplaced bodies (each tagged)
   --dir <dir>    read decompressed tiles from a local dir instead of the dump
```

Decoding the whole of Kokiri Forest in Python takes about 37 s (the JSON is about 60 MB); Hyrule Field (256
tiles) about 130 s. All 80 maps decode with no errors and no unknown shape types.

For checking: [objmap-oot.zeldamods.org](https://objmap-oot.zeldamods.org) shows the same collision, already
converted, on its map.

Sources: [Bphsh](https://epd.zeldamods.org/wiki/Bphsh) and [Common Phive](https://epd.zeldamods.org/wiki/Common_Phive)
on the EPD wiki. Nothing public documents `.bphsc`; its layout below was worked out from the files.

## 5. The file format

### Format

**Phive container** (all .bph* files): `"Phive\0"`, u16 version (0x0100 bphsh, 0x0202 bphsc), BOM FFFE,
u8 file type (0 shape, 1 navmesh, 2 static compound, 3 cloth), u8 N, u32 offset[N], u32 size[N], padded to 0x10
(the layout is as on epd.zeldamods.org/wiki/Common_Phive). bphsc has N = 160 (header 0x510), bphsh has N = 4.

**Havok tagfile** (SDK "20250200", newer than the 2023.2 documented on the wiki). Chunks have big-endian
`u32 size | flags<<30` and a 4cc; flag 0 = container, 1 = leaf.
`TAG0{SDKV, DATA, TYPE{TPTR, TST1, TNA1, FST1, TBDY, THSH, TPAD}, INDX{ITEM}}`. There's no PTCH: every
pointer is relative (hkRelArrayView = s32 offset from the field + s32 count; hkRelArray / hkRelPtr = s64 offset from
the field), so the data is position-independent. ITEM = 12-byte entries (u32 type index | flags<<24, u32 offset into
DATA, u32 count; flag 0x10 = an object, 0x20 = an array). The decoder dispatches on the ITEM type name of each
object. Varints in TNA1/TBDY use the usual Havok prefix encoding (0xxxxxxx, 10.., 110.., 11100.., 11101.., 11110..,
F8, F9). One quirk in TBDY: a member whose flags have 0x80 set carries one extra varint before its offset.
`phive.py types` prints the full layouts (hknpMeshShape, hknpCompoundShape, hknpShapeInstance, hknpConvexShape,
...). They match the wiki's hknpMeshShape table shifted by -8, because hknpShape is 0x38 bytes here.

**.bphsh** (`Phive/Shape/Dcc/*`, used by actors): [0] tagfile with one shape (hknpMeshShape so far),
[1] materials, 16 B each: u32 material (PhiveConfig MaterialCollection), u32 sub-material, u64 user-shape-tag mask.
[2] layer hit masks, 16 B each (u64, u64). The mesh's per-primitive shape tag indexes [1]/[2].

**.bphsc** (`Phive/StaticCompoundBody/<Area>/<Map>[_Xn_Zn]`): self-contained. It does **not** reference the
Dcc .bphsh files; the meshes are embedded. Sections:

| sec | content |
|---|---|
| 0 | 0xCAB0-byte header. +0x0000..0xC000 is a table I couldn't decode (probably a hash/lookup table). At +0xC000 there are 8 u32 counts: entity instances, sensor instances, entity compounds, sensor compounds, actor entries, instance infos (= the first two added), materials, layer masks. The tile name is at +0xCA20. |
| 1 | actor table, 32 B: u32, u32, u64 hash, u32 hash. For air walls the u64 is the placed actor's `Hash` from the .bcett (all 25 air walls in X1_Z1 match). |
| 2 | 16 B: two u64 (about half are placed-actor hashes). Not used. |
| 3 | u16 pairs. Not used. |
| 4 | instance info, 20 B: u32 index, u8 isSensor, u8 layer (LayerEntity- or LayerSensorCollection), u8, u8, u16, u16 index within its compound, u32 first material, u32 material count |
| 5 | materials (same as bphsh [1]) |
| 6 | layer hit masks (same as bphsh [2]). The first u64 is a mask over LayerEntityCollection, and bit 3 = Player. |
| 7 | 0x50 bytes, all zero in every tile seen |
| 11.. | one tagfile per layer: an `hknpCompoundShape`. Order seen: GroundHighRes, GroundHighResSmooth, GroundHighResRough, GroundObject, [extra entity bodies], then sensor compounds (StaticGeneralArea, InDoor, ForbidAutoPlacementArea) |
| then | optional water-volume-like block, a 0x30 ground-placement block, and a nested Phive .bphsh (the tile's ground/terrain mesh) |

Compound instances (hknpShapeInstance, 64 B): quaternion xyzw, translation, leafIdAndFlags, scale, u16 shapeTag,
relptr shape, userData. They are in **world space**. world = R(q)·(scale ∘ v) + t. **shapeTag indexes the
instance-info table [4]**, which gives the layer, sensor flag and material range. A triangle's material is
`materials[firstMaterial + meshLocalShapeTag]`; its local tag was below the material count for all 1627 instances
checked. Child shapes include hknpMeshShape, hknpBoxShape, hknpConvexShape, hknpCylinderShape, hknpCapsuleShape,
hknpSphereShape and nested hknpCompoundShape (all handled).

The ground mesh (the nested .bphsh) is in tile-local space. The 0x30 block holds scale xyz (1,1,1), a pad, then a
translation xyz at +0x18 (e.g. X1_Z1 → (-250, 0, -250)), then u32 6. Tiles are 250 m: X0..X3 = -500..+500 for
4×4 maps (8×8 maps such as DesertColossus span ±1000). This mesh is the terrain collision, so the TerrainArc .hght
files aren't needed for collision. Single-tile maps (dungeons, Market) have no ground mesh.

**hknpMeshShape**: `geometrySections` (0x40 each): sectionBvh, primitives (4 × u8 vertex ids), vertexBuffer
(untyped bytes), interiorPrimitiveBitField (2 bits per primitive, not needed), sectionOffset u32[3],
bitScale8Inv, bitOffset (these last two are for the 8-bit section BVH).
- Vertices: u16×3. **pos = (sectionOffset[i] + v[i]) · vertexConversionUtil.bitScale16Inv[i]** (bitScale16 = 512,
  so 1/512 m ≈ 2 mm). This matches the float AABBs of the top-level hkcdSimdTree exactly.
- When sectionOffset = (0x7FFFFFFF)×3 the section is **unquantised: hkFloat3 vertices** (12 B). These are rare
  and only seen in ground meshes.
- Primitive (a, b, c, d) gives triangle (a, b, c), plus (a, c, d) when d ≠ c (a quad).
- Shape tags: `shapeTagTable` = runs of (first primitive key, tag), ending with tag 0xFFFF.
  Primitive key = `section << 9 | primitive << 1 | quadHalf` (numShapeKeyBits = sectionBits + 9). Checked: the
  ground-mesh triangles tagged Water are 100% floor-facing and sit in narrow height bands, while Stone/Grass mix
  walls and floors.

**Convex shapes**: all carry an hknpConvexHull (float3 vertices, planes, faces {u16 firstIndex, u8 count}, u8 index
list); faces are fanned into triangles. The hull is the true surface shrunk by `convexRadius` (+0x68, usually
0.05 m), so drawn boxes are about 5 cm small. Capsule = 2 vertices + radius; sphere = 1 vertex + radius. Both are
drawn as 8-sided tubes with pointed caps.

### Layers: what to draw

PhiveConfig (Bootup pack `Phive/Config/PhiveConfig.byml`) gives the names; they are copied into phive.py.
- **GroundHighRes**: the main static geometry (rocks, buildings, props, baked air walls).
- **GroundHighResSmooth / GroundHighResRough**: the same instances, one-to-one with identical transforms, in two
  versions. Rough is detailed (e.g. 1266 triangles) and Smooth is simplified (447 triangles, with stairs and bumps
  smoothed). In the LayerEntityFilterTable, Objects ignore Smooth and collide with Rough, and Player/Character are
  marked as hitting both. **Unresolved**: which one the player's character controller actually stands on.
  `map` keeps Smooth and drops Rough by default; `--all` keeps both.
- **GroundObject**: small static objects.
- **Ground**: the nested terrain mesh (its material masks are all-ones).
- Sensors (StaticGeneralArea, InDoor, ...) are dropped by default.
- **Air walls are baked into the static compound** (material AirWall). Every air wall in data/airwalls.json for
  KokiriForest_Child (34/34) and SacredForestMeadow_Child (37/37) has an instance at exactly the same position. The
  per-material hit mask sorts them correctly: HitOnlyPlayer walls hit the player, while AirWallEnemy,
  CameraSight and CrawlHole walls don't. By default, `map` drops triangles whose mask lacks the Player bit.
- "Extra" entity compounds hold single bodies in local space at the origin (e.g. InsideTheDekuTree sections 15-17).
  They are probably placed at run time by an actor (moving platforms). They're flagged and dropped.
- Dynamic actors (doors, pushable blocks, lifts, chests) are not in the static compound. Their collision comes
  from actor packs (ShapeParam → Dcc .bphsh, or primitives as in airwalls.py).

### Validation

- **Kokiri Forest child**: the floors under 93 placed actors from the .bcett (pots, StartPos, signs, chests, lift
  rocks, grass) were checked. **89 of 93 have a decoded floor within 0.3 m below them, and most are within 0.05 m.**
  This exercises quaternions, scale, ground placement and dequantisation.
- **SacredForestMeadow child**: under Link's known position (-5.9, 115.9, -106.6) the highest floor is at
  y = 115.85 (GroundHighRes, Grass), within 5 cm.
- The coordinate space is the same as airwalls.json (exact instance positions, above). Units are metres, +Y up.
- No triangles are duplicated across tiles: compound instances live in exactly one tile, even when they stick out
  of it. There are about 400 exact duplicates inside a tile, from props placed twice.

### Triangle counts (default filter: player collision, Rough excluded)

Kokiri Forest child, per tile (each tile is 250 m; only X1/X2 × Z1/Z2 have the forest itself):

| tile | triangles | GroundHighRes | Smooth | GroundObject | Ground (terrain) |
|---|---|---|---|---|---|
| X1_Z1 | 572,938 | 276,029 | 39,380 | 1,064 | 256,465 |
| X1_Z2 | 320,267 | 125,840 | 7,431 | 104 | 186,892 |
| X2_Z1 | 436,203 | 197,298 | 43,815 | 104 | 194,986 |
| X2_Z2 | 152,272 | 53,391 | 5,437 | 84 | 93,360 |
| the other 12 | 4.9k-11k each, ground only | | | | |
| **total** | **1,559,553** (832,769 unique vertices) | | | | |

The whole tile X1_Z1 with `--all` (Rough, sensors, enemy walls included) is 665,499 triangles.
SacredForestMeadow child: 465,430 triangles over 4 tiles. InsideTheDekuTree: 532,792. Whole game: see the
table at the end (typical dungeon 0.1-0.9 M, field maps 0.5-5.3 M).

**Density** (Kokiri, default filter, triangle centroids within an XZ radius): 5 m ≈ 4-9 k, **10 m ≈ 14-22 k**,
20 m ≈ 48-69 k, 40 m ≈ 166-210 k. The median triangle area is 0.08 m² (edges about 0.4 m), so this is
render-mesh-like density. Decoding the whole Kokiri map in Python takes about 37 s; the JSON is about 60 MB.

### Not decoded / open

- bphsc section 0's first 0xC000 bytes (a lookup table), sections 2 and 3, the instance-info bytes 3-4 and the
  u16 at +8 (it is not the actor index), leafIdAndFlags, and the link from instances to placed actors (only the air
  walls were matched).
- The water-volume-like block after the compounds: records begin (0, 0, 1.0, scale xyz, rotation xyz (radians),
  translation xyz, u32 28 = Water), but the record size varies (0x48/0x50/...). It was left raw. Water surfaces
  also exist as Water-material triangles in the ground mesh.
- Smooth vs Rough: which one the player uses (see above).
- The 2-bit interior-primitive field, the section BVHs and the simd trees aren't needed for drawing and weren't
  decoded.
- The convex radius isn't applied (hulls are drawn about 5 cm inside the surface).
- NavMesh (.bphnm) and TerrainArc weren't needed. The ground meshes cover terrain.

### Whole game (all 80 static-compound maps decoded, no errors, no unknown shape types)

Default filter (player collision, Rough dropped). Total 55.9 M triangles. Python decode time: HyruleField 256 tiles about 130 s, most maps under 20 s.

| map | tiles | triangles | biggest tile |
|---|---|---|---|
| HyruleField_Child | 256 | 5,318,215 | 721,296 |
| HyruleField_Adult | 256 | 5,313,997 | 723,135 |
| KakarikoVillage_Adult | 64 | 3,945,989 | 701,236 |
| KakarikoVillage_Adult_Alternative | 64 | 3,943,653 | 698,968 |
| KakarikoVillage_Child | 64 | 3,929,009 | 684,324 |
| DeathMountainTrail_Adult | 16 | 2,187,084 | 584,643 |
| DeathMountainTrail_Child | 16 | 2,185,593 | 584,700 |
| KokiriForest_Adult | 16 | 1,561,398 | 573,123 |
| KokiriForest_Child | 16 | 1,559,553 | 572,938 |
| ZoraRiver_Adult | 64 | 1,246,747 | 430,451 |
| ZoraRiver_Child | 64 | 1,243,198 | 427,668 |
| GerudoFortress_Adult | 64 | 1,178,426 | 308,481 |
| Graveyard_Adult | 64 | 1,132,205 | 247,011 |
| GerudoValley_Adult | 64 | 1,061,259 | 265,670 |
| GerudoValley_Child | 64 | 1,050,270 | 265,670 |
| Graveyard_Child | 64 | 1,034,714 | 176,672 |
| DesertColossus_Adult | 64 | 916,313 | 250,528 |
| DesertColossus_Child | 64 | 916,313 | 250,528 |
| IceCavern | 1 | 901,238 | 901,238 |
| HyruleCastle | 64 | 789,738 | 249,293 |
| ZoraFountain_Adult | 64 | 772,392 | 223,731 |
| ZoraFountain_Child | 64 | 770,220 | 222,193 |
| LostWood_Child | 1 | 676,369 | 676,369 |
| LostWood_Adult | 1 | 676,041 | 676,041 |
| ZoraDomain_Adult | 1 | 590,152 | 590,152 |
| HountedWasteland_Adult | 64 | 574,922 | 53,949 |
| HountedWasteland_Child | 64 | 574,922 | 53,949 |
| WolfosWhiteStage | 1 | 572,008 | 572,008 |
| SpiritTemple | 4 | 552,399 | 160,416 |
| InsideTheDekuTree | 1 | 532,792 | 532,792 |
| DodongoCavern | 1 | 502,215 | 502,215 |
| SacredForestMeadow_Child | 4 | 465,430 | 158,711 |
| SacredForestMeadow_Adult | 4 | 464,350 | 158,135 |
| DeathMountainCrater_Child | 1 | 463,434 | 463,434 |
| DeathMountainCrater_Adult | 1 | 462,304 | 462,304 |
| ZoraDomain_Adult_Alternative | 1 | 432,908 | 432,908 |
| ZoraDomain_Child | 1 | 431,054 | 431,054 |
| FireTemple | 1 | 396,188 | 396,188 |
| WolfosStage | 4 | 355,372 | 120,128 |
| GoronCity_Child | 1 | 288,107 | 288,107 |
| GoronCity_Adult | 1 | 285,565 | 285,565 |
| CrumblingGanonCastle | 1 | 275,616 | 275,616 |
| WaterTemple | 9 | 272,721 | 272,529 |
| ShadowTemple | 1 | 268,990 | 268,990 |
| Market_Child | 1 | 253,546 | 253,546 |
| InsideJabuJabuBelly | 1 | 239,231 | 239,231 |
| Market_Adult | 1 | 228,755 | 228,755 |
| GerudoTrainingGround | 16 | 221,541 | 76,799 |
| ForestTemple | 1 | 172,098 | 172,098 |
| InsideGanonCastle | 1 | 159,680 | 159,680 |
| FinalBattleField | 1 | 150,586 | 150,586 |
| GanonStage | 1 | 150,242 | 150,242 |
| DodongoStage | 1 | 144,596 | 144,596 |
| LizalfosStage | 1 | 139,434 | 139,434 |
| GanonCastle | 1 | 138,651 | 138,651 |
| BottomOfTheWell | 1 | 126,458 | 126,458 |
| VolvagiaStage | 1 | 120,329 | 120,329 |
| StalfosStage | 1 | 107,687 | 107,687 |
| FlareDancerStage | 1 | 95,266 | 95,266 |
| GohmaStage | 1 | 55,719 | 55,719 |
| MegStage | 1 | 55,378 | 55,378 |
| BarinadeStage | 1 | 43,779 | 43,779 |
| MorphaStage | 9 | 24,965 | 24,965 |
| GerudoThiefStage | 1 | 19,738 | 19,738 |
| EnemyMuseum | 1 | 19,686 | 19,686 |
| PhantomGanonStage | 1 | 16,681 | 16,681 |
| TwinrovaStage | 1 | 16,645 | 16,645 |
| DarkLinkStage | 9 | 16,488 | 16,488 |
| GanondorfStage | 1 | 16,090 | 16,090 |
| BigOctoStage | 1 | 15,293 | 15,293 |
| IronKnuckleWhiteStage | 1 | 10,020 | 10,020 |
| NaboruStage | 1 | 8,200 | 8,200 |
| BongoBongoStage | 1 | 7,648 | 7,648 |
| BossRushIntervalRoom | 1 | 7,598 | 7,598 |
| IronKnuckleBlackStage | 1 | 4,115 | 4,115 |
| DeadHandStage | 1 | 4,040 | 4,040 |
| StalfosWeakStage | 1 | 2,501 | 2,501 |
| DinolfosStage | 1 | 2,392 | 2,392 |
| SageRoom | 1 | 308 | 308 |
| Demo_ShootingLocation_Tapestry_ED | 1 | 224 | 224 |

Undecoded raw sections found across the game, reported but not used:
- Desert maps (DesertColossus, HountedWasteland, GerudoTrainingGround, SpiritTemple) have, in some tiles, an 8 MB
  section (1024x1024 x 8 B, e.g. `fb0f ff7f ff7f 0009` per cell) and a 2 MB section (1024x1024 x 2 B, zero in the
  samples). SpiritTemple also has a 32 MB one. Section 7 is then non-zero and starts with floats 250.0, 800.0.
  This is almost certainly the sinking-sand heightfield: Sand material has IsWater = true and buoyancy in
  PhiveConfig. The walkable ground under it is still in the ground mesh and the compounds. Not decoded.
- Blocks of 7.5-7.9 KB in DeathMountainCrater/Trail, GanonCastle, HyruleCastle and ZoraFountain, plus many small
  blocks (0x50-0x5F0). These are probably the water/lava volume lists. Not decoded.
