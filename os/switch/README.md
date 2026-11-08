# Fallout 2 CE for Nintendo Switch (with Fallout et tu)

Homebrew build of Fallout 2 Community Edition for a modded Switch. One build gives you two launchers:

- `fallout2-ce.nro` starts Fallout 2
- `fallout1-ettu.nro` starts Fallout et tu (Fallout 1 in the Fallout 2 engine)

## SD card layout

```
sdmc:/switch/fallout2/
    fallout2-ce.nro
    fallout1-ettu.nro
    ce.dat
    fallout2_nx.ini
    master.dat          (from your Fallout 2 install)
    critter.dat         (from your Fallout 2 install)
    patch000.dat        (from your Fallout 2 install)
    fallout2.cfg        (from your Fallout 2 install)
    data/               (from your Fallout 2 install)
    sound/              (from your Fallout 2 install)
    Fallout1in2/        (Fallout et tu, see below)
```

`sdmc:/fallout2/` also works as the game folder if you used that before.

## Installing Fallout et tu

Do this on a PC first, then copy the result to the SD card.

1. Download the latest Fallout et tu release (v2.0 or newer, the first version that supports Fallout 2 CE)
2. Extract it into a copy of your Fallout 2 folder, so you get `Fallout 2/Fallout1in2/`
3. Run `UNDAT.EXE` (Windows) or `undat.sh` (Linux/macOS) and point it at Fallout 1's `master.dat`. This extracts Fallout 1's data into `Fallout1in2/data/`
4. Copy the whole `Fallout1in2` folder to `sdmc:/switch/fallout2/Fallout1in2/`

Et tu uses Fallout 2's `master.dat` and `critter.dat` from the folder above it, so Fallout 2 must be installed in `sdmc:/switch/fallout2/` too. Et tu ships its own `ce.dat` inside `Fallout1in2`, so leave that one in place.

The Et tu launcher is picked by its file name. If you rename it, keep "ettu" in the name.

## Controls

| Button | Action |
| --- | --- |
| Left stick | Move cursor |
| R (hold) | Faster cursor |
| ZL | Left click |
| ZR | Right click |
| Right stick | Scroll the map |
| Touch screen | Tap to click, drag to move the cursor |
| A | Start combat |
| B | End turn |
| X | Skilldex |
| Y | Inventory |
| + | Options menu |
| - | Character screen |
| D-pad up | Pip-Boy |
| D-pad down | Center on player |
| D-pad left (hold) | Highlight items |
| Left stick click | Sneak |
| Right stick click | End combat |

## Screen size

Edit `fallout2_nx.ini` to change the screen size. It applies to both games.
