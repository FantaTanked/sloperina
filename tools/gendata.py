"""Generate src/ootgz/data.c (the native menu's tables) from this folder's JSON files.

    python tools/gendata.py

Run it after changing data/field_hashes.json, extra_fields.json, story.json or warps.json and commit
the result: the native build itself doesn't need Python. The lists below mirror ootmenu.pyw.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

# --- same lists as ootmenu.pyw -----------------------------------------------------------------
# The item screen, in order: (kind, field title). "owned" items have an owned flag; "acquired" ones only an
# acquisition-history entry; "trade" and "ocarina" are the special slots (their title is just a label).
ITEM_SCREEN = ([("acquired", "Deku sticks"), ("owned", "Fairy Slingshot"), ("owned", "Boomerang"),
                ("acquired", "Deku nuts"), ("acquired", "Bombs"), ("acquired", "Bombchus"), ("owned", "Hammer"),
                ("owned", "Hookshot"), ("owned", "Longshot"), ("owned", "Fairy Bow"), ("owned", "Arrow Fire"),
                ("owned", "Arrow Ice"), ("owned", "Arrow Light"), ("acquired", "Magic Bean"),
                ("owned", "Din’s Fire"), ("owned", "Farore’s Wind"), ("owned", "Nayru’s Love")]
               + [("trade", "child"), ("trade", "adult")]
               + [("owned", "Lens of Truth"), ("owned", "Gerudo membership card"), ("ocarina", "ocarina")])
GEAR = ["Kokiri Sword", "Master Sword", "Giant’s Knife", "Broken Giant’s Knife", "Biggoron’s Sword",
        "Deku Shield", "Hylian Shield", "Mirror Shield", "Goron Tunic", "Zora Tunic", "Iron Boots", "Hover Boots",
        # owned flags only (inventory > collection); giving one for real is the ocarina switch
        "Fairy Ocarina", "Ocarina of Time"]
# (label, item name, button item); the first N_CHILD_TRADE_ITEMS are the child's, the rest the adult's
TRADE_ITEMS = [("weird egg", "Item_WeirdEgg", "WeirdEgg"), ("cucco", "Item_AlarmCucco", "AlarmCucco"),
               ("zelda's letter", "Item_ZeldaLetter", "ZeldaLetter"), ("pocket egg", "Item_PocketEgg", "PocketEgg"),
               ("pocket cucco", "Item_PocketCucco", "PocketCucco"), ("cojiro", "Item_CojiroCucco", "CojiroCucco"),
               ("odd mushroom", "Item_OddMushroom", "OddMushroom"), ("odd potion", "Item_OddPowder", "OddPowder"),
               ("poacher's saw", "Item_PoacherSaw", "PoacherSaw"), ("broken sword", "Item_BrokenSword", "BrokenSword"),
               ("prescription", "Item_Prescription", "Prescription"),
               ("eyeball frog", "Item_EyeballFrog", "EyeballFrog"), ("eye drops", "Item_EyeDrop", "EyeDrop"),
               ("claim check", "Item_ClaimCheck", "ClaimCheck")]
N_CHILD_TRADE_ITEMS = 3  # weird egg, cucco, Zelda's letter


def _mask_row(mask):
    """(label, item name, button item) for a mask, in the same shape as TRADE_ITEMS."""
    label = "bunny hood" if mask == "Bunny" else f"{mask.lower()} mask"
    return (label, f"Armor_Child_Mask_{mask}", f"{mask}Mask")


MASKS = [_mask_row(mask) for mask in ("Keaton", "Skull", "Spooky", "Bunny", "Goron", "Zora", "Gerudo", "Truth")]
# item title -> the item name the game uses when it is put on a button
BUTTON_ITEM = {"Fairy Slingshot": "SlingShot", "Fairy Bow": "Bow", "Arrow Fire": "FireArrow",
               "Arrow Ice": "IceArrow", "Arrow Light": "LightArrow", "Boomerang": "Boomerang",
               "Hookshot": "Hookshot", "Longshot": "Longshot", "Hammer": "Hammer",
               "Lens of Truth": "GlassTruth", "Din’s Fire": "DinFire", "Farore’s Wind": "FaroreWind",
               "Nayru’s Love": "NayruLove", "Iron Boots": "HeavyBoot", "Hover Boots": "HoverBoot"}
# equipment slot -> the gamedata.h hash of the String64 save field that holds what is worn there
GD_STR = {"sword": "GD_STR_SWORD", "shield": "GD_STR_SHIELD", "tunic": "GD_STR_TUNIC", "boots": "GD_STR_BOOTS"}
# gear title -> (equipment slot, the value that slot holds when it is worn)
WORN_GEAR = {"Kokiri Sword": ("sword", "Weapon_Sword_Kokiri"), "Master Sword": ("sword", "Weapon_Sword_Master"),
             "Giant’s Knife": ("sword", "Weapon_Sword_Giant"),
             "Broken Giant’s Knife": ("sword", "Weapon_Sword_BrokenGiant"),
             "Biggoron’s Sword": ("sword", "Weapon_Sword_Biggoron"), "Deku Shield": ("shield", "Weapon_Shield_Deku"),
             "Hylian Shield": ("shield", "Weapon_Shield_Hylian"), "Mirror Shield": ("shield", "Weapon_Shield_Mirror"),
             "Goron Tunic": ("tunic", "Armor_Adult_Goron"), "Zora Tunic": ("tunic", "Armor_Adult_Zora"),
             "Iron Boots": ("boots", "Armor_Adult_HeavyBoot"), "Hover Boots": ("boots", "Armor_Adult_HoverBoot")}
# (label, count field title, capacity field title or a fixed capacity, acquisition-history title)
AMMO = [("deku sticks", "Deku sticks", "Deku Stick capacity", "Deku sticks"),
        ("deku nuts", "Deku nuts", "Deku Nut capacity", "Deku nuts"),
        ("deku seeds", "Deku seeds", "Deku Seed capacity", "Deku Seed"),
        ("bombs", "Bombs", "Bomb capacity", "Bombs"), ("arrows", "Arrows", "Arrow capacity", "Arrows"),
        ("bombchus", "Bombchus", 50, "Bombchus"), ("magic beans", "Magic Bean", 10, "Magic Bean")]
SONGS = ["Zelda's Lullaby", "Epona's Song", "Saria's Song", "Sun's Song", "Song of Time", "Song of Storms",
         "Scarecrow's Song", "Minuet of Forest", "Bolero of Fire", "Serenade of Water", "Requiem of Spirit",
         "Nocturne of Shadow", "Prelude of Light"]
# the spiritual stones as the menu names them (the save's titles have "Kokiri's", "Goron's", "Zora's")
QUEST_ITEM_LABELS = {"Kokiri's Emerald": "kokiri emerald", "Goron's Ruby": "goron ruby",
                     "Zora's Sapphire": "zora sapphire"}
QUEST_ITEMS = ["Kokiri's Emerald", "Goron's Ruby", "Zora's Sapphire", "Forest Medallion", "Fire Medallion",
               "Water Medallion", "Spirit Medallion", "Shadow Medallion", "Light Medallion"]
# (dungeon name in the save fields, label)
DUNGEON_LIST = [("Inside The Deku Tree", "deku tree"), ("Dodongo Cavern", "dodongo's cavern"),
                ("Inside Jabu Jabu Belly", "jabu-jabu"), ("Forest Temple", "forest temple"),
                ("Fire Temple", "fire temple"), ("Water Temple", "water temple"),
                ("Shadow Temple", "shadow temple"), ("Spirit Temple", "spirit temple"),
                ("Bottom Of The Well", "bottom of well"), ("Ice Cavern", "ice cavern"),
                ("Gerudo Training Ground", "gerudo training"), ("Gerudo Fortress", "gerudo fortress"),
                ("Inside Ganon Castle", "ganon's castle"), ("Final Battle Field", "final battle")]
# (field under dungeons/<dungeon>/, label, acquisition-history title)
DUNGEON_ITEMS = [("Map", "map", "Dungeon Map"), ("Compass", "compass", "Dungeon Compass"),
                 ("Boss key", "boss key", "Dungeon Boss Key")]
# bottle contents -> acquisition-history title
BOTTLE_ACQ = {"Red Potion": "Bottle Red Potion", "Green Potion": "Bottle Green Potion",
              "Blue Potion": "Bottle Blue Potion", "Blue Fire": "Bottle Blue Fire", "Fish": "Bottle Fish",
              "Letter": "Bottle Letter", "Big Poe": "Bottle Big Poe", "Poe": "Bottle Poe", "Bug": "Bottle Bug",
              "Fairy": "Bottle Fairy", "Max Milk": "Bottle Milk", "Half Milk": "Bottle Half Milk"}
DUNGEON_KEYS = ["Forest Temple", "Fire Temple", "Water Temple", "Shadow Temple", "Spirit Temple",
                "Bottom Of The Well", "Gerudo Training Ground", "Inside Ganon Castle"]
# quest field name -> label, where the name made readable (quest_label) isn't clear enough
QUEST_NAMES = {"Step_Get_StoneOfForest": "deku tree", "Step_Get_StoneOfFire": "dodongo's cavern",
               "Step_Get_StoneOfWater": "jabu-jabu", "Step_SageOfForest": "forest temple",
               "Step_SageOfFire": "fire temple", "Step_SageOfWater": "water temple",
               "Step_SageOfShadow": "shadow temple", "Step_SageOfSpirit": "spirit temple",
               "Step_EscapeFromCrumblingGanonCastle": "castle escape", "Step_GatheringTokenGold": "skulltulas",
               "Step_SubEvent_GetSwordBigGoron": "biggoron sword", "Step_SubEvent_AllBossBattle": "boss rush",
               "Step_Goto_HyruleCastle": "zelda / castle", "Step_Meet_DekuTree": "kokiri sword",
               "Step_Minigame_SkullKidSession": "skull kid song"}
# spawns that only work once something in the world has been done
GATED_SPAWNS = {"MountainTrail_DodongoCavernSide": "boulder bombed",
                "GoronCity_LostWoodSide_before": "lost woods wall bombed",
                "GoronCity_LostWoodSide_after": "lost woods wall bombed",
                "LostWood_GoronCitySide_before": "goron city wall bombed",
                "LostWood_GoronCitySide_after": "goron city wall bombed"}
# maps that count as dungeons (warp category and the warp_map_t dungeon flag)
DUNGEONS = {"InsideTheDekuTree", "DodongoCavern", "InsideJabuJabuBelly", "ForestTemple", "FireTemple",
            "WaterTemple", "ShadowTemple", "SpiritTemple", "BottomOfTheWell", "IceCavern",
            "GerudoTrainingGround", "InsideGanonCastle", "CrumblingGanonCastle"}
# typos in the game's map and spawn names
NAME_FIXES = {"hounted": "haunted", "musuem": "museum", "caslte": "castle", "wateland": "wasteland",
              "caven": "cavern", "grave yard": "graveyard", "ganon castle": "ganon's castle",
              "link bed": "link's bed", "enemy museum": "hall of fallen foes"}  # after 'musuem' above
# whole words: abbreviations spelled out, the game's 'Lear' (rear), and place names as the
# game says them ('LostWood': lost woods, 'ZoraDomain': zora's domain)
WORD_FIXES = {"pos": "position", "lear": "rear", "lost wood": "lost woods",
              "zora domain": "zora's domain", "zora river": "zora's river", "zora fountain": "zora's fountain"}
# spawns whose label no rule below gets right (by spawn name, in whichever map has it)
SPAWN_LABELS = {"GanonCastleEntrance_FromInside": "from inside ganon's castle",
                "Market_Entrance_FromCastle": "from ganon's castle",
                "KakarikoWindmill_GraveYardSide": "windmill (from graveyard)",
                "GraveYard_KakarikoWindmillSide": "from kakariko windmill",
                "Graveyard_Adult/4": "in front of dampe"}
# spawns left out of the menu: the same place as another ('<map>/<spawn>')
HIDDEN_SPAWNS = {"Graveyard_Adult/5", "Graveyard_Adult/6"}  # as 4: in front of dampe
# warp menu categories, indexed like data.h's WC_* enum
CATEGORIES = ["overworld (child)", "overworld (adult)", "dungeons", "boss & battle rooms", "other"]
WC_CHILD, WC_ADULT, WC_DUNGEON, WC_BOSS, WC_OTHER = range(len(CATEGORIES))

# air walls: airshape_t type, and airwall_t kind (does it stop Link?)
SHAPE_BOX, SHAPE_CYLINDER = 0, 1
WALL_STOPS_PLAYER, WALL_IGNORES_PLAYER = 0, 1
PLAYER_MASKS = {"HitOnlyPlayer", "Default", None}  # rigid-body hit masks that stop Link (airwall.py PLAYER_MASKS)

NO_FIELD = "{ 0, FT_NONE }"
FIELD_TYPES = {"Int": "FT_INT", "Bool": "FT_BOOL", "Enum": "FT_ENUM", "Float": "FT_FLOAT"}


def pretty(name):
    """A game name made readable: 'KokiriForest_Child' -> 'kokiri forest (child)'."""
    parts = name.split("_")
    tags = [part.lower() for part in parts[1:] if part in ("Child", "Adult", "Alternative")]
    words = " ".join(part for part in parts if part.lower() not in tags)
    # split CamelCase and letter/digit runs into words
    words = re.sub(r"(?<=[a-z])(?=[A-Z0-9])|(?<=[A-Z])(?=[A-Z][a-z])", " ", words).lower()
    for typo, fixed in NAME_FIXES.items():
        words = words.replace(typo, fixed)
    for word, fixed in WORD_FIXES.items():
        words = re.sub(rf"\b{word}\b", fixed, words)
    words = re.sub(r"\bstart position\b", "starting position", words)  # not 'restart position'
    if tags:
        return f"{words} ({' '.join(tags)})"
    return words


def map_label(name):
    """pretty() without the (child) / (adult) tag: the warps menu's category (breadcrumb) says it."""
    label = re.sub(r" \((child|adult) ", " (", pretty(name))
    return re.sub(r" \((child|adult)\)$", "", label)


