# macdock

A macOS-style dock for X11 Linux desktops, written in C with GTK 3, Cairo and
libwnck (tested on LXQt + Openbox + compton).

- Translucent rounded "glass" bar with a soft shadow
- Smooth icon magnification under the pointer
- App-name tooltip above the hovered icon
- Bounce animation when launching an app
- Dot under running apps; separator between pinned and other running apps
- Click to launch, raise, cycle windows, or minimize; middle-click for a new instance
- Right-click menu: window list, New Window, Keep in Dock, Quit
- Reserves screen space so maximized windows stay above the dock

## Build

Dependencies:

- Arch: `sudo pacman -S base-devel cmake gtk3 libwnck3`
- Debian/Ubuntu: `sudo apt install build-essential cmake pkg-config libgtk-3-dev libwnck-3-dev`

```sh
cmake -S . -B build
cmake --build build
./build/macdock
```

Install (to `/usr/local` by default):

```sh
sudo cmake --install build
cp data/macdock.desktop ~/.config/autostart/   # start at login
```

You need a compositor (picom, compton, or the one built into KWin/xfwm4) for
transparency.

For the macOS layout, move the LXQt panel to the top of the screen. Otherwise
the dock sits on top of it.

## Configuration

The first run writes `~/.config/macdock/config.ini`. Edit it and restart the dock.

| key | default | meaning |
|---|---|---|
| `icon_size` | 48 | icon size at rest (px) |
| `max_scale` | 1.8 | zoom factor of the icon under the pointer (1 = off) |
| `magnify_range` | 3 | number of neighbouring icons affected by the zoom |
| `spacing`, `padding`, `margin` | 6, 8, 6 | gap between icons, bar padding, gap to screen edge |
| `corner_radius` | 16 | bar corner radius |
| `theme` | `dark` | `dark` or `light` |
| `monitor` | -1 | monitor index, -1 = primary |
| `reserve_space` | true | stop maximized windows from covering the dock |
| `show_labels` | true | name tooltip on hover |
| `pinned` | auto | `;`-separated `.desktop` ids, in order |

Use right-click > Keep in Dock to pin or unpin apps; this updates `pinned`.

## Design

```
CMakeLists.txt  build (pkg-config: gtk+-3.0, gio-unix-2.0, libwnck-3.0, x11)
src/main.c      GtkApplication entry point (single instance), SIGINT/SIGTERM handling
src/config.c    GKeyFile-based config
src/apps.c      model: DockItem + AppTracker (pinned apps ⟷ libwnck windows)
src/dock.c      view/controller: the DOCK window, Cairo drawing, animation, input, struts
```

**Window.** One undecorated GTK3 window with an RGBA visual and
`_NET_WM_WINDOW_TYPE_DOCK`. It is sticky, kept above, and does not take focus.
It covers the full width of the monitor, and is tall enough for a magnified,
bouncing icon plus its tooltip. Most of it is transparent. An X input shape
makes only the visible bar (and the zoomed icons) take clicks. The pointer
passes through the rest to the windows underneath. Screen space is reserved by
setting `_NET_WM_STRUT_PARTIAL` with Xlib, since GTK3 has no API for it.

**Rendering.** A single `GtkDrawingArea` paints everything with Cairo: the
gradient bar, the highlight, the shadow, icons, dots, the separator and the
tooltip (Pango). Icons are loaded once at rest size, which keeps them
pixel-sharp, and once at full zoom size, which is scaled down while zooming.

**Magnification.** Each frame, every icon gets
`scale = 1 + (max_scale-1) · ½(1+cos(π·d/range)) · zoom`.
`d` is the pointer's distance to the icon's centre on the *unmagnified* layout,
measured in icon widths. Using the unmagnified layout avoids feedback
jitter. The magnified row is then re-centred, so the bar grows to both sides.
`zoom` eases toward 0 or 1 with an exponential filter, driven by a GTK tick
callback that only runs while something is animating.

**Window tracking.** libwnck reports when windows open or close and when the
active window changes. Each window is matched to a `.desktop` file through its
`WM_CLASS`, using these tables in order: `StartupWMClass`, then the desktop id
(including the last part of reverse-DNS ids), then the executable name. A
window with no match becomes its own item, using the window's icon.

**Memory ownership.** `AppTracker` owns the `DockItem`s. Before freeing an
item it calls `item_removed`, so the dock can drop any pointer it holds
(hovered, pressed, or the item whose menu is open).

**Click behaviour** (`app_tracker_activate`). If the app is not running, launch
it. If it is running but not focused, raise its topmost window, or restore all
its windows if they are all minimized. If it is focused, cycle through its
windows, or minimize it when it has only one.

## Debug build

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan && ASAN_OPTIONS=detect_leaks=0 ./build-asan/macdock
```

## Ideas for next steps

- Drag and drop to reorder icons, and to pin by dropping a `.desktop` file
- Auto-hide, or intelligent hide when a window overlaps the dock
- Left/right dock positions
- Window previews on hover
- Badges and progress bars (Unity LauncherEntry D-Bus API)
- Wayland support through gtk-layer-shell (wlroots compositors)
