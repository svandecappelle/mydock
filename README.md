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
- Reserves screen space so maximized windows stay clear of the dock
- Attach to any screen edge: bottom, top, left or right
- Optional panel mode: stretch the bar along the whole screen edge
- Icons centred, or aligned at the start or end of the edge
- Optional auto-hide: the dock slides away and comes back when the pointer touches the screen edge
- One "Dock size" setting scales the whole dock; it shrinks automatically when the icons would not fit the screen edge

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

Open the settings window in any of these ways:

- right-click the dock > **Dock Settings…**
- run `macdock --preferences` (opens the running dock's settings; starts the dock if needed)
- choose **Dock Settings** in your application menu (after `cmake --install`)

Changes apply to the dock immediately and are saved automatically.

| tab | settings |
|---|---|
| Appearance | theme, dock size, corner radius, app-name labels, magnification (on/off, zoom, spread) |
| Position | screen edge (bottom/top/left/right), icon alignment (start/center/end), monitor, extend to screen edges, auto-hide, reserve screen space, icon spacing, bar padding, distance from screen edge |
| Apps | pinned apps: add (searchable list), remove, reorder |

**Reset to Defaults** restores every setting except the pinned apps.

Settings are stored in `~/.config/macdock/config.ini`. You can also edit that
file by hand, then restart the dock:

| key | default | meaning |
|---|---|---|
| `position` | `bottom` | screen edge: `bottom`, `top`, `left` or `right` |
| `alignment` | `center` | `start` (left/top), `center` or `end` (right/bottom) |
| `icon_size` | 48 | dock size: icon size at rest (px) |
| `max_scale` | 1.8 | zoom factor of the icon under the pointer (1 = off) |
| `magnify_range` | 3 | number of neighbouring icons affected by the zoom |
| `spacing`, `padding` | 6, 8 | gap between icons, bar padding (at size 48; scaled with `icon_size`) |
| `margin` | 6 | gap between the bar and the screen edge (px, not scaled) |
| `corner_radius` | 16 | bar corner radius (at size 48; scaled with `icon_size`) |
| `theme` | `dark` | `dark` or `light` |
| `monitor` | -1 | monitor index, -1 = primary |
| `reserve_space` | true | stop maximized windows from covering the dock (ignored while auto-hiding) |
| `expand` | false | stretch the bar along the whole screen edge (icons stay centred) |
| `autohide` | false | hide the dock until the pointer touches the screen edge |
| `show_labels` | true | name tooltip on hover |
| `pinned` | auto | `;`-separated `.desktop` ids, in order |

## Design

```
CMakeLists.txt  build (pkg-config: gtk+-3.0, gio-unix-2.0, libwnck-3.0, x11)
src/main.c      GtkApplication entry point (single instance), SIGINT/SIGTERM handling
src/config.c    GKeyFile-based config
src/apps.c      model: DockItem + AppTracker (pinned apps ⟷ libwnck windows)
src/dock.c      view/controller: the DOCK window, Cairo drawing, animation, input, struts
src/preferences.c  the "Dock Settings" window
```

**Window.** One undecorated GTK3 window with an RGBA visual and
`_NET_WM_WINDOW_TYPE_DOCK`. It is sticky, kept above, and does not take focus.
It covers the full width of the monitor, and is tall enough for a magnified,
bouncing icon plus its tooltip. Most of it is transparent. An X input shape
makes only the visible bar (and the zoomed icons) take clicks. The pointer
passes through the rest to the windows underneath. Screen space is reserved by
setting `_NET_WM_STRUT_PARTIAL` with Xlib, since GTK3 has no API for it.

**Screen edges.** Layout is computed in *dock coordinates*. One axis runs
along the dock. The other measures the distance from the screen edge the dock
is attached to. A single function, `to_window()`, maps these to window pixels
for the chosen edge. Icons and text are never rotated, only placed. Name
labels and right-click menus open on the side away from the edge. The
reserved strip (`_NET_WM_STRUT_PARTIAL`) is set on that edge. On a vertical
dock the window is wider, to make room for labels beside the icons.

**Extend to screen edges.** In expand mode the bar spans the whole dock
length. It overshoots both ends by the corner radius, so the rounded corners
fall outside the window and the ends look square. The icons, magnification
and input handling don't change.

**Alignment.** `row_start()` places the icon row centred, or at a fixed
inset from the start or end of the edge. The same anchor is used for the
unmagnified layout, which measures pointer distance, and for the magnified
one. So with start or end alignment the anchored end stays put, and zooming
grows the row away from it. Without expand mode the bar follows the row.

**Auto-hide.** `hide` animates between 0 (shown) and 1 (hidden), eased like
the zoom. `to_window()` moves everything toward the screen edge by
`hide × (bar + margin + shadow)`, so the dock slides out of its own window.
The input shape follows it, but never gets thinner than a 2px strip along the
edge (`TRIGGER_SIZE`). Touching that strip reveals the dock. Leaving the dock
starts a 500 ms timer that hides it again. The timer is cancelled if the
pointer comes back, and is not started while a menu is open. An auto-hidden
dock reserves no screen space.

**Dock size.** Spacing, padding and corner radius are stored for the 48px
reference size and scaled with `icon_size`, so the dock keeps its proportions.
When the icons would not fit the screen edge, `fit_size()` uses the largest
size that does. The bar's length is linear in the icon size, so that size is
a single division.

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

**Settings window.** `preferences.c` edits the shared `DockConfig` in place.
After each change it calls back into the dock (`apply_config`), which reloads
the theme and icons, then resizes and moves the window. Saving waits 400 ms, so
dragging a slider does not rewrite the file on every step. Changes to the
pinned list go through `app_tracker_set_pinned_ids`, the same path the "Keep
in Dock" menu uses. A second `macdock --preferences` process forwards an
`app.preferences` action to the running instance over D-Bus (GApplication).

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