def quest_label(name):
    """A quest field name as a menu label: 'Step_SubEvent_GetSwordBigGoron' -> 'biggoron sword'."""
    if name in QUEST_NAMES:
        return QUEST_NAMES[name]
    words = re.sub(r"^Step_(Get_|Goto_|SubEvent_|Mini[Gg]ame_)?", "", name).replace("_", " ")
    return re.sub(r"(?<=[a-z])(?=[A-Z])", " ", words).lower()


def label_of(title):
    return title.lower().replace("’", "'")


def category(name):
    """The warp menu category (WC_*) of a map."""
    if name in DUNGEONS:
        return WC_DUNGEON
    if name.endswith("Stage") or name in ("BossRushIntervalRoom", "FinalBattleField"):
        return WC_BOSS
    if name.endswith("_Child"):
        return WC_CHILD
    if "_Adult" in name or name in ("HyruleCastle", "GanonCastle"):
        return WC_ADULT
    return WC_OTHER


def c_string(text):
    """A C string literal (or NULL); the menu font has no curly apostrophe."""
    if text is None:
        return "NULL"
    return json.dumps(text.replace("’", "'"))


def c_float(value):
    """A C float literal: 1 -> '1.0f', 0.25 -> '0.25f', 1e-05 -> '1e-05f'."""
    text = f"{value:g}"
    if not any(ch in text for ch in ".en"):  # an integer: make it a float literal
        text += ".0"
    return text + "f"


