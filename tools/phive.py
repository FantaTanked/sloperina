"""Read Phive collision (Havok hknp) files: .bphsh shapes and .bphsc static compound bodies.

    python phive.py <romfs path | local file>        summary of a .bphsh / .bphsc (local files: already decompressed)
    python phive.py types <romfs path | local file>   dump the Havok type layouts of every tagfile in the file
    python phive.py map <Area> <Map> [out.json]       world-space triangles of every tile of a map, as JSON
                                                      e.g. map KokiriForest KokiriForest_Child
    python phive.py obj <Area> <Map> out.obj          the same, as a Wavefront .obj (for a quick look in Blender)
      options: --all        keep everything (sensors, enemy-only walls, Rough layer, unplaced bodies), tagged
               --dir <dir>  read the tiles from <dir> (decompressed, romfs names without .zs) instead of the dump

Romfs paths need OOTMENU_NUP and zstandard (see gamefiles.py); a path to a local, decompressed file doesn't.

Format (this game: Havok SDK "20250200", Phive file version 0x0202 for bphsc, 0x0100 for bphsh):
  Phive header: "Phive\\0", u16 version, BOM 0xFFFE, u8 file type (0 shape, 1 navmesh, 2 static compound,
  3 cloth), u8 N, u32 offset[N], u32 size[N] (padded to 0x10). Sections are found by content: a Havok
  tagfile ("TAG0" at +4) or a nested Phive file ("Phive" at +0).
  Havok tagfile: big-endian chunk headers (u32 size|flags<<30, 4cc). TAG0{SDKV, DATA, TYPE{TPTR, TST1, TNA1,
  FST1, TBDY, THSH, TPAD}, INDX{ITEM}}. No PTCH: every pointer/array is relative (hkRelArrayView = s32 offset
  from the field + s32 count; hkRelArray/hkRelPtr = s64 offset from the field). ITEM entries (u32 type|flags<<24,
  u32 data offset, u32 count) name the type of each object, which is how shapes are dispatched here.
  .bphsh sections: [0] tagfile (one hknpShape, usually hknpMeshShape), [1] material table (16 B each: u32 material
  (PhiveConfig MaterialCollection), u32 sub-material, u64 user shape-tag mask), [2] layer hit masks (u64, u64).
  .bphsc sections: [0] 0xCAB0-byte header (u32 counts at +0xC000; tile name at +0xCA20), [1] actors (32 B: u32,
  u32, u64 placed-actor Hash (as in the .bcett), u32 SRT hash), [2] u64 pairs, [3] u16 pairs, [4] instance info
  (20 B: u32 index, u8 isSensor, u8 layer, u8 subLayer?, u8 ?, u16 actor index, u16 instance index in its
  compound, u32 first material, u32 material count), [5] materials (as .bphsh), [6] layer hit masks; then one
  tagfile per layer holding an hknpCompoundShape (instances: quaternion, translation, scale, shape, shapeTag =
  index into [4]), then small 0x50/0x30 blocks and a nested .bphsh (the tile's merged ground mesh).
  hknpMeshShape: geometry sections; each has u16x3 vertices, decoded as
  (sectionOffset[i] + v[i]) * vertexConversionUtil.bitScale16Inv[i], and primitives of four u8 vertex indices
  (a, b, c, d): triangle (a, b, c), plus (a, c, d) when d != c (a quad).
  Convex shapes (box, cylinder, polytope) all carry an hknpConvexHull (vertices + faces); faces are fanned.
  The real surface is the hull grown by convexRadius (+0x68, usually 0.05): the hull drawn here is that much
  inside it. Capsule = 2 hull vertices + radius, sphere = 1 vertex + radius: approximated with 8-sided tubes.
Coordinates are the game's world units (metres); +Y is up.
"""
import json
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

PHIVE_TYPES = {0: "shape", 1: "navmesh", 2: "static compound", 3: "cloth"}

# PhiveConfig (romfs/Pack/Bootup.Nin_Ounce_NVN2.pack.zs : Phive/Config/PhiveConfig.byml) names, this version.
MATERIALS = [
    "Undefined", "Soil", "Grass", "Sand", "HeavySand", "Snow", "HeavySnow", "Stone", "StoneSlip", "StoneNoSlip",
    "Metal", "MetalSlip", "MetalNoSlip", "WireNet", "Wood", "Ice", "Cloth", "Glass", "Bone", "Rope", "Character",
    "Ragdoll", "Meat", "Vegetable", "Bomb", "MagicBall", "Barrier", "AirWall", "Water", "HotWater", "IceWater",
    "Lava", "Bog", "ContaminatedWater", "Gas", "InvalidateRestartPos", "HorseSpeedLimit",
    "ForbidDynamicCuttingAreaForHugeCharacter", "ForbidHorseReturnToSafePosByClimate", "WaterSlip",
    "HorseDeleteImmediatelyOnDeath", "DropItem", "Skulltula", "PoisonBog", "WoodSlip", "IceNoSlip"]
LAYERS_ENTITY = [
    "Object", "SmallObject", "GroundObject", "Player", "Character", "Water", "GroundHighRes",
    "GroundHighResSmooth", "GroundHighResRough", "GroundStaticTree", "HitOnlyGround", "CharacterSight",
    "CameraSight", "CameraBody", "NoHit", "CustomReceiver", "Fluid", "Smoke", "Gas", "LowTree", "IK",
    "CameraAlphaCtrlCheck"]
