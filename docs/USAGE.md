# Using sloperinaGZ

A quick guide to get you practising. For every option in every menu, see the [manual](MANUAL.md).

## Contents

- [Getting started](#getting-started)
- [Controls](#controls)
- [Shortcuts in the game](#shortcuts-in-the-game)
- [How do I...?](#how-do-i)
- [Things on the screen](#things-on-the-screen)
- [Your settings](#your-settings)
- [Something's not right](#somethings-not-right)

## Getting started

1. **Install it.** Follow [INSTALLATION.md](../INSTALLATION.md) (an emulator's mod folder, Atmosphère, or a patched copy).
   sloperinaGZ works with **The Legend of Zelda: Ocarina of Time (Switch 2), versions 1.0.0 and 1.1.1**.
2. **Start the game.** For a few seconds the bottom-right corner shows sloperinaGZ's name, version and
   build date, the website, and how to open the menu. That means it's running.
3. **Open the menu:** hold **R** and push the **right stick down**. Do the same again to close it.

While the menu is open, the game doesn't see your controller, so you can't accidentally walk off a ledge while
picking an option.

## Controls

The menu works like gz's: the D-pad moves, **L** selects.

| Button | In the menu |
|---|---|
| **R + right stick down** | Open or close the menu |
| **D-pad up / down** | Move up and down the list |
| **D-pad left / right** | Change a value (switches, numbers), or pick a button on a row of buttons |
| **L** | Select: open a menu, tick a box, press a button |
| **L held + D-pad left / right** | Change a number by 10 |
| **R + D-pad left** | Go back one menu (at the top: close the menu) |
| **return** (the last item) | Go back one menu |

The line at the bottom of the menu always tells you what the buttons do for the item you're on.

**What the items look like**

| Looks like | It is | Use |
|---|---|---|
| `items >` | A menu | **L** opens it |
| `☑ name` | A checkbox | **L** (or D-pad left / right) ticks and unticks it |
| `name  < value >` | A switch | **D-pad left / right** (or **L**) changes it |
| `name  < 3 > / 9` | A number | **D-pad left / right**; hold **L** to change by 10; tap **L** to fill (where it says so) |
| grey text | A heading or information | Can't be selected |
| `✕ ☑ 🔧 label ...` | A row of buttons | **D-pad left / right** picks a button (it turns blue), **L** presses it |

## Shortcuts in the game

These work with the menu closed.

| Shortcut | What it does |
|---|---|
| **R + right stick down** | Open the menu |
| **R + D-pad right** | Show or hide the watches |
| **R + A + D-pad left** | Start or stop the timer |
| **R + B + D-pad left** | Reset the timer |
| **R + B + D-pad right** | Reset the lag counter |
| **L3** | Levitate: start floating / drop (when it's turned on in settings) |
| **X / B** (held, while floating) | Levitate up / down; release to hover |
| **R3** (held, while floating) | Hold height even if X/B is held |
| **L** (or **R**) | Frame advance: pause, then one frame per tap, frames continuously while held (on at first; **settings > frame advance**) |
| **L + L3** (or **R + L3**) | Frame advance: let the game run again |

Everything you do shows for a moment in the bottom-right corner (gz's log), for example `infinite magic on` or
`warp to kokiri forest`.

## How do I...?

**...go somewhere?**
**warps > places**, pick the area (overworld child or adult, dungeons, boss rooms, other), the map, then where in
the map. You arrive straight away, with the game's own fade.

**...be adult (or child)?**
**link > age** to **adult**, then **link > reload map**. The map reloads as the adult version. (Sword and shield
swap with the ones you had as the other age.)

**...get an item?**
**inventory > items** (child, adult or universal), **equipment items** (swords, shields, tunics, boots...) or
**collection** (ocarinas, strength, scale, wallets and bags...). Tick it. Switches like **hookshot**
(none / hookshot / longshot) and **big sword** give you one at a time.

**...refill health, magic or rupees?**
**link > health** (or **rupees**), then tap **L**. Magic is in **inventory > collection > magic**.

**...never run out?**
**cheats**: infinite energy, rupees, magic, and each kind of ammo.

**...see my position, speed or angle?**
**watches**: open the folder (📁) and pick from the **known watches**. They show on the screen in a box. Tick or
untick the checkbox on a watch to show or hide just that one.

**...watch a value in memory?**
**watches**, press the **+**, type the address (D-pad left / right to pick a digit, up / down to change it), **L**
to add it. Or open **debug > memory**, find the value, edit the row (**L**) and press **L** on its address.

**...move the watches box?**
**watches > watches box > move watches position**, then the D-pad (hold it to go faster). **L** when it's in place.

**...see the invisible walls?**
**scene > collision > show**. Cyan walls stop Link; red ones are walls he's inside.

**...float around?**
Turn on **settings > levitate > enabled**, close the menu, press **L3**. Push the left stick to move; hold **X** to rise or **B** to descend. Release both to hover.
With **through walls** enabled, Link can pass through walls and ceilings;
held B commands descent through floors. Exit geometry before toggling floating off.
**vertical speed** controls both directions; an old saved value of 0 uses 4 units/s for X/B.
**L3** stops floating, restores collision and starts the fall with zero vertical speed. Normal
gravity resumes, with fall-distance protection through the first landing. Void triggers still apply.

**...look at a trick frame by frame?**
Turn on frame advance with **settings > frame advance > enabled** (it's off to begin with). In the game, tap **L** to pause, tap **L** for one frame or hold it
to play slowly, and press **L + L3** to carry on. Buttons and stick directions you press while paused are given to
the next frame.

**...see my inputs on screen?**
**settings > input display > show**. Choose **style: controller** for a picture of a Pro Controller.

**...make the menu smaller?**
**settings > size**: small, medium or large.

## Things on the screen

| Where | What |
|---|---|
| Top left, under the D-pad icon | The menu |
| Any corner (you choose) | The watches box |
| Bottom right | The log: what you just did, fading out after a second |
| Bottom left, or anywhere (you choose) | The input display (if it's on) |
| Top left corner | The pause icon, while frame advance has the game paused |
| Over the game | Air walls (if they're on) |

## Your settings

Everything you change in **settings**, **cheats** and **watches** (and the watches box position) is saved by
itself a couple of seconds after you close the menu, and comes back next time you start the game. It's kept on
the SD card in `sloperinaGZ/settings.ini`, plain text you can edit. Your game saves are never touched.

## Something's not right

- **Nothing happens when I press R + right stick down.** Check the sloperinaGZ splash showed at start-up. If not,
  sloperinaGZ needs the module and the IPS for the active game build. See [INSTALLATION.md](../INSTALLATION.md).
- **The game doesn't react to a button.** Frame advance takes **L** (or **R**) away from the game while it's on
  (except in the game's pause screen, unless **settings > frame advance > in pause screen** is on),
  levitate takes **L3**, plus **X/B/R3** while floating. Turn them off in **settings** if you need those buttons.
- **The screen froze.** Frame advance may have paused the game: press **L + L3** (or **R + L3**).
- **Something I ticked didn't change in the game.** Some things only show after the game redraws them: open and
  close the pause screen, or go through a loading zone. Story stages apply after a loading zone.
- **"settings won't be kept".** The game couldn't open the SD card; everything works, it just won't remember your
  settings.

Still stuck? The **log** menu (on the main menu) lists what sloperinaGZ did and what went wrong (set **type** to
**problems**), and everything is also saved to `sloperinaGZ/log.txt` on the SD card: send that file with any
problem report. The [manual](MANUAL.md) explains every option.