def c_vec3(vector):
    return f"{{ {c_float(vector[0])}, {c_float(vector[1])}, {c_float(vector[2])} }}"


def read_json(name):
    return json.loads((ROOT / "data" / name).read_text(encoding="utf-8"))


class DataWriter:
    """Collects the lines of data.c and looks up save fields by title."""

    def __init__(self, field_hashes, extra_fields):
        self.lines = []
        self.extra = extra_fields
        # title -> (hash, type) of the save fields, leaving out the two bookkeeping groups
        self.fields = {}
        # title -> hash of its "Acquisition history" entry (the game's record that an item was ever got)
        self.acquired = {}
        for hash_hex, meta in field_hashes.items():
            group = meta.get("group")
            if group not in ("Acquisition history", "Assignment order"):
                self.fields[meta["title"]] = (int(hash_hex, 16), meta.get("type", "Int"))
            if group == "Acquisition history":
                self.acquired[meta["title"]] = int(hash_hex, 16)

    def write(self, line):
        self.lines.append(line)

    def end_array(self, name, blank_line=True):
        """Close an array and define its data_n_* element count."""
        count_name = name.replace("data_", "data_n_")
        self.write(f"}};\nconst int {count_name} = sizeof({name}) / sizeof(*{name});" + ("\n" if blank_line else ""))

    def field_hash(self, title):
        return self.fields[title][0]

    def extra_hash(self, key):
        return int(self.extra[key]["hash"], 16)

    def acquired_hash(self, title):
        return f"{self.acquired.get(title, 0):#010x}"

    @staticmethod
    def field(field_hash, field_type):
        """A field_t initialiser."""
        return f"{{ {field_hash:#010x}, {FIELD_TYPES[field_type]} }}"

    def extra_field(self, key):
        """field_t of an extra_fields.json entry, or an empty field if it isn't there."""
        if key not in self.extra:
            return NO_FIELD
        return self.field(self.extra_hash(key), self.extra[key]["type"])

    def inv_item(self, title, kind):
        """An inv_item_t row for an item with an owned flag (and maybe a button item and a worn slot)."""
        field_hash, field_type = self.fields.get(title, (0, "Int"))
        slot, worn = WORN_GEAR.get(title, (None, None))
        slot_string = GD_STR[slot] if slot else 0
        return (f"  {{ {c_string(label_of(title))}, {kind}, {self.field(field_hash, field_type)}, "
                f"{self.acquired_hash(title)}, {c_string(BUTTON_ITEM.get(title))}, {slot_string}, {c_string(worn)} }},")

    def flag(self, label, keys, acquired_title=None):
        """A flag_t: up to two extra fields set together, and the acquisition entry to set with them."""
        flag_fields = [self.extra_field(key) for key in keys] + [NO_FIELD] * (2 - len(keys))
        acquired = self.acquired_hash(acquired_title) if acquired_title else 0
        return f"{{ {c_string(label)}, {{ {flag_fields[0]}, {flag_fields[1]} }}, {acquired} }}"

    def pouch_table(self, name, rows):
        """A pouch_one_t table: items that share one pouch slot (masks, trade items)."""
        self.write(f"const pouch_one_t {name}[] = {{")
        for label, item, button in rows:
            self.write(f"  {{ {c_string(label)}, {c_string(item)}, {c_string(button)} }},")
        self.end_array(name)