LAYERS_SENSOR = [
    "Object", "SmallObject", "Player", "Enemy", "NPC", "Animal", "TgtHitAll", "PlayerAttack", "EnemyAttack",
    "NPCAttack", "AttackAll", "ChemicalObject", "ChemicalElement", "InDoor", "StaticGeneralArea", "QueryOnly",
    "Tree", "Camera", "NoHit", "CustomReceiver", "PerceptionSensor", "PerceptionEmitter", "Fluid",
    "ForbidAutoPlacementArea", "SpecialChemical", "GeneralArea", "EnemyPerceptionSightNoThrough"]
PLAYER_LAYER_BIT = 1 << LAYERS_ENTITY.index("Player")

PHIVE_SHAPE = 0
PHIVE_STATIC_COMPOUND = 2

# .bphsc layout (see the module docstring): fixed sections, then one tagfile per layer and the rest
BPHSC_COUNTS_AT = 0xC000  # in section 0: u32 counts[8]
BPHSC_TILE_NAME_AT = 0xCA20  # in section 0
SEC_ACTORS, SEC_INSTANCE_INFO, SEC_MATERIALS, SEC_LAYER_MASKS = 1, 4, 5, 6
LAST_FIXED_SECTION = 6
GROUND_PLACEMENT_SIZE = 0x30  # the block that places the nested ground mesh

ITEM_IS_OBJECT = 0x10  # tagfile ITEM flag
NO_TAG = 0xFFFF  # mesh shape tag table: a run without a shape tag
UNQUANTISED = (0x7FFFFFFF,) * 3  # mesh section offset meaning its vertices are stored as floats
FLOOR_NORMAL_Y = 0.5  # classify(): about 60 degrees from vertical


def name_of(table, i):
    return table[i] if 0 <= i < len(table) else str(i)


# ---------------------------------------------------------------------------------------------------- Phive


def phive_sections(data, base=0):
    """(file type, [(offset, size)]) of a Phive file starting at data[base]."""
    if data[base:base + 6] != b"Phive\0":
        raise ValueError("not a Phive file (magic %r)" % data[base:base + 6])
    file_type, count = data[base + 10], data[base + 11]
    offsets = struct.unpack_from("<%dI" % count, data, base + 12)
    sizes = struct.unpack_from("<%dI" % count, data, base + 12 + 4 * count)
    return file_type, [(base + offset, size) for offset, size in zip(offsets, sizes)]


def is_tagfile(data, off):
    return data[off + 4:off + 8] == b"TAG0"


def is_phive(data, off):
    return data[off:off + 6] == b"Phive\0"


# ---------------------------------------------------------------------------------------------------- Havok tagfile


def varint(d, p):
    """A tagfile variable-length integer at d[p] -> (value, position after it). The first byte's top bits give the
    length; the rest of it and the following bytes are the value, big-endian."""
    first = d[p]
    if first < 0x80:
        return first, p + 1
    if first < 0xC0:
        return int.from_bytes(d[p:p + 2], "big") & 0x3FFF, p + 2
    if first < 0xE0:
        return int.from_bytes(d[p:p + 3], "big") & 0x1FFFFF, p + 3
    if first < 0xE8:
        return int.from_bytes(d[p:p + 4], "big") & 0x7FFFFFF, p + 4
    if first < 0xF0:
        return int.from_bytes(d[p:p + 5], "big") & 0x7FFFFFFFF, p + 5
    if first < 0xF8:
        return int.from_bytes(d[p:p + 8], "big") & 0x7FFFFFFFFFFFFFF, p + 8
    if first == 0xF8:
        return int.from_bytes(d[p + 1:p + 6], "big"), p + 6
    if first == 0xF9:
        return int.from_bytes(d[p + 1:p + 9], "big"), p + 9
    raise ValueError("bad varint byte %#x at %#x" % (first, p))


def _chunks(d, p, end):
    """{4cc: nested dict, or (start, end) of a leaf chunk's contents} for the chunks in d[p:end]."""
    out = {}
    while p + 8 <= end:
        header = struct.unpack_from(">I", d, p)[0]
        size = header & 0x3FFFFFFF
        if size < 8:
            break
        tag = d[p + 4:p + 8].decode("latin1")
        is_container = header >> 30 == 0
        out[tag] = _chunks(d, p + 8, p + size) if is_container else (p + 8, p + size)
        p += size
    return out


