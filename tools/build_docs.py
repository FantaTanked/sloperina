"""Rebuild the browsable field / quest / tutorial reference (docs/reference/) from the data files.

    python3 tools/build_docs.py

Inputs (data/): gamedata_fields.json, story.json. Outputs (docs/reference/): fields.js, quests.html,
guides.html. index.html, fields.html and style.css are hand-written and only read the generated data.
"""
import html
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs" / "reference"
DATA = ROOT / "data"

NAV = ('<nav><a href="index.html">overview</a><a href="fields.html">save fields</a><a href="quests.html">quests</a>'
       '<a href="guides.html">tutorials</a></nav>')
ARROW = " &rarr; "  # between stages, in order


def page(title, body):
    """A whole reference page: the shared head and navigation bar around body."""
    return f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)}</title><link rel="stylesheet" href="style.css"></head>
<body>{NAV}
<main>{body}</main></body></html>
"""


def quest_label(name):
    """'Step_Get_StoneOfForest' -> 'Get Stone Of Forest'."""
    words = re.sub(r"^Step_", "", name).replace("_", " ")
    return re.sub(r"(?<=[a-z])(?=[A-Z])", " ", words)


def stage_list(stages):
    return ARROW.join(f"<code>{html.escape(stage)}</code>" for stage in stages)


def write_fields_js(fields):
    """fields.js: every save field, for fields.html to show and search."""
    data = json.dumps(fields, ensure_ascii=False, separators=(",", ":"))
    (DOCS / "fields.js").write_text("window.FIELDS=" + data + ";", encoding="utf-8")


def write_quests(fields, story):
    """quests.html: each quest's stages; then quest-like fields whose name wasn't recovered."""
    rows = []
    for hash_hex, quest in sorted(story["quests"].items(), key=lambda item: item[1]["name"]):
        name = quest["name"]
        rows.append(f"<tr><td>{html.escape(quest_label(name))}</td><td><code>{html.escape(name)}</code>"
                    f"<br><span class=dim>0x{int(hash_hex, 16):08x}</span></td>"
                    f"<td>{stage_list(quest['stages'])}</td></tr>")
    # an unnamed Enum field with a "Complete" value is most likely a quest too
    unnamed = [field for field in fields
               if field["t"] == "Enum" and "Complete" in (field.get("v") or []) and not field["n"]]
    for field in unnamed:
        rows.append(f"<tr><td class=dim>(name not recovered)</td><td><span class=dim>0x{field['h']}</span></td>"
                    f"<td>{stage_list(field['v'])}</td></tr>")
    (DOCS / "quests.html").write_text(page("Quest stages", f"""
<h1>Quest stages</h1>
<p>Story progress is not a set of flags: each quest is one <b>Enum</b> save field whose value is the
MurmurHash3 of the current stage name. The game checks progress with expressions such as
<code>Get_StoneOfForest_IsCompleted_Exp</code> (stage == <code>Complete</code>) and
<code>..._IsAfter_DefeatBoss_Exp</code> (stage &ge; <code>DefeatBoss</code>), so setting a quest's
stage is what makes the world react. Changes show after a loading zone.</p>
<p>In ootmenu: <b>story</b> page, one selector per quest.</p>
<table><thead><tr><th>quest</th><th>field</th><th>stages, in order</th></tr></thead><tbody>
{''.join(rows)}</tbody></table>"""), encoding="utf-8")


def write_guides(story):
    """guides.html: the tutorial ("action guide") types and their seen flags."""
    rows = "".join(f"<tr><td>{html.escape(name)}</td><td><code>{field_hash}</code></td></tr>"
                   for name, field_hash in sorted(story["action_guides"].items()))
    (DOCS / "guides.html").write_text(page("Tutorial guides", f"""
<h1>Tutorial guides (ActionGuide_IsEnd)</h1>
<p>The game records which tutorial screens ("action guides") the player has seen in the struct
<code>ActionGuide_IsEnd</code>: one Bool field per guide type. Some features stay hidden until their guide
has been seen. Example: the D-pad (Navi, item shortcuts, ocarina) only appears once the <b>Navi</b> guide,
normally shown on entering the Deku Tree, is marked seen.</p>
<p>In ootmenu: <b>story &rarr; all tutorials seen</b>.</p>
<table><thead><tr><th>guide type</th><th>Bool field</th></tr></thead><tbody>{rows}</tbody></table>"""),
                                      encoding="utf-8")


def main():
    fields = json.loads((DATA / "gamedata_fields.json").read_text(encoding="utf-8"))
    write_fields_js(fields)
    story = json.loads((DATA / "story.json").read_text(encoding="utf-8"))
    write_quests(fields, story)
    write_guides(story)
    print(f"docs: {len(fields)} fields, {len(story['quests'])} quests, {len(story['action_guides'])} guides")


if __name__ == "__main__":
    main()
