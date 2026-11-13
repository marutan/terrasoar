# Installing Terra Soar

Terra Soar runs on **Windows 10 or later** (64-bit) and on **Linux**
(x86-64). Every download includes the base pack, the built-in data (Blue
Marble, shoreline, country outlines, terrain and sea level, about 63 MB),
so the program works without a network from the first start.

Downloads are on the project's
[Releases page](https://github.com/marutan/terrasoar/releases), each with a
`.sha256` file to check it against (`sha256sum -c <file>.sha256` on Linux,
`Get-FileHash <file>` in PowerShell).

## What the computer needs

- A graphics driver with **OpenGL 3.3** (any graphics card or integrated
  graphics from the last ten years). Without it Terra Soar still starts
  but says the globe can't be drawn. Starting it with `--software-gl`
  uses a slower software renderer instead (on remote desktops and virtual
  machines, for example).
- About 200 MB of disk for the program, more for saved web tiles and data
  packs (see [offline use](offline-use.md#space-on-disk)).

## Windows

1. Download `TerraSoar-0.0.1-setup.exe` and run it.
2. Windows may say it protected your computer, because the installer
   isn't signed yet: choose **More info → Run anyway**.
3. Choose where to install it; the installer adds a Start menu entry and,
   if you like, a desktop shortcut.

To remove it, use **Settings → Apps** (or the uninstaller in the install
folder). Your projects, saved tiles and data packs are kept; delete
`%LOCALAPPDATA%\Terra Soar` to remove them too.

Installing a newer version over an older one keeps all of these.

## Linux

Three ways, all with the base pack:

- **AppImage**: download `TerraSoar-0.0.1-x86_64.AppImage`, make it
  executable (`chmod +x TerraSoar-0.0.1-x86_64.AppImage`) and run it. No
  installation; delete the file to remove it.
- **Flatpak**: download `TerraSoar-0.0.1.flatpak` and install it with
  `flatpak install --user TerraSoar-0.0.1.flatpak` (it needs the KDE
  runtime from Flathub, which Flatpak offers to install). Start it from
  the applications menu or with `flatpak run io.github.marutan.TerraSoar`;
  remove it with `flatpak uninstall io.github.marutan.TerraSoar`.
- **From source**: `terrasoar-0.0.1.tar.gz`, built as described in
  [building.md](../building.md); distribution packagers start here.

## Where your files go

| What | Windows | Linux (AppImage, source) |
| --- | --- | --- |
| Data packs | `%LOCALAPPDATA%\Terra Soar\terrasoar\packs` | `~/.local/share/Terra Soar/terrasoar/packs` |
| Saved web tiles, offline regions | `%LOCALAPPDATA%\Terra Soar\terrasoar\cache` | `~/.cache/Terra Soar/terrasoar` |
| Projects | wherever you save them (`*.terrasoar`) | the same |

The Flatpak keeps its files inside its sandbox, under
`~/.var/app/io.github.marutan.TerraSoar/`.