class Tagfile:
    """A Havok tagfile at data[off]: type names, the ITEM list, and object lookup by data offset."""

    def __init__(self, data, off):
        self.d = data
        size = struct.unpack_from(">I", data, off)[0] & 0x3FFFFFFF
        self.chunks = _chunks(data, off, off + size)["TAG0"]
        self.sdk = data[slice(*self.chunks["SDKV"])].decode("latin1")
        self.base = self.chunks["DATA"][0]
        type_chunks = self.chunks["TYPE"]
        self.tstr = [name.decode("latin1") for name in data[slice(*type_chunks["TST1"])].split(b"\0")]
        self.fstr = [name.decode("latin1") for name in data[slice(*type_chunks["FST1"])].split(b"\0")]
        self.types = self._read_type_names(type_chunks["TNA1"][0])
        # ITEM: (type index, flags, absolute data offset, count)
        p, end = self.chunks["INDX"]["ITEM"]
        self.items = []
        while p < end:
            type_and_flags, offset, count = struct.unpack_from("<III", data, p)
            p += 12
            self.items.append((type_and_flags & 0xFFFFFF, type_and_flags >> 24, self.base + offset, count))
        # objects by absolute offset -> type name
        self.objects = {offset: self.tname(type_index)
                        for type_index, flags, offset, _ in self.items if flags & ITEM_IS_OBJECT}

    def _read_type_names(self, p):
        """TNA1: per type, its name and template parameters. Index 0 is 'no type'."""
        data = self.d
        count, p = varint(data, p)
        types = [None]
        for _ in range(1, count):
            name_index, p = varint(data, p)
            n_params, p = varint(data, p)
            template = []
            for _ in range(n_params):
                param_name, p = varint(data, p)
                param_value, p = varint(data, p)
                template.append((self.tstr[param_name], param_value))
            types.append({"name": self.tstr[name_index], "tpl": template})
        return types

    def tname(self, i):
        """A type's full name, with its template arguments."""
        if not i:
            return "-"
        if i >= len(self.types):
            return "?%d" % i
        type_info = self.types[i]
        name = type_info["name"]
        if type_info["tpl"]:
            # parameters named t... are types, the others values
            args = [self.tname(value) if key.startswith("t") else str(value) for key, value in type_info["tpl"]]
            name += "<" + ",".join(args) + ">"
        return name

    def root(self):
        """(offset, type name) of the first object (the top-level shape)."""
        type_index, _, offset, _ = self.items[1]
        return offset, self.tname(type_index)

    def type_bodies(self):
        """Parse TBDY (layouts). Only for the 'types' dump; best effort (this version's attribute flag is unknown)."""
        d = self.d
        p, end = self.chunks["TYPE"]["TBDY"]
        while p < end:
            type_index, p = varint(d, p)
            if type_index == 0:
                continue
            type_info = self.types[type_index]
            type_info["parent"], p = varint(d, p)
            flags, p = varint(d, p)
            type_info["flags"] = flags
            if flags & 1:
                type_info["format"], p = varint(d, p)
            if flags & 2:
                type_info["subtype"], p = varint(d, p)
            if flags & 4:
                type_info["version"], p = varint(d, p)
            if flags & 8:
                type_info["size"], p = varint(d, p)
                type_info["align"], p = varint(d, p)
            if flags & 0x10:
                type_info["tflags"], p = varint(d, p)
            if flags & 0x20:
                type_info["members"], p = self._read_members(p)
            if flags & 0x40:
                count, p = varint(d, p)
                type_info["interfaces"] = []
                for _ in range(count):
                    interface, p = varint(d, p)
                    value, p = varint(d, p)
                    type_info["interfaces"].append((interface, value))
            if flags & 0x80:
                raise ValueError("type %d: attribute flag not understood; stopped" % type_index)

    def _read_members(self, p):
        """A TBDY member list at p -> ([(name, flags, offset, type index)], position after it)."""
        d = self.d
        count, p = varint(d, p)
        members = []
        for _ in range(count):
            name_index, p = varint(d, p)
            flags, p = varint(d, p)
            if flags & 0x80:  # seen on hknpShapeMassProperties.compressedMassProperties: one more varint
                _, p = varint(d, p)
            offset, p = varint(d, p)
            type_index, p = varint(d, p)
            name = self.fstr[name_index] if name_index < len(self.fstr) else "?%d" % name_index
            members.append((name, flags, offset, type_index))
        return members, p


def rel_view(d, p):
    """hkRelArrayView<T,int> at p -> (absolute offset, count)."""
    offset, count = struct.unpack_from("<ii", d, p)
    return p + offset, count


def rel_array(d, p):
    """hkRelArray<T> at p -> (absolute offset, count)."""
    offset, count = struct.unpack_from("<qi", d, p)
    return p + offset, count


def rel_ptr(d, p):
    """hkRelPtr at p -> absolute offset, or None for a null pointer."""
    offset = struct.unpack_from("<q", d, p)[0]
    return p + offset if offset else None


# ---------------------------------------------------------------------------------------------------- shapes
# A decoded shape is a list of (v0, v1, v2, tag): local-space triangles; tag is the mesh's per-primitive
# shape tag (0 when the shape has none).

# hknpMeshShape fields
MESH_BIT_SCALE_INV = 0x50  # vertexConversionUtil.bitScale16Inv: float[3]
MESH_SHAPE_TAG_TABLE = 0x60  # hkRelArrayView of (u32 first primitive key, u16 tag) runs, 8 bytes each
MESH_SECTIONS = 0x88  # hkRelArrayView of geometry sections
MESH_SECTION_SIZE = 0x40
SECTION_PRIMITIVES = 0x08  # hkRelArrayView of u8[4] primitives
SECTION_VERTICES = 0x10  # hkRelArrayView of the vertex buffer (bytes)
SECTION_OFFSET = 0x20  # s32[3], added to the quantised vertices

# hknpConvexShape fields
CONVEX_VERTICES = 0x38  # hkRelArrayView of float[3]
CONVEX_FACES = 0x48  # hkRelArrayView of (u16 first index, u8 count) faces, 4 bytes each
CONVEX_FACE_INDICES = 0x50  # hkRelArrayView of u8 vertex indices
CONVEX_RADIUS = 0x68  # float

# hknpCompoundShape: instance array, and the fields of one instance
COMPOUND_INSTANCES = 0x38  # hkRelArray
INSTANCE_SIZE = 64
INSTANCE_ROTATION = 0x00  # quaternion x y z w
INSTANCE_TRANSLATION = 0x10  # float[3]
INSTANCE_LEAF = 0x1C  # u32
INSTANCE_SCALE = 0x20  # float[3]
INSTANCE_SHAPE_TAG = 0x2C  # u16
INSTANCE_SHAPE = 0x30  # hkRelPtr to the shape; null for a free slot


