"""Write spawn_starters.json: for each warps.json spawn, the placed actors (other than its StartPos)
that share its name in the map's layouts.

    python tools/spawnstarters.py            (set OOTMENU_NUP; needs zstandard)

Spawns that exits lead to have the exit actor there (FieldAndDungeonEntranceEventRequestActor), which
the standard arrival event (Dm_SY_BancChangeArrive) uses as its event starter. Spawns without one
(warp-song pads, blue warps, boss rooms, restart points) are only ever entered with no starter and
their own arrival event; the native menu's warps use OcarinaWarp_Arrive for those (tools/gendata.py).
"""
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import byml  # noqa: E402
import gamefiles  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
LAYOUT_SUFFIX = ".bcett.byml.zs"


def layouts_by_map():
    """{map name: [romfs paths of its _Static / _Dynamic layouts]}."""
    layouts = {}
    for path in gamefiles.nup().index:
        if path.startswith("romfs/Banc/") and path.endswith(LAYOUT_SUFFIX):
            base = path.rsplit("/", 1)[-1][:-len(LAYOUT_SUFFIX)]
            for part in ("_Static", "_Dynamic"):
                if base.endswith(part):
                    layouts.setdefault(base[:-len(part)], []).append(path)
    return layouts


def collect_named_actors(node, names):
    """Add {Name: {Gyaml, ...}} for every named placed actor (other than StartPos) anywhere in a layout."""
    if isinstance(node, dict):
        if "Gyaml" in node and node.get("Gyaml") != "StartPos" and node.get("Name"):
            names.setdefault(node["Name"], set()).add(node["Gyaml"])
        for value in node.values():
            collect_named_actors(value, names)
    elif isinstance(node, list):
        for value in node:
            collect_named_actors(value, names)


def main():
    spawns = json.loads((ROOT / "data" / "warps.json").read_text())["spawns"]
    layouts = layouts_by_map()
    out = {}
    for map_name, map_spawns in spawns.items():
        names = {}
        for path in layouts.get(map_name, []):
            collect_named_actors(byml.load(gamefiles.read(path)), names)
        out[map_name] = {position: sorted(names.get(position, [])) for position, _, _ in map_spawns}
    (ROOT / "data" / "spawn_starters.json").write_text(json.dumps(out, indent=1, sort_keys=True))
    print(f"spawn_starters.json: {sum(len(v) for v in out.values())} spawns")


if __name__ == "__main__":
    main()