def write_items(out):
    """The item screen, the gear, the pouches and the ammo."""
    out.write("const inv_item_t data_items[] = {")
    for kind, title in ITEM_SCREEN:
        if kind == "trade":
            trade_kind = "IK_TRADE_CHILD" if title == "child" else "IK_TRADE_ADULT"
            out.write(f"  {{ {c_string(title + ' trade')}, {trade_kind}, {NO_FIELD}, 0, NULL, 0, NULL }},")
        elif kind == "ocarina":
            out.write(f'  {{ "ocarina", IK_OCARINA, {NO_FIELD}, 0, NULL, 0, NULL }},')
        elif kind == "acquired":
            out.write(f"  {{ {c_string(label_of(title))}, IK_ACQUIRED, {NO_FIELD}, {out.acquired_hash(title)}, "
                      f"NULL, 0, NULL }},")
        else:
            out.write(out.inv_item(title, "IK_OWNED"))
    out.end_array("data_items")
    out.write("const inv_item_t data_gear[] = {")
    for title in GEAR:
        out.write(out.inv_item(title, "IK_OWNED"))
    out.end_array("data_gear")

    out.pouch_table("data_masks", MASKS)
    out.pouch_table("data_trade_child", TRADE_ITEMS[:N_CHILD_TRADE_ITEMS])
    out.pouch_table("data_trade_adult", TRADE_ITEMS[N_CHILD_TRADE_ITEMS:])

    out.write("const ammo_t data_ammo[] = {")
    for label, count_title, capacity, acquired_title in AMMO:
        capacity_field = out.field_hash(capacity) if isinstance(capacity, str) else 0
        fixed_capacity = capacity if isinstance(capacity, int) else 0
        out.write(f"  {{ {c_string(label)}, {out.field_hash(count_title):#010x}, {capacity_field:#010x}, "
                  f"{fixed_capacity}, {out.acquired_hash(acquired_title)} }},")
    out.end_array("data_ammo")