def _section_vertices(d, section, inv_scale):
    """A mesh geometry section's vertices: quantised u16x3 (the usual case) or plain floats."""
    vertex_buffer, buffer_size = rel_view(d, section + SECTION_VERTICES)
    section_offset = struct.unpack_from("<3i", d, section + SECTION_OFFSET)
    if section_offset == UNQUANTISED:  # section stored unquantised: hkFloat3 vertices
        count = buffer_size // 12
        raw = struct.unpack_from("<%df" % (3 * count), d, vertex_buffer)
        return [(raw[3 * j], raw[3 * j + 1], raw[3 * j + 2]) for j in range(count)]
    count = buffer_size // 6
    raw = struct.unpack_from("<%dH" % (3 * count), d, vertex_buffer)
    vertices = []
    for j in range(count):
        vertices.append(((section_offset[0] + raw[3 * j]) * inv_scale[0],
                         (section_offset[1] + raw[3 * j + 1]) * inv_scale[1],
                         (section_offset[2] + raw[3 * j + 2]) * inv_scale[2]))
    return vertices


def mesh_shape(d, o):
    """hknpMeshShape at o -> triangles."""
    inv_scale = struct.unpack_from("<3f", d, o + MESH_BIT_SCALE_INV)
    tag_table_at, tag_count = rel_view(d, o + MESH_SHAPE_TAG_TABLE)
    tag_table = [struct.unpack_from("<IH", d, tag_table_at + 8 * i) for i in range(tag_count)]
    sections_at, section_count = rel_view(d, o + MESH_SECTIONS)
    tris = []
    for section_index in range(section_count):
        section = sections_at + MESH_SECTION_SIZE * section_index
        primitives, primitive_count = rel_view(d, section + SECTION_PRIMITIVES)
        verts = _section_vertices(d, section, inv_scale)
        for primitive_index in range(primitive_count):
            a, b, c, e = d[primitives + 4 * primitive_index:primitives + 4 * primitive_index + 4]
            key = (section_index << 9) | (primitive_index << 1)
            tag = _tag_for_key(tag_table, key)
            tris.append((verts[a], verts[b], verts[c], tag))
            if e != c:  # a quad: its second triangle
                tris.append((verts[a], verts[c], verts[e], tag))
    return tris


def _tag_for_key(table, key):
    """Shape tag for a primitive key: the table holds (first key, tag) runs, ended by tag 0xFFFF."""
    tag = 0
    for first_key, run_tag in table:
        if first_key > key:
            break
        tag = run_tag
    return 0 if tag == NO_TAG else tag


def convex_shape(d, o):
    """hknpConvexShape (and subclasses: box, cylinder, ...) at o -> triangles of its hull."""
    vertices_at, vertex_count = rel_view(d, o + CONVEX_VERTICES)
    faces_at, face_count = rel_view(d, o + CONVEX_FACES)
    indices_at, index_count = rel_view(d, o + CONVEX_FACE_INDICES)
    verts = [struct.unpack_from("<3f", d, vertices_at + 12 * i) for i in range(vertex_count)]
    if face_count == 0 and vertex_count == 2:  # capsule: a segment + convexRadius
        return capsule_tris(verts[0], verts[1], struct.unpack_from("<f", d, o + CONVEX_RADIUS)[0])
    if face_count == 0 and vertex_count == 1:  # sphere: a point + convexRadius; drawn as a capsule along Y
        radius = struct.unpack_from("<f", d, o + CONVEX_RADIUS)[0]
        centre = verts[0]
        return capsule_tris((centre[0], centre[1] - 1e-3, centre[2]), (centre[0], centre[1] + 1e-3, centre[2]),
                            radius)
    indices = d[indices_at:indices_at + index_count]
    tris = []
    for face in range(face_count):
        first, count = struct.unpack_from("<HB", d, faces_at + 4 * face)
        ring = [verts[indices[first + k]] for k in range(count)]
        for k in range(1, count - 1):  # fan the face into triangles
            tris.append((ring[0], ring[k], ring[k + 1], 0))
    return tris


def _cross(u, v):
    return [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]


def capsule_tris(a, b, r, n=8):
    """A capsule as an n-sided tube from a to b with pointed (cone) caps reaching r past each end."""
    axis = [b[i] - a[i] for i in range(3)]
    length = math.sqrt(sum(x * x for x in axis)) or 1.0
    axis = [x / length for x in axis]
    # two unit vectors at right angles to the axis and each other
    reference = (1.0, 0.0, 0.0) if abs(axis[0]) < 0.9 else (0.0, 1.0, 0.0)
    u = _cross(axis, reference)
    u_length = math.sqrt(sum(x * x for x in u))
    u = [x / u_length for x in u]
    w = _cross(axis, u)

    def ring(centre, k):
        """The k-th of n points around centre."""
        angle = 2 * math.pi * k / n
        return tuple(centre[i] + r * (math.cos(angle) * u[i] + math.sin(angle) * w[i]) for i in range(3))

    tip_a = tuple(a[i] - r * axis[i] for i in range(3))
    tip_b = tuple(b[i] + r * axis[i] for i in range(3))
    tris = []
    for k in range(n):
        a0, a1, b0, b1 = ring(a, k), ring(a, k + 1), ring(b, k), ring(b, k + 1)
        tris += [(a0, b0, b1, 0), (a0, b1, a1, 0), (tip_a, a0, a1, 0), (tip_b, b1, b0, 0)]
    return tris


