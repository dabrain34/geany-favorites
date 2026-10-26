# Geany Favorites

A [Geany](https://www.geany.org/) plugin to keep a list of favorite files at hand.

- A **Favorites** tab in the sidebar: click or <kbd>Enter</kbd> opens a
  file, drag and drop reorders, <kbd>Delete</kbd> removes, right-click opens a
  context menu. Files that no longer exist are shown as missing.
- A **Tools › Favorites** submenu for quick access.
- An **Add to Favorites** / **Remove from Favorites** entry in the editor's
  right-click menu for the current file.
- Keybindings (unassigned by default, set them in
  *Edit › Preferences › Keybindings › Favorites*):
  add current file, remove current file, focus the Favorites tab.

The list is stored in `~/.config/geany/plugins/favorites/favorites.conf`.

Requires Geany ≥ 1.26 (tested with 2.0).

## Dependencies

You need a C compiler, [Meson](https://mesonbuild.com/), Ninja, pkg-config and
the Geany development files.

Debian / Ubuntu:

```sh
sudo apt install build-essential meson ninja-build pkg-config geany libgtk-3-dev
```

Fedora:

```sh
sudo dnf install gcc meson ninja-build pkgconf geany-devel
```

Arch Linux:

```sh
sudo pacman -S base-devel meson ninja geany
```

Check that the Geany headers are found:

```sh
pkg-config --modversion geany
```

## Build and install

### System-wide

Installs into Geany's own plugin directory (e.g.
`/usr/lib/x86_64-linux-gnu/geany`), as reported by `pkg-config geany`:

```sh
meson setup build
meson compile -C build
sudo meson install -C build
```

### Per user (no root needed)

Geany also loads plugins from `~/.config/geany/plugins`:

```sh
meson setup build -Dplugindir="$HOME/.config/geany/plugins"
meson compile -C build
meson install -C build
```

Any other directory works too, as long as it is set as *Extra plugin path* in
*Edit › Preferences › General › Startup*.

### Enable the plugin

Restart Geany, open *Tools › Plugin Manager* and tick **Favorites**.

## Uninstall

From the build directory used for installing:

```sh
sudo ninja -C build uninstall   # drop sudo for a per-user install
```

## Alternative: Makefile

A plain `Makefile` is also provided for a quick per-user build without Meson:

```sh
make
make install      # copies favorites.so to ~/.config/geany/plugins
```

## License

GPL-2.0-or-later