def write_quest_status(out):
    """Quest items, songs, dungeon items and keys, and the single flags and counters."""
    out.write("const flag_t data_quest_items[] = {")
    for title in QUEST_ITEMS:
        label = QUEST_ITEM_LABELS.get(title, label_of(title))
        out.write("  " + out.flag(label, [f"quests/Quest items/{title}", f"inventory/Items/{title}"]) + ",")
    out.end_array("data_quest_items")
    out.write("const flag_t data_songs[] = {")
    for title in SONGS:
        out.write("  " + out.flag(label_of(title), [f"quests/Songs/{title}"]) + ",")
    out.end_array("data_songs")
    out.write("const dungeon_t data_dungeons[] = {")
    for dungeon, label in DUNGEON_LIST:
        items = ", ".join(out.flag(item_label, [f"dungeons/{dungeon}/{item}"], acquired_title)
                          for item, item_label, acquired_title in DUNGEON_ITEMS)
        out.write(f"  {{ {c_string(label)}, {{ {items} }} }},")
    out.end_array("data_dungeons")
    out.write("const number_t data_small_keys[] = {")
    for dungeon in DUNGEON_KEYS:
        label = label_of(dungeon).replace("ganon castle", "ganon's castle")
        out.write(f"  {{ {c_string(label)}, {out.field_hash(f'{dungeon} small keys'):#010x} }},")
    out.end_array("data_small_keys")
    out.write(f'const number_t data_skulltulas = {{ "skulltulas", {out.field_hash("Gold Skulltula tokens"):#010x} }};')
    out.write("const flag_t data_double_defense = " + out.flag("double defense", ["link/Abilities/Double defense"])
              + ";")
    agony = out.flag("stone of agony", ["equipment/Effects & appearance/Stone of Agony equipped"], "Agony Stone")
    out.write("const flag_t data_agony = " + agony + ";\n")