def quat_rotate(q, v):
    x, y, z, w = q
    vx, vy, vz = v
    # t = 2 * cross(q.xyz, v); v' = v + w*t + cross(q.xyz, t)
    tx, ty, tz = 2 * (y * vz - z * vy), 2 * (z * vx - x * vz), 2 * (x * vy - y * vx)
    return (vx + w * tx + (y * tz - z * ty), vy + w * ty + (z * tx - x * tz), vz + w * tz + (x * ty - y * tx))


def compound_instances(d, o):
    """hknpCompoundShape at o -> [dict(q, t, s, shape offset, shapeTag, leaf)] (free slots skipped)."""
    instances_at, count = rel_array(d, o + COMPOUND_INSTANCES)
    out = []
    for i in range(count):
        p = instances_at + INSTANCE_SIZE * i
        rotation = struct.unpack_from("<4f", d, p + INSTANCE_ROTATION)
        translation = struct.unpack_from("<3f", d, p + INSTANCE_TRANSLATION)
        leaf = struct.unpack_from("<I", d, p + INSTANCE_LEAF)[0]
        scale = struct.unpack_from("<3f", d, p + INSTANCE_SCALE)
        tag = struct.unpack_from("<H", d, p + INSTANCE_SHAPE_TAG)[0]
        shape = rel_ptr(d, p + INSTANCE_SHAPE)
        if shape is None:
            continue
        out.append({"index": i, "q": rotation, "t": translation, "s": scale, "shape": shape, "shapeTag": tag,
                    "leaf": leaf})
    return out


CONVEX_KINDS = ("hknpConvexShape", "hknpBoxShape", "hknpCylinderShape", "hknpConvexPolytopeShape",
                "hknpCapsuleShape", "hknpSphereShape")


class ShapeDecoder:
    """Decodes shapes inside one tagfile, caching the local triangles of shared shapes."""

    def __init__(self, tf):
        self.tf = tf
        self.cache = {}
        self.unknown = {}  # type name -> how many shapes of it weren't decoded

    def local(self, o):
        """The shape at offset o -> its local-space triangles."""
        if o in self.cache:
            return self.cache[o]
        kind = self.tf.objects.get(o, "?")
        d = self.tf.d
        if kind == "hknpMeshShape":
            tris = mesh_shape(d, o)
        elif kind in CONVEX_KINDS:
            tris = convex_shape(d, o)
        elif kind == "hknpCompoundShape":
            tris = []
            for inst in compound_instances(d, o):
                tris += transform(self.local(inst["shape"]), inst)
        else:
            self.unknown[kind] = self.unknown.get(kind, 0) + 1
            tris = []
        self.cache[o] = tris
        return tris


def transform(tris, inst):
    """Triangles placed by an instance's scale, rotation and translation (each shared vertex worked out once)."""
    q, t, s = inst["q"], inst["t"], inst["s"]

    def place(v):
        r = quat_rotate(q, (v[0] * s[0], v[1] * s[1], v[2] * s[2]))
        return (r[0] + t[0], r[1] + t[1], r[2] + t[2])

    placed = {}

    def placed_vertex(v):
        if v not in placed:
            placed[v] = place(v)
        return placed[v]

    return [(placed_vertex(a), placed_vertex(b), placed_vertex(c), tag) for a, b, c, tag in tris]


# ---------------------------------------------------------------------------------------------------- files


