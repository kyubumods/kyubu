# Cube World Alpha: content pipeline tools

These tools target the Cube World Alpha **v0.1.1** client (Cube.exe built 2 July 2013). They need Python 3 and Pillow (`pip install pillow`).

| File | What it does |
|---|---|
| `cwdb.py` | Reads and writes the obfuscated `data1-4.db` asset stores (list, unpack, pack, verify). |
| `cubvox.py` | Converts `.cub` to and from MagicaVoxel `.vox` without losing anything. It can also render a PNG preview. |
| `cwmod.py` | The pipeline itself: extract, then build a mod folder, then install or restore. |
| `model_slots.json` | Maps each model file to the slot ID the game's loader puts it in. This was pulled from Cube.exe. |
| `catalog.csv` | One row for each of the 2,857 models: slot, size, colour count, and whether the game uses it. |
| `example-mod/` | A sample mod. It reskins the iron sword gold, fixes an untranslated German quest line, and adds one new model. |

## Workflow
```
python cwmod.py extract "<game dir>" work              # work/vanilla/{models,vox,sounds,icons,text}
#   edit or copy files into work/mods/<name>/{models,sounds,icons,text}
python cwmod.py build   "<game dir>" work/mods/<name> work/build/<name>
python cwmod.py install "<game dir>" work/build/<name>  # makes a one-time *.vanilla backup of each DB it replaces
python cwmod.py restore "<game dir>"
```
- When you save a mod file, keep the vanilla asset name (for example `iron-sword1.vox`) so it replaces that asset.
- Start the game with **Cube.exe**, not CubeLauncher.exe. The launcher is the old auto-updater.
- Server.exe loads `data1.db` too, so give the server the same modded file.