def write_bottles_and_abilities(out):
    out.write("const bottle_t data_bottles[5] = {")
    for bottle in "ABCDE":
        item = out.extra_field(f"inventory/Items/Bottle {bottle}")
        available = out.extra_hash(f"inventory/Bottles/Bottle {bottle} available")
        contents = out.extra_hash(f"inventory/Bottles/Bottle {bottle} contents")
        out.write(f"  {{ {item}, {available:#010x}, {contents:#010x} }},")
    out.write("};")
    # every bottle has the same contents enum: (value, name) pairs
    choices = out.extra["inventory/Bottles/Bottle A contents"]["choices"]
    names = ", ".join(c_string(name.lower()) for _, name in choices)
    values = ", ".join(f"{value:#010x}" for value, _ in choices)
    acquired = ", ".join(out.acquired_hash(BOTTLE_ACQ.get(name, "")) for _, name in choices)
    out.write("const char *const data_bottle_names[] = { " + names + " };")
    out.write("const uint32_t data_bottle_values[] = { " + values + " };")
    out.write("const uint32_t data_bottle_acquired[] = { " + acquired + " };")
    out.write(f"const int data_n_bottle_contents = {len(choices)};")
    out.write(f"const uint32_t data_acquired_bottle = {out.acquired_hash('Bottle')};\n")

    strength = out.extra["link/Abilities/Strength"]
    strength_values = ", ".join(f"{value:#010x}" for value, _ in strength["choices"])
    out.write(f"const field_t data_strength = {out.extra_field('link/Abilities/Strength')};")
    out.write("const uint32_t data_strength_values[4] = { " + strength_values + " };")
    out.write(f"const field_t data_diving_depth = {out.extra_field('link/Abilities/Diving depth')};")
    out.write("const float data_scale_depths[3] = { 3.0f, 6.0f, 9.0f };\n")