def _records(data, offset, size, fmt):
    """The fixed-size records filling data[offset:offset + size], 16 bytes each."""
    return [struct.unpack_from(fmt, data, offset + 16 * i) for i in range(size // 16)]


def decode_bphsh(data, base=0):
    """A .bphsh -> dict(kind, tris, materials, layermasks, sdk)."""
    _, sections = phive_sections(data, base)
    tf_off = next(offset for offset, size in sections if size and is_tagfile(data, offset))
    tf = Tagfile(data, tf_off)
    root, kind = tf.root()
    decoder = ShapeDecoder(tf)
    tris = decoder.local(root)
    materials, masks = [], []
    # the sections after the tagfile: the material table, then the layer hit masks
    rest = [(offset, size) for offset, size in sections if size and offset != tf_off]
    if len(rest) >= 1:
        materials = _records(data, rest[0][0], rest[0][1], "<IIQ")
    if len(rest) >= 2:
        masks = _records(data, rest[1][0], rest[1][1], "<QQ")
    return {"kind": kind, "tris": tris, "materials": materials, "layermasks": masks, "sdk": tf.sdk,
            "unknown": decoder.unknown}


def _decode_layer(data, offset, section_index, info, out):
    """A .bphsc layer tagfile (an hknpCompoundShape): add its instances, in world space, to out["layers"]."""
    tf = Tagfile(data, offset)
    root, kind = tf.root()
    decoder = ShapeDecoder(tf)
    instances = compound_instances(data, root) if kind == "hknpCompoundShape" else []
    for inst in instances:
        inst["kind"] = tf.objects.get(inst["shape"], "?")
        inst["info"] = info[inst["shapeTag"]] if inst["shapeTag"] < len(info) else None
        inst["tris"] = transform(decoder.local(inst["shape"]), inst)
    for shape_kind, count in decoder.unknown.items():
        out["unknown"][shape_kind] = out["unknown"].get(shape_kind, 0) + count
    first = instances[0]["info"] if instances and instances[0]["info"] else None
    key = (first[1], first[2]) if first else None  # (isSensor, layer)
    # A second entity compound on a layer already seen holds bodies in local space (e.g. 1 instance at
    # the origin in InsideTheDekuTree): placed at run time by their actor, not in world space here.
    extra = bool(key and not key[0] and any(layer["key"] == key for layer in out["layers"]))
    out["layers"].append({"section": section_index, "kind": kind, "instances": instances, "key": key,
                          "extra": extra})


def decode_bphsc(data):
    """A .bphsc -> dict(name, counts, actors, info, materials, layermasks, layers=[...], ground=bphsh dict|None).

    Each entry of layers: dict(section, kind, instances=[instance dicts + 'info', 'tris' (world space)]).
    """
    file_type, sections = phive_sections(data)
    if file_type != PHIVE_STATIC_COMPOUND:
        raise ValueError("not a static compound (type %d)" % file_type)
    header, _ = sections[0]
    name_at = header + BPHSC_TILE_NAME_AT
    name = data[name_at:data.index(b"\0", name_at)].decode("latin1")
    counts = struct.unpack_from("<8I", data, header + BPHSC_COUNTS_AT)
    n_actors, n_info, n_materials = counts[4], counts[5], counts[6]

    def table(section_index, fmt, count):
        offset, size = sections[section_index]
        record_size = struct.calcsize(fmt)
        if not size:
            return []
        return [struct.unpack_from(fmt, data, offset + record_size * k) for k in range(count)]

    actors = table(SEC_ACTORS, "<IIQI", n_actors)
    info = table(SEC_INSTANCE_INFO, "<I4BHHII", n_info)
    materials = table(SEC_MATERIALS, "<IIQ", n_materials)
    masks = table(SEC_LAYER_MASKS, "<QQ", n_materials)
    out = {"name": name, "counts": counts, "actors": actors, "info": info, "materials": materials,
           "layermasks": masks, "layers": [], "ground": None, "unknown": {}, "other": []}
    for section_index, (offset, size) in enumerate(sections):
        if not size or section_index <= LAST_FIXED_SECTION:
            continue
        if is_tagfile(data, offset):
            _decode_layer(data, offset, section_index, info, out)
        elif is_phive(data, offset):
            out["ground"] = decode_bphsh(data, offset)
            out["ground"]["section"] = section_index
        elif size == GROUND_PLACEMENT_SIZE:
            # placement of the nested ground mesh: scale xyz, pad, then (from +0x18) translation x y z, u32 6
            floats = struct.unpack_from("<12f", data, offset)
            out["groundXform"] = {"s": floats[0:3], "t": floats[6:9], "q": (0.0, 0.0, 0.0, 1.0)}
        else:
            # Not decoded. Section 7 (0x50, zero so far) and a block after the tagfiles that looks like water
            # volumes: records starting (0, 0, 1.0?, scale xyz, rotation xyz (radians), translation xyz,
            # u32 material 28 = Water, ...) but the record size varies (0x48/0x50) - not worked out.
            out["other"].append((section_index, data[offset:offset + size]))
    if out["ground"] and "groundXform" in out:
        out["ground"]["tris"] = transform(out["ground"]["tris"], out["groundXform"])
    return out


def instance_props(sc, inst):
    """(layer name, is sensor, [material names], hits player) for a static compound instance."""
    info = inst.get("info")
    if not info:
        return "?", False, [], True
    _, sensor, layer, _, _, _, _, first_material, material_count = info
    layer_name = name_of(LAYERS_SENSOR if sensor else LAYERS_ENTITY, layer)
    material_range = range(first_material, first_material + material_count)
    materials = [name_of(MATERIALS, sc["materials"][m][0]) for m in material_range if m < len(sc["materials"])]
    hits = any(sc["layermasks"][m][0] & PLAYER_LAYER_BIT for m in material_range if m < len(sc["layermasks"]))
    return layer_name, bool(sensor), materials, hits and not sensor


def bbox(tris):
    """(lowest x y z, highest x y z) of the triangles' vertices; None if there are none."""
    if not tris:
        return None
    low = [min(min(tri[k][axis] for k in range(3)) for tri in tris) for axis in range(3)]
    high = [max(max(tri[k][axis] for k in range(3)) for tri in tris) for axis in range(3)]
    return low, high


def classify(tri):
    """'floor' / 'wall' / 'ceiling' by the face normal's Y (threshold: 0.5, about 60 degrees from vertical)."""
    a, b, c, _ = tri
    ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
    nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    length = math.sqrt(nx * nx + ny * ny + nz * nz)
    if length == 0:
        return "degenerate"
    ny /= length
    if ny > FLOOR_NORMAL_Y:
        return "floor"
    if ny < -FLOOR_NORMAL_Y:
        return "ceiling"
    return "wall"


# ---------------------------------------------------------------------------------------------------- CLI


def load(path):
    """A local file's bytes, or else a romfs file from the dump."""
    if os.path.exists(path):
        return open(path, "rb").read()
    import gamefiles
    return gamefiles.read(path)


def fmt_box(b):
    if not b:
        return "(empty)"
    low, high = b
    return "x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f" % (low[0], high[0], low[1], high[1], low[2], high[2])


def _count(counts, key, n=1):
    counts[key] = counts.get(key, 0) + n


def _summary_bphsh(data):
    shape = decode_bphsh(data)
    print("  shape %s (Havok %s): %d triangles, %s" % (
        shape["kind"], shape["sdk"], len(shape["tris"]), fmt_box(bbox(shape["tris"]))))
    print("  materials:", [(name_of(MATERIALS, m), sub, "%#x" % tags) for m, sub, tags in shape["materials"]])
    print("  layer masks:", ["%#x/%#x" % m for m in shape["layermasks"]])
    tags = {}
    for tri in shape["tris"]:
        _count(tags, tri[3])
    print("  triangles per shape tag:", tags)
    if shape["unknown"]:
        print("  NOT decoded:", shape["unknown"])


def _summary_bphsc(data):
    sc = decode_bphsc(data)
    print("  tile %s: counts %s; %d actors, %d instance infos, %d materials" % (
        sc["name"], sc["counts"], len(sc["actors"]), len(sc["info"]), len(sc["materials"])))
    total = 0
    for layer in sc["layers"]:
        tris = [tri for inst in layer["instances"] for tri in inst["tris"]]
        total += len(tris)
        kinds, layer_counts = {}, {}
        for inst in layer["instances"]:
            _count(kinds, inst["kind"])
            layer_name, sensor, _, _ = instance_props(sc, inst)
            _count(layer_counts, ("sensor:" if sensor else "") + layer_name)
        print("  section %d %s%s: %d instances %s, layers %s, %d triangles, %s" % (
            layer["section"], layer["kind"], " (EXTRA: local space, not placed)" if layer["extra"] else "",
            len(layer["instances"]), kinds, layer_counts, len(tris), fmt_box(bbox(tris))))
    ground = sc["ground"]
    if ground:
        total += len(ground["tris"])
        print("  section %d nested shape %s: %d triangles, %s, materials %s" % (
            ground["section"], ground["kind"], len(ground["tris"]), fmt_box(bbox(ground["tris"])),
            [name_of(MATERIALS, m[0]) for m in ground["materials"]]))
    if "groundXform" in sc:
        print("  ground mesh placed at", sc["groundXform"]["t"], "scale", sc["groundXform"]["s"])
    for section_index, raw in sc["other"]:
        print("  section %d: %d bytes not decoded: %s" % (section_index, len(raw), raw[:0x30].hex()))
    if sc["unknown"]:
        print("  NOT decoded:", sc["unknown"])
    print("  total triangles: %d" % total)


def summary(path):
    data = load(path)
    file_type, sections = phive_sections(data)
    print("%s: Phive %s, version %04x, %d bytes, sections %s" % (
        path, PHIVE_TYPES.get(file_type, file_type), struct.unpack_from(">H", data, 6)[0], len(data),
        [(i, hex(offset), hex(size)) for i, (offset, size) in enumerate(sections) if size]))
    if file_type == PHIVE_SHAPE:
        _summary_bphsh(data)
    elif file_type == PHIVE_STATIC_COMPOUND:
        _summary_bphsc(data)
    else:
        print("  (no decoder for this Phive type)")


def _print_type_layouts(data, section_index, offset):
    tf = Tagfile(data, offset)
    print("== section %d tagfile at %#x, Havok %s" % (section_index, offset, tf.sdk))
    try:
        tf.type_bodies()
    except Exception as e:  # noqa: BLE001
        print("   (layouts incomplete: %s)" % e)
    for i, type_info in enumerate(tf.types):
        if not type_info or "members" not in type_info:
            continue
        print("  %s : %s  size %s" % (tf.tname(i), tf.tname(type_info.get("parent")), type_info.get("size")))
        for name, _, member_offset, type_index in type_info["members"]:
            type_name = tf.tname(type_index) if type_index < len(tf.types) else type_index
            print("      +%#04x %s %s" % (member_offset, name, type_name))
    counts = {}
    for type_index, _, _, _ in tf.items[1:]:
        _count(counts, tf.tname(type_index))
    print("  items:", counts)


def dump_types(path):
    data = load(path)
    _, sections = phive_sections(data)
    for section_index, (offset, size) in enumerate(sections):
        if not size:
            continue
        if is_phive(data, offset):  # a nested Phive file: its tagfiles
            _, nested = phive_sections(data, offset)
            tagfiles = [at for at, nested_size in nested if nested_size and is_tagfile(data, at)]
        else:
            tagfiles = [offset] if is_tagfile(data, offset) else []
        for tagfile_offset in tagfiles:
            _print_type_layouts(data, section_index, tagfile_offset)


def map_tiles(area, mapname, local_dir=None):
    """A map's static compound tiles: romfs paths from the dump's index, or files in local_dir (decompressed,
    named like the romfs files without .zs)."""
    stem = mapname + "_X"

    def is_tile(name):
        return (name.startswith(stem) or name.startswith(mapname + ".")) and ".bphsc" in name

    if local_dir:
        return sorted(os.path.join(local_dir, name) for name in os.listdir(local_dir)
                      if is_tile(name) and not name.endswith(".zs"))
    import gamefiles
    prefix = "romfs/Phive/StaticCompoundBody/%s/" % area
    return sorted(path for path in gamefiles.nup().index
                  if path.startswith(prefix) and is_tile(path[len(prefix):]) and path.endswith(".zs"))


SKIP_LAYERS_DEFAULT = ("GroundHighResRough",)


def _tile_triangles(sc, everything):
    """One decoded tile's world-space triangles (see map_triangles) -> (tris, {reason dropped: count})."""
    tris = []
    dropped = {}
    for layer in sc["layers"]:
        if layer["extra"] and not everything:
            _count(dropped, "extra bodies", sum(len(inst["tris"]) for inst in layer["instances"]))
            continue
        for inst in layer["instances"]:
            layer_name, sensor, _, hits = instance_props(sc, inst)
            if not everything and (sensor or layer_name in SKIP_LAYERS_DEFAULT):
                _count(dropped, "sensor" if sensor else layer_name, len(inst["tris"]))
                continue
            if inst.get("info"):
                first_material, material_count = inst["info"][7], inst["info"][8]
            else:
                first_material, material_count = 0, 0
            for a, b, c, tag in inst["tris"]:
                # the shape tag picks one of the instance's materials
                m = first_material + tag if tag < material_count else first_material
                material = name_of(MATERIALS, sc["materials"][m][0]) if m < len(sc["materials"]) else "?"
                if m < len(sc["layermasks"]):
                    hit = bool(sc["layermasks"][m][0] & PLAYER_LAYER_BIT)
                else:
                    hit = hits
                if not everything and not hit:
                    _count(dropped, "not hit by player (%s)" % material)
                    continue
                tris.append((a, b, c, {"layer": ("sensor:" if sensor else "") + layer_name, "material": material,
                                       "hitsPlayer": hit and not sensor, "src": "compound"}))
    ground = sc["ground"]
    if ground:
        for a, b, c, tag in ground["tris"]:
            material = ground["materials"][tag] if tag < len(ground["materials"]) else (0, 0, 0)
            mask = ground["layermasks"][tag][0] if tag < len(ground["layermasks"]) else ~0
            tris.append((a, b, c, {"layer": "Ground", "material": name_of(MATERIALS, material[0]),
                                   "hitsPlayer": bool(mask & PLAYER_LAYER_BIT), "src": "ground"}))
    return tris, dropped


def map_triangles(paths, everything=False, log=print):
    """World-space triangles of the given tiles: (tiles, tris).

    tris: (v0, v1, v2, attrs), attrs = dict(layer, material, hitsPlayer, src). By default only what can stop
    the player: no sensors, no 'extra' (unplaced) bodies, no materials whose hit mask leaves out the Player layer
    (e.g. enemy-only air walls), and not GroundHighResRough (the detailed twin of GroundHighResSmooth - see the
    report). everything=True keeps all of it (attributes say what each triangle is).
    """
    tris, tiles = [], []
    for path in paths:
        sc = decode_bphsc(load(path))
        tile_tris, dropped = _tile_triangles(sc, everything)
        tris += tile_tris
        by_layer = {}
        for tri in tile_tris:
            _count(by_layer, tri[3]["layer"])
        tiles.append({"path": path, "name": sc["name"], "triangles": len(tile_tris), "byLayer": by_layer,
                      "dropped": dropped, "bbox": bbox(tile_tris), "notDecoded": sc["unknown"],
                      "otherSections": [[section_index, len(raw)] for section_index, raw in sc["other"]]})
        log("%s: %d triangles %s %s" % (sc["name"], len(tile_tris), fmt_box(bbox(tile_tris)), by_layer))
    return tiles, tris


def map_json(area, mapname, tiles, tris):
    """Compact JSON document: shared vertex list (mm-rounded) + triangles as index triples with attributes."""
    vertex_index, verts = {}, []
    names = {"class": [], "material": [], "layer": []}

    def code(kind, value):
        """value's index in the names list of its kind (added if new)."""
        values = names[kind]
        if value not in values:
            values.append(value)
        return values.index(value)

    def vertex(v):
        """v's index in the shared vertex list (added if new)."""
        key = (round(v[0], 3), round(v[1], 3), round(v[2], 3))
        if key not in vertex_index:
            vertex_index[key] = len(verts)
            verts.append(list(key))
        return vertex_index[key]

    rows = []
    for tri in tris:
        attrs = tri[3]
        rows.append([vertex(tri[0]), vertex(tri[1]), vertex(tri[2]), code("class", classify(tri)),
                     code("material", attrs["material"]), code("layer", attrs["layer"]), int(attrs["hitsPlayer"])])
    return {"source": "game files (tools/phive.py)", "area": area, "map": mapname, "tiles": tiles,
            "triangleCount": len(tris), "bbox": bbox(tris), "vertices": verts,
            "triangleFields": ["v0", "v1", "v2", "class", "material", "layer", "hitsPlayer"],
            "classNames": names["class"], "materialNames": names["material"], "layerNames": names["layer"],
            "triangles": rows}


def write_obj(path, tris):
    """A Wavefront .obj with three vertices of its own per triangle."""
    with open(path, "w") as f:
        for tri in tris:
            for v in tri[:3]:
                f.write("v %.3f %.3f %.3f\n" % v)
        for i in range(len(tris)):
            f.write("f %d %d %d\n" % (3 * i + 1, 3 * i + 2, 3 * i + 3))


def main():
    args = sys.argv[1:]
    everything = "--all" in args
    local_dir = None
    if "--dir" in args:
        i = args.index("--dir")
        local_dir = args[i + 1]
        del args[i:i + 2]
    args = [arg for arg in args if arg != "--all"]
    if len(args) >= 3 and args[0] in ("map", "obj"):
        command, area, mapname = args[:3]
        paths = map_tiles(area, mapname, local_dir)
        if not paths:
            raise SystemExit("no static compound tiles for %s/%s" % (area, mapname))
        tiles, tris = map_triangles(paths, everything, log=lambda s: print(s, file=sys.stderr))
        if command == "obj":
            write_obj(args[3], tris)
            print("%d triangles -> %s" % (len(tris), args[3]), file=sys.stderr)
            return
        doc = map_json(area, mapname, tiles, tris)
        if len(args) > 3:
            with open(args[3], "w") as f:
                json.dump(doc, f, separators=(",", ":"))
            print("%d triangles, %d vertices -> %s" % (len(tris), len(doc["vertices"]), args[3]), file=sys.stderr)
        else:
            json.dump(doc, sys.stdout, separators=(",", ":"))
    elif len(args) == 2 and args[0] == "types":
        dump_types(args[1])
    elif len(args) == 1:
        summary(args[0])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
