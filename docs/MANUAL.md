# sloperinaGZ manual

Everything sloperinaGZ can do, menu by menu. New to it? Start with the [usage guide](USAGE.md).

## Contents

- [1 Introduction](#1-introduction)
  - [1.1 Controls](#11-controls)
  - [1.2 Reading the menu](#12-reading-the-menu)
  - [1.3 Shortcuts in the game](#13-shortcuts-in-the-game)
- [2 Menus](#2-menus)
  - [2.1 Link](#21-link)
  - [2.2 Warps](#22-warps)
  - [2.3 Scene](#23-scene)
  - [2.4 Cheats](#24-cheats)
  - [2.5 Inventory](#25-inventory)
  - [2.6 Equips](#26-equips)
  - [2.7 Story](#27-story)
  - [2.8 Watches](#28-watches)
  - [2.9 Debug](#29-debug)
  - [2.10 Settings](#210-settings)
  - [2.11 Log](#211-log)
  - [2.12 About](#212-about)
- [3 On the screen](#3-on-the-screen)
- [4 About frame advance](#4-about-frame-advance)
- [5 Settings file](#5-settings-file)
- [6 Languages](#6-languages)
- [7 Known limits](#7-known-limits)

## 1 Introduction

sloperinaGZ is a practice menu for **The Legend of Zelda: Ocarina of Time (Switch 2)**, versions 1.0.0 and 1.1.1, modelled
on gz, the practice ROM for the N64 game. The game loads it itself, so it runs on a console (Atmosphère) and in
emulators alike. Open the menu with **R + right stick down**.

### 1.1 Controls

| Button | Does |
|---|---|
| **R + right stick down** | Open / close the menu |
| **D-pad up / down** | Move |
| **D-pad left / right** | Change the value, or pick a button in a row |
| **L** | Select / press |
| **L held + D-pad left / right** | Numbers: change by 10 |
| **R + D-pad left** | Back one menu (closes the menu at the top) |

While the menu is open the game doesn't see the controller.

### 1.2 Reading the menu

- The **top line** shows where you are, e.g. `sloperinaGZ > inventory > collection`.
- The **bottom line** says what the buttons do for the selected item.
- **Grey lines** are headings or information; they can't be selected. Items under a heading are indented.
- `name >` opens another menu. `☑ name` is a checkbox. `name  < value >` is a switch. `name  < 3 > / 9` is a
  number (with its maximum).
- Some rows are **rows of buttons** (watches, the memory viewer): D-pad left / right picks a button, which turns
  blue, and **L** presses it.
- A **^** or **v** at the side means the list goes on above or below.
- Read-only information sits under **return**, after a blank line.

### 1.3 Shortcuts in the game

With the menu closed:

| Shortcut | Does |
|---|---|
| **R + D-pad right** | Show / hide the watches |
| **R + A + D-pad left** | Start / stop the timer |
| **R + B + D-pad left** | Reset the timer |
| **R + B + D-pad right** | Reset the lag counter |
| **R + A + D-pad right** | Reset **JS Height** to FLT_MAX (as after a load: the jumpslash then works at any height) |
| **R + D-pad up** | Save position: where Link is and which way he faces |
| **R + D-pad down** | Load position: put Link back there (same map only) |
| **L3** | Levitate on / off (if enabled, [2.10.2](#2102-levitate)) |
| **R3** held | Levitate: hold your height |
| **L** or **R** (your choice) | Frame advance (if enabled, [2.10.3](#2103-frame-advance)) |

## 2 Menus

### 2.1 Link

| Item | What it does |
|---|---|
| **health** | Link's health in hearts and quarters. D-pad changes it a quarter heart at a time; **L** fills it. |
| **heart containers** | 1 to 20. Lowering it below your health lowers your health too. |
| **rupees** | Up to your wallet's size. **L** fills it. |
| **age** | **child** or **adult**: what the next **reload map** gives you. |
| **reload map** | Reloads the map you're in, at the age chosen above. Changing age loads the other age's version of the map and swaps sword and shield with the ones from the other age (adult-only tunics and boots come off a child). |

| **save position (R + d-up)** | Remembers where Link is, which way he faces and the map (gz's save position). The first save in an area only finds Link's position in memory ("Locating Link's start position, please try again"; he's lifted a little while it's tested, as with levitate, and it's quicker while he's moving): press it again once it says it's found him. |
| **load position (R + d-down)** | Puts Link back there, facing the same way. Only in the same map. With nothing saved: "No position saved yet". |

Under **return**: Link's **position** (x, y, z), his **current age** and the **map**.

### 2.2 Warps

| Item | What it does |
|---|---|
| **places** | Every map, sorted into **overworld (child)**, **overworld (adult)**, **dungeons**, **boss & battle rooms** and **other**. Pick a map, then a spawn point; you go there straight away with the game's own fade. |
| **cancel warp** | Cancels a warp that's waiting (see below). |
| **arrival cutscenes** | Arrive with the spawn point's own arrival scene instead of the standard one. |

If the game ever refuses the instant warp, the warp waits for you instead: walk through any exit or loading zone
and you'll come out at the chosen place. Under **return**: the **current** map and the **warp** state.

### 2.3 Scene

| Item | What it does |
|---|---|
| **reload map** | Reloads the current map (at the age set in **link**). |
| **collision** | See below. |
| **enemy health** | A bar with **life/max** over each living enemy within 30 m of Link (red under a quarter); it goes when the enemy reaches 0. Enemies that haven't appeared yet (Wolfos still in the ground, Stalfos before they drop) have none. |

#### 2.3.1 Collision

**air walls**: the map's invisible walls, drawn as outlines over the game.

| Item | What it does |
|---|---|
| **show** | Draw them. **Cyan**: walls that stop Link. **Red**: a wall Link is inside. **Violet**: other kinds. |
| **all kinds** | Also draw the walls meant for enemies, NPCs and objects. |

The two lines under them count the walls in this map and those near you, and say whether Link is inside one
(how deep, and which way the game will push him out). Outlines are drawn on top of everything, through scenery.

### 2.4 Cheats

**infinite**: energy, rupees, magic, deku sticks, deku nuts, deku seeds, bombs, arrows, bombchus, magic beans:
each kept full while ticked.

**player**: **2x movement speed** makes Link move twice as far each frame. **walk through walls**: hold the stick
against a wall, rock or air wall and Link walks on through it at the height he was at, until you let go of the stick
or turn it. While floating, levitate's own **through walls** setting applies instead. **lock JS height**
keeps the JS Height watch's value at what it was when you ticked it (3.40282347e+38 after a fresh load), writing it
back every frame, across area changes too. Ticked at start-up, it keeps the first value it can read.

### 2.5 Inventory

| Menu | What's in it |
|---|---|
| **items** | **child items** (deku sticks, boomerang, fairy slingshot, magic beans, child trade item), **adult items** (fairy bow, megaton hammer, fire / ice / light arrow, the **hookshot** switch: none / hookshot / longshot, adult trade item), **universal** (bombs, bombchus, deku nuts, Din's fire, Farore's wind, Nayru's love, ocarina, bottles A to E). |
| **ammo** | How much of each kind of ammo you carry, up to your bag's size. **L** fills it. |
| **equipment items** | **swords** (kokiri, master, and the **big sword** switch: none / giant's knife / broken giant's knife / biggoron's sword), **shields** (kokiri, hylian, mirror), **tunics** (goron, zora), **boots** (iron, hover), **other** (lenses of truth, echo gem, **mask slot**: one mask at a time). |
| **collection** | Fairy ocarina, ocarina of time, gerudo card, double defense; **strength** (goron's bracelet, **gauntlets**: none / silver / gold); **other** (**scale**, **skulltulas**, **ammo capacity**: the size of each bag and the wallet, **magic**: your magic and the meter). |
| **quest items** | **spiritual stones** and **medallions**. |
| **songs** | **normal songs** and **warp songs**. |
| **dungeon items** | Per dungeon: map, compass, boss key, and **small keys** where it has them. |

Everything goes through the game's own code where it needs it: items appear on the pause screen, the HUD
redraws, taking away an item you're wearing takes it off Link, and an item on a button comes off the button.

### 2.6 Equips

What Link is wearing and holding right now (read only).

### 2.7 Story

| Item | What it does |
|---|---|
| **all tutorials seen** | Marks every tutorial message as seen, so they stop popping up. |
| *each quest* | The quest's current stage; D-pad left / right (or **L**) changes it. It takes effect after a loading zone. |

### 2.8 Watches

gz's watch list: values shown on the screen in a box while you play.

| Item | What it does |
|---|---|
| **show on screen** | Show the watches box (also **R + D-pad right** in the game). |
| **watches box > position** | Top-left, top-right, bottom-left, bottom-right, top-centre, bottom-centre, or **custom**. |
| **watches box > move** | Move the box anywhere: the D-pad glides it (hold to speed up), **L** leaves it there. It's shown while you move it. |

**watches**: your list, five at a time (the **↑ ↓** buttons above it scroll when there are more). Each watch is a
row of buttons:

| Button | Does |
|---|---|
| **✕** | Remove the watch. |
| **☑** | Show or hide it in the box (it stays in the list). |
| **🔧** | Open the value in **debug > memory** (for watches that read one place in memory). |
| *the name* | Rename it, with gz's on-screen keyboard. An empty name brings back its own name. |
| **< type >** | How the value is shown (next type each press). |
| **↑ ↓** | Move the watch up or down the list. |

Under the list:

- **📁 (known watches)**: the built-in watches: x, y, z, facing, move angle, speed, y speed, camera angle /
  pitch / distance, input, stick, lag, timer, frame, map, air wall, JS Height. Each can be in the list once.
  **JS Height** is the height your last jump set (takeoff + 0.7; FLT_MAX until you jump after a load), which
  the jumpslash launch is limited to. It's always in the list (hide it to keep it off screen).
- **+ (custom watch)**: a watch of any memory address. Type the address (D-pad left / right: digit, up / down:
  change it), **L** to add. It starts from your last memory watch, as in gz. One watch per address.

**Display types.** Positions and speeds: **f32** (decimal), **s32** (whole number), **x32** (the float's bits).
Angles: **deg**, **hex** (the game's 16-bit angle, 0x4000 = 90°), **s16**, **rad**. Counters: **dec**, **hex**.
Timer: **m:s**, **sec**. Stick: **%**, **raw**. Memory watches: **u8 s8 x8 u16 s16 x16 u32 s32 x32 f32 x64**.

**timer**: **start/stop timer**, **reset timer**, **reset lag counter**, **reset JS Height** (back to FLT_MAX, as
after a load) (also shortcuts, [1.3](#13-shortcuts-in-the-game)).

**The keyboard** (renaming): the text line at the top (D-pad left / right moves the cursor, **L** finishes), then
rows of keys (D-pad picks a key, **L** types it) and a row of **shift**, **space**, **back**, **del** and **ok**.
**return** cancels. Up to 12 characters.

Memory watches aren't saved (addresses change every time the game starts); everything else is.

### 2.9 Debug

#### 2.9.1 Heap

How much memory the game has and uses (refreshed every second), and **view the heap in memory**.

#### 2.9.2 Memory

gz's memory viewer.

| Item | What it does |
|---|---|
| **address** | **L** to type an address; D-pad left / right moves a page. |
| **view** | byte, halfword, word, float, pointer. |
| **go to** | Jump to: main, sloperinaGZ, link actor, link status, camera, game data, save strings. |
| **↑ ↓** | Scroll a row (hold **L** to keep scrolling). |
| *rows* | 8 bytes each. **L** edits a row: D-pad left / right moves along it, up / down changes the digit (written at once). On the row's address, **L** adds a watch there; in pointer view, **L** on a value goes where it points. **R + D-pad left** stops editing. |

Unreadable memory shows as `??`; read-only memory can't be changed. **Editing memory can crash the game.**

#### 2.9.3 Pause research

The game's own pause state, for development: values you can watch, change and log.

### 2.10 Settings

**menu settings**: **language** ([6](#6-languages)), **size** (auto, small 2x, medium 2.5x, large 3x: the menu, watches
and log) and **background opacity** (0 to 100%, 60% at first; below 100% the game shows through, slightly behind).

**other settings**: the **input display**, **levitate** and **frame advance** menus below.

#### 2.10.1 Input display

gz's input display, at the bottom left (hidden while the menu is open).

| Item | What it does |
|---|---|
| **show** | Turn it on. |
| **background** | Put it on a background, like the watches. |
| **left stick**, **right stick**, **buttons** | What it shows. |
| **style** | **row**: gz's line of stick values and buttons. **controller**: a Switch 2 Pro Controller with the buttons lighting up, rings for the sticks and their values underneath. |
| **size** | small, medium, large: half, three quarters or all of the menu's size. |

#### 2.10.2 Levitate

| Item | What it does |
|---|---|
| **enabled** | **L3** starts floating, **L3** again drops Link (no fall damage); hold **R3** to stay at your height; the left stick moves him. The game doesn't see L3 while this is on. |
| **through walls** | Float through walls instead of being stopped by them. |
| **rise speed**, **move speed** | How fast he rises and moves, in units per second. |

The status line under **return** shows what levitate is doing.

#### 2.10.3 Frame advance

| Item | What it does |
|---|---|
| **enabled** | Turns frame advance on ([4](#4-about-frame-advance)). |
| **pause icon** | gz's pause icon at the top left while paused. |
| **button** | **L** or **R**. The game doesn't see that button while frame advance is on. |

### 2.11 Log

The last 128 messages sloperinaGZ wrote, newest first, 12 at a time (**↑ ↓** scroll).

| Item | What it does |
|---|---|
| **log history** `< all >` | Which messages: **all**, only **problems** (anything that couldn't be done), or one kind (warp, settings, notice... whatever kinds there are). |

Everything is also written to **`sloperinaGZ/log.txt`** on the SD card (on an emulator, its virtual SD card;
Eden / Lichii: `%APPDATA%\eden\sdmc\sloperinaGZ\log.txt`), with the time since the game started, about once a
second. It starts fresh each time the game starts. Send it along when you report a problem.

### 2.12 About

The name, version, build date and description, and the website.

## 3 On the screen

| What | Where | Notes |
|---|---|---|
| Menu | Top left, under the D-pad icon | Always on top. |
| Watches | The corner you choose, or anywhere | [2.8](#28-watches) |
| Log | Bottom right | What you just did; stays a second, then fades. gz's grey text with a shadow. |
| Input display | Bottom left | [2.10.1](#2101-input-display); moves above the watches if they're there too. |
| Pause icon | Top left | Two bars: paused by frame advance. |
| Air walls | Over the game | [2.3.1](#231-collision) |

## 4 About frame advance

With **settings > frame advance > enabled** on and the menu closed:

- **Tap the button** (L or R): the game pauses (one more frame plays first, so the pause icon is on screen and
  you can see it hasn't crashed). Tap again: one frame. **Hold** it: after a moment, frames play
  one after another (about 15 a second) until you let go.
- **Button + L3**: the game runs again. It always works, even with the menu open (the menu closes): nothing is
  drawn while paused, so a menu opened then can't be seen. Press L3 first, then the button, so the menu doesn't
  take the button press.
- A tap counts when you let go. You can hold or press anything else meanwhile (move the stick, hold A or Z...),
  as in gz: the frame you let through gets it. Only with **R** as the button, R + D-pad and R + right stick down
  stay shortcuts and don't advance.
- **Buffered input**: everything you press while paused, and the last direction you push each stick, is given to
  the next frame, even if you've let go by then.
- While paused the picture doesn't change (the game draws nothing), so the watches and input display update with
  each frame you let through. The menu can still be opened and used while paused; its button works the menu then.

## 5 Settings file

Settings are saved on the SD card in `sloperinaGZ/settings.ini` (on an emulator, its virtual SD card) a couple of
seconds after you close the menu. It's plain `key=value` text: cheats, watches (which, in what order, their types,
names and whether shown), the watches box, menu size, background, language, input display, levitate and frame
advance. Delete it to go back to the defaults. Your game saves are never touched.

## 6 Languages

**settings > language** lists English and every translation on the SD card (`sloperinaGZ/lang/<code>.json`).
To make one, see [INSTALLATION.md](../INSTALLATION.md#languages). The font only has plain letters, so accents are
dropped (é shows as e) and other alphabets can't be shown yet.

## 7 Known limits

- Game versions **1.0.0** (base) and **1.1.1** (update v131072) are supported.
- Collision shows the air walls; the full collision mesh isn't drawn yet.
- Memory watches aren't kept between runs.
- Frame advance freezes the picture while paused ([4](#4-about-frame-advance)).
- Some changes only show after the game redraws them (pause screen, loading zone); story stages need a loading
  zone.