def write_story(out, story):
    """The quests (sorted by label) with their stages, and the tutorial (action guide) fields. Returns the quests."""
    quests = sorted(story["quests"].items(), key=lambda item: quest_label(item[1]["name"]))
    for i, (_, quest) in enumerate(quests):
        stages = ", ".join(c_string(stage) for stage in quest["stages"])
        out.write(f"static const char *const stages_{i}[] = {{ " + stages + " };")
    out.write("const quest_t data_quests[] = {")
    for i, (hash_hex, quest) in enumerate(quests):
        out.write(f"  {{ {c_string(quest_label(quest['name']))}, {int(hash_hex, 16):#010x}, {len(quest['stages'])}, "
                  f"stages_{i} }},")
    out.end_array("data_quests")
    guides = story["action_guides"]
    guide_hashes = ", ".join(f"{int(hash_hex, 16):#010x}" for hash_hex in guides.values())
    out.write("const uint32_t data_action_guides[] = { " + guide_hashes + " };")
    out.write("const int data_n_action_guides = sizeof(data_action_guides) / sizeof(*data_action_guides);")
    out.write(f"const uint32_t data_action_guide_navi = {int(guides['Navi'], 16):#010x};\n")
    return quests


def name_parts(name):
    """The ways a place's name can be shortened in a spawn's name, without spaces: its whole name,
    or its start or end ('death mountain crater' -> 'crater', 'kakariko village' -> 'kakariko')."""
    words = name.split(" (")[0].split()
    return ({"".join(words[i:]) for i in range(len(words))}
            | {"".join(words[:i]) for i in range(1, len(words) + 1)})


def without_map_name(label, map_name):
    """A spawn's label without the words at its start that name its own map, which the breadcrumb
    already shows: in death mountain crater 'crater goron city side' -> 'goron city side', in
    kakariko village 'kakariko well side' -> 'well side' (spaces don't count). A label that is just
    the map's name is its entrance."""
    parts = name_parts(map_label(map_name))
    words = label.split()
    for n in range(len(words), 0, -1):
        if "".join(words[:n]) in parts:
            return " ".join(words[n:]) or "entrance"
    return label


def spawn_label(position, source, map_name):
    """A spawn's menu label: where it is, in words that make sense under its map's breadcrumb, plus
    where it's entered from and what it needs.

    'goron city crater side' (from death mountain crater), in goron city -> 'from death mountain
    crater'; 'start pos sage of fire' -> 'starting position (sage of fire)'; 'front of market' -> 'in
    front of market'; 'ShadowNocturne' -> 'from playing nocturne of shadow'. A spawn whose name is just
    a number is 'unnamed spawn <n>'."""
    origin = ""
    if source.startswith("from "):
        origin = pretty(source[len("from "):]).split(" (")[0]
    fixed = SPAWN_LABELS.get(f"{map_name}/{position}", SPAWN_LABELS.get(position))
    if fixed:
        return fixed
    # a warp song's pad ('ShadowNocturne'): by the song menu's name for the song
    words = pretty(position).split()
    song = len(words) == 2 and f"{words[1]} of {words[0]}"
    if song in {label_of(title) for title in SONGS}:
        return f"from playing {song}"
    if position.isdigit():
        label = f"unnamed spawn {position}"
    else:
        label = without_map_name(pretty(position), map_name)
        label = re.sub(r"\bfront of\b", "in front of", label)
        label = label.replace("sub event all boss battle", "boss rush").replace("sub event ", "")
        # '<kind> position <story step or place>' -> '<kind> position (<...>)'
        label = re.sub(r"^(starting position|restart position|entrance position) (?!\d)(.+)$", r"\1 (\2)", label)
        # '<place> side': the way in from that place
        side = re.fullmatch(r"(.+?) side\b(.*)", label)
        if side and (not origin or "".join(side[1].split()) in name_parts(origin)):
            label = f"from {origin or side[1]}{side[2]}"
    if origin and origin != label and not re.match(rf"from {re.escape(origin)}\b", label) \
            and f"(from {origin})" not in label:
        label = f"{label} (from {origin})"
    if position in GATED_SPAWNS:
        label = f"{label} [needs {GATED_SPAWNS[position]}]"
    return label


