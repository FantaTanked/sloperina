# Installing sloperinaGZ

Install **`subsdk9`** (the menu) and both **`.ips`** files. The loader selects the IPS for the
running game build automatically.

## Before you start

- **The game:** The Legend of Zelda: Ocarina of Time (Switch 2), **1.0.0** (base) or **1.1.1** (update v131072). Other
  versions do not load the practice menu.
- **Where it runs:** an emulator from the yuzu family (Lichii, Fritchii, Eden, yuzu, suyu, sudachi, citron, torzu),
  Ryujinx, or a console running Atmosphère.
- **Download** the latest release zip and unzip it anywhere.

## The easy way: the installer

### Windows

1. Double-click **`install.bat`** in the unzipped folder.
2. It lists the emulators it found, plus *Nintendo Switch SD card* and *Another emulator folder*. Type the number
   of the one you want and press Enter. (For a Switch, put its SD card in your PC and give the drive, e.g. `E:\`.)
3. Start the game. For a few seconds the bottom-right corner shows sloperinaGZ's name and version and how to open
   the menu.

If Windows asks whether to run it, choose **More info > Run anyway** (it's a plain script; you can read it in
Notepad).

### Linux, macOS, Steam Deck

```sh
./install.sh
```

Same questions, same result. (If it says *permission denied*: `sh install.sh`.)

### For scripts and power users

| Windows | Linux / macOS | Does |
|---|---|---|
| `install.bat -Target lichii` | `./install.sh lichii` | Install for that emulator without asking (`lichii fritchii eden yuzu suyu sudachi citron torzu ryujinx`) |
| `install.bat -Target switch -Path E:\` | `./install.sh switch /media/you/SWITCH` | Install to a Switch SD card |
| `install.bat -Target folder -Path D:\emu\user` | `./install.sh folder ~/emu/user` | A portable or unusual emulator folder (the one with `load` and `sdmc`) |
| add `-Uninstall` | add `--uninstall` first | Remove sloperinaGZ (your settings stay) |
| add `-Yes` | add `--yes` first | Don't ask before replacing files |

## Installing by hand

The zip's `files` folder has everything. Copy the module and both patches (and, if you like, the translation template) here:

### Lichii, Fritchii, Eden, yuzu, suyu, sudachi, citron, torzu

Open the emulator's folder (right-click the game > *Open mod data location*, or `%APPDATA%\lichii` (Lichii build 150 and later), `%APPDATA%\fritchii` (Fritchii v5.8 and later), `%APPDATA%\eden` (Eden, Lichii up to build 130, Fritchii up to v5.7), `%APPDATA%\yuzu`
... on Windows, `~/.local/share/<name>` on Linux) and make:

```
load/0400486024DEC000/sloperinaGZ/exefs/subsdk9
load/0400486024DEC000/sloperinaGZ/exefs/33FA6C2C23848282A77E8B88A0A93F4887C55920.ips
load/0400486024DEC000/sloperinaGZ/exefs/E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips
sdmc/sloperinaGZ/lang/template.json          (optional, for translators)
```

### Ryujinx

In Ryujinx's folder (`%APPDATA%\Ryujinx` on Windows, `~/.config/Ryujinx` on Linux):

```
mods/contents/0400486024dec000/sloperinaGZ/exefs/subsdk9
mods/contents/0400486024dec000/sloperinaGZ/exefs/33FA6C2C23848282A77E8B88A0A93F4887C55920.ips
mods/contents/0400486024dec000/sloperinaGZ/exefs/E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips
```

### Console with Atmosphère

Copy the zip's `files/atmosphere` folder to the root of the SD card, so it has:

```
atmosphere/contents/0400486024DEC000/exefs/subsdk9
atmosphere/exefs_patches/sloperinaGZ/33FA6C2C23848282A77E8B88A0A93F4887C55920.ips
atmosphere/exefs_patches/sloperinaGZ/E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips
```

This is Atmosphère's standard layout (exefs files through LayeredFS, IPS patches by build id). sloperinaGZ needs
nothing else. On a console, the game needs SD card access to save sloperinaGZ's settings; without it, everything
works but settings aren't kept.

### Patching the game files instead

`files/exefs` has the files for a tool that rebuilds the game's exefs. Add `subsdk9` and apply
only the IPS32 whose filename matches the build ID of `main` (offsets include the 0x100-byte NSO header, the usual convention).

## Updating

Install the new version the same way; it replaces the old files. Your settings are kept.

## Removing it

Run the installer with **-Uninstall** / **--uninstall**, or delete the `sloperinaGZ` folders listed above (and, on
a console, `atmosphere/contents/0400486024DEC000/exefs/subsdk9` if no other mod uses it). Your settings are in
`sloperinaGZ/settings.ini` on the (emulator's) SD card; delete that folder too to remove them.

## Coming from ootgz

sloperinaGZ used to be called ootgz. The installer removes an old `ootgz` install (two copies would clash). Your
old settings in `sd:/ootgz` are read the first time and then saved in `sd:/sloperinaGZ`.

## Languages

The menu is in English. To translate it, copy `files/lang/template.json` to the SD card (the emulator's `sdmc`
or `sdcard` folder) as `sloperinaGZ/lang/<code>.json`, where `<code>` is one of
`es fr de it pt nl pl sv da no fi cs hu tr ro ca id custom`. In the file:

- set `"language"` to the language's own name (it's shown in **settings > language**);
- fill in the translations; anything left `""` stays English;
- keep every `%s`, `%d`, `%.2f` and so on of a line, in the same order (a translation whose placeholders don't
  match is ignored, so a mistake can't crash the game).

The font has plain letters only: accents are dropped (é shows as e) and other alphabets can't be shown yet.
Language files are looked for when the game starts.

## It didn't work

- **No sloperinaGZ splash at start-up.** Check the folder names and that the module and both patches are there.
  The game must be 1.0.0 or 1.1.1. On an emulator, check the mod is ticked in the game's *Add-ons* / *Mods* list.
- **The game crashes at start-up.** Remove other mods that also add a `subsdk9` for this game; only one can.
- More help: [the usage guide](docs/USAGE.md).
