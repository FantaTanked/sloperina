"""Build airwalls.json: every air wall placed in every map, with its collision shape.

    python airwalls.py            writes ../airwalls.json (needs OOTMENU_NUP and zstandard)

For each map (banc name, e.g. SacredForestMeadow_Child, the same as the "map" string in the save), every
placed actor whose name contains "AirWall": its position, rotation (radians, X Y Z) and scale from the
.bcett layout, and its shapes from the actor pack (ControllerSetParam ShapeNamePathAry, following $parent),
in the actor's own unscaled space. Box: centre (incl. OffsetTranslation) + half extents. Cylinder: two end
centres + radius. Other shapes (Polytope, PhshMesh) are counted but not stored. "mask" is the rigid body's
EnableLayerHitMask (HitOnlyPlayer, HitOnlyCharacter, ... or None = everything).
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import byml  # noqa: E402
import gamefiles  # noqa: E402

OUT = os.path.join(os.path.dirname(HERE), "data", "airwalls.json")
LAYOUT_SUFFIX = ".bcett.byml.zs"
NOT_SHAPES = ("Box", "Cylinder", "AutoCalc", "$parent")  # keys of a shape param that aren't other shape kinds


def member(path):
    """'Work/Phive/X/Y.phive__X.gyml' -> the pack member name 'Phive/X/Y.phive__X.bgyml'."""
    name = path[len("Work/"):] if path.startswith("Work/") else path
    if name.endswith(".gyml"):
        return name[:-len(".gyml")] + ".bgyml"
    return name


def load_chain(files, name):
    """A pack BYML with its $parent chain merged (child keys win)."""
    merged, seen = {}, set()
    while name and name in files and name not in seen:
        seen.add(name)
        params = byml.load(files[name])
        for key, value in params.items():
            merged.setdefault(key, value)
        name = member(params["$parent"]) if "$parent" in params else None
    return merged


def v3(vector, default=0.0):
    """An {X, Y, Z} dict as [x, y, z] (missing parts: default); anything else as [default] * 3."""
    if not isinstance(vector, dict):
        return [default] * 3
    return [float(vector.get(axis, default)) for axis in ("X", "Y", "Z")]


def _shapes_of(shape_param):
    """The boxes and cylinders of a shape param, as airwalls.json shapes."""
    shapes = []
    for box in shape_param.get("Box") or []:
        centre, offset = v3(box.get("Center")), v3(box.get("OffsetTranslation"))
        shapes.append({"type": "box", "center": [centre[i] + offset[i] for i in range(3)],
                       "half": v3(box.get("HalfExtents"), 1.0)})
    for cylinder in shape_param.get("Cylinder") or []:
        shapes.append({"type": "cylinder", "a": v3(cylinder.get("CenterA")), "b": v3(cylinder.get("CenterB")),
                       "radius": float(cylinder.get("Radius", 1.0))})
    return shapes


_cache = {}


def actor_shapes(actor):
    """(shapes, mask, skipped) for an actor name, from its pack."""
    if actor in _cache:
        return _cache[actor]
    path = f"romfs/Pack/Actor/{actor}.pack.zs"
    result = ([], None, [])
    try:
        files = gamefiles.sarc(gamefiles.read(path))
    except KeyError:  # no pack for this actor
        _cache[actor] = result
        return result
    physics = load_chain(files, f"Component/Physics/{actor}.engine__component__PhysicsParam.bgyml")
    if "ControllerSetPath" in physics:
        controller_set_name = member(physics["ControllerSetPath"])
    else:
        controller_set_name = f"Phive/ControllerSetParam/{actor}.phive__ControllerSetParam.bgyml"
    controller_set = load_chain(files, controller_set_name)
    shapes, skipped = [], []
    for ref in controller_set.get("ShapeNamePathAry") or []:
        shape_param = load_chain(files, member(ref["FilePath"]))
        shapes += _shapes_of(shape_param)
        skipped += [key for key in shape_param if key not in NOT_SHAPES]
    mask = None
    for ref in controller_set.get("RigidBodyEntityNamePathAry") or []:
        rigid_body = load_chain(files, member(ref["FilePath"]))
        mask = rigid_body.get("EnableLayerHitMask", mask)
    result = (shapes, mask, skipped)
    _cache[actor] = result
    return result


def _scale_of(placed):
    """A placed actor's scale as [x, y, z]: the layout has either a list or one number for all three."""
    scale = placed.get("Scale")
    if isinstance(scale, list):
        return [float(v) for v in scale or [1, 1, 1]]
    return [float(placed.get("Scale", 1.0))] * 3


def main():
    nup = gamefiles.nup()
    layouts = sorted(path for path in nup.index if path.startswith("romfs/Banc/") and path.endswith(LAYOUT_SUFFIX))
    maps, skipped_total = {}, {}
    for path in layouts:
        stem = path.rsplit("/", 1)[1][:-len(LAYOUT_SUFFIX)]
        # each map has a _Static and a _Dynamic layout
        part = stem.rsplit("_", 1)[-1]
        map_name = stem.rsplit("_", 1)[0] if part in ("Static", "Dynamic") else stem
        try:
            root = byml.load(gamefiles.read(path))
        except Exception as e:  # a file this reader can't handle: report and go on
            print(f"skip {path}: {e}")
            continue
        for placed in root.get("Actors") or []:
            actor = placed.get("Gyaml", "")
            if "AirWall" not in actor:
                continue
            shapes, mask, skipped = actor_shapes(actor)
            for kind in skipped:
                skipped_total[kind] = skipped_total.get(kind, 0) + 1
            if not shapes:
                continue
            maps.setdefault(map_name, []).append({
                "actor": actor, "part": part, "mask": mask, "hash": placed.get("Hash"),
                "t": [float(v) for v in placed.get("Translate", [0, 0, 0])],
                "r": [float(v) for v in placed.get("Rotate") or [0, 0, 0]],
                "s": _scale_of(placed),
                "shapes": shapes,
            })
    with open(OUT, "w") as f:
        json.dump({"source": "game files (tools/airwalls.py)", "maps": maps}, f, separators=(",", ":"))
    total = sum(len(walls) for walls in maps.values())
    print(f"{total} air walls in {len(maps)} maps -> {OUT}")
    if skipped_total:
        print("shapes not stored (not box/cylinder):", skipped_total)


if __name__ == "__main__":
    main()