def write_warps(out, warps, starters):
    """The warp menu: maps by category then label, each with its spawns. Returns (maps, spawns)."""
    out.write("const char *const data_warp_categories[WC_COUNT] = { "
              + ", ".join(c_string(name) for name in CATEGORIES) + " };")
    spawns, maps = [], []
    for map_category in range(len(CATEGORIES)):
        names = sorted((name for name in warps["spawns"] if category(name) == map_category), key=pretty)
        for name in names:
            first = len(spawns)
            for position, event, source in warps["spawns"][name]:
                if f"{name}/{position}" in HIDDEN_SPAWNS:
                    continue
                # does the spawn have an event starter actor (tools/spawnstarters.py)?
                has_starter = int(bool(starters.get(name, {}).get(position)))
                spawns.append((spawn_label(position, source, name), position, event, has_starter))
            maps.append((name, map_label(name), map_category, int(name in DUNGEONS), first, len(spawns) - first))
    out.write("const warp_map_t data_warp_maps[] = {")
    for name, label, map_category, is_dungeon, first, count in maps:
        out.write(f"  {{ {c_string(name)}, {c_string(label)}, {map_category}, {is_dungeon}, {first}, {count} }},")
    out.end_array("data_warp_maps")
    out.write("const spawn_t data_spawns[] = {")
    for label, position, event, has_starter in spawns:
        out.write(f"  {{ {c_string(label)}, {c_string(position)}, {c_string(event)}, {has_starter} }},")
    out.end_array("data_spawns", blank_line=False)
    return maps, spawns


def write_airwalls(out, airwall_maps):
    """Air walls (airwalls.json, from tools/airwalls.py): per map, the walls with their shapes.

    Returns (walls, maps)."""
    out.write("")
    out.write("const airshape_t data_airshapes[] = {")
    walls, maps = [], []
    n_shapes = 0
    for name in sorted(airwall_maps):
        first_wall = len(walls)
        for wall in airwall_maps[name]:
            first_shape = n_shapes
            for shape in wall["shapes"]:
                if shape["type"] == "box":
                    shape_type = SHAPE_BOX
                    a, b, radius = shape["center"], shape["half"], 0.0
                else:
                    shape_type = SHAPE_CYLINDER
                    a, b, radius = shape["a"], shape["b"], shape["radius"]
                out.write(f"  {{ {shape_type}, {c_vec3(a)}, {c_vec3(b)}, {c_float(radius)} }},")
                n_shapes += 1
            kind = WALL_STOPS_PLAYER if wall.get("mask") in PLAYER_MASKS else WALL_IGNORES_PLAYER
            walls.append((wall["t"], wall["r"], wall["s"], first_shape, n_shapes - first_shape, kind))
        maps.append((name, first_wall, len(walls) - first_wall))
    out.write("};")
    out.write("const airwall_t data_airwalls[] = {")
    for translation, rotation, scale, first_shape, count, kind in walls:
        out.write(f"  {{ {c_vec3(translation)}, {c_vec3(rotation)}, {c_vec3(scale)}, "
                  f"{first_shape}, {count}, {kind} }},")
    out.write("};")
    out.write("const airmap_t data_airmaps[] = {")
    for name, first_wall, count in maps:
        out.write(f"  {{ {c_string(name)}, {first_wall}, {count} }},")
    out.end_array("data_airmaps", blank_line=False)
    return walls, maps


def main():
    field_hashes = read_json("field_hashes.json")
    extra = read_json("extra_fields.json")
    story = read_json("story.json")
    warps = read_json("warps.json")
    starters = read_json("spawn_starters.json")
    out = DataWriter(field_hashes, extra)

    out.lines += ["/* Generated by tools/gendata.py from field_hashes.json, extra_fields.json, story.json and",
                  " * warps.json. Don't edit by hand. */", '#include "data.h"', '#include "gamedata.h"', "",
                  "#define NULL ((void *)0)", ""]
    write_items(out)
    write_quest_status(out)
    write_bottles_and_abilities(out)
    quests = write_story(out, story)
    maps, spawns = write_warps(out, warps, starters)
    walls, airwall_maps = write_airwalls(out, read_json("airwalls.json")["maps"])

    missing = [title for kind, title in ITEM_SCREEN if kind in ("owned",) and title not in out.fields]
    missing += [title for title in GEAR if title not in out.fields]
    if missing:
        sys.exit(f"missing fields: {missing}")
    (ROOT / "src/ootgz/data.c").write_text("\n".join(out.lines) + "\n", encoding="utf-8")
    print(f"src/ootgz/data.c: {len(maps)} maps, {len(spawns)} spawns, {len(quests)} quests, "
          f"{len(walls)} air walls in {len(airwall_maps)} maps")


if __name__ == "__main__":
    main()
