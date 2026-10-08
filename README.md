# macdock

A macOS-style dock for X11 Linux desktops, written in C with GTK 3, Cairo and
libwnck (tested on LXQt + Openbox + compton). An experimental Windows build
is included (see [Windows](#windows)).

- Translucent rounded "glass" bar with a soft shadow
- Smooth icon magnification under the pointer
- App-name tooltip above the hovered icon
- Bounce animation when launching an app
- Dot under running apps; separator between pinned and other running apps
- Click to launch, raise, cycle windows, or minimize; middle-click for a new instance
- Right-click menu: window list, New Window, Keep in Dock, Quit
- Drag and drop to reorder icons; drop a running app among the pinned ones to pin it
- Reserves screen space so maximized windows stay clear of the dock
- Attach to any screen edge: bottom, top, left or right
- Optional panel mode: stretch the bar along the whole screen edge
- Icons centred, or aligned at the start or end of the edge
- Optional auto-hide: the dock slides away and comes back when the pointer touches the screen edge
- One "Dock size" setting scales the whole dock; it shrinks automatically when the icons would not fit the screen edge

## Build

Dependencies:

- Arch: `sudo pacman -S base-devel cmake gtk3 libwnck3 libxext`
- Debian/Ubuntu: `sudo apt install build-essential cmake pkg-config libgtk-3-dev libwnck-3-dev libxext-dev`

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

A compositor (picom, compton, or the one built into KWin/xfwm4) gives the
translucent glass look. Without one, the dock still works: it is drawn opaque
and cut to its exact shape, so windows below stay fully visible. When a
compositor starts or stops, the dock switches between the two modes by itself.

For the macOS layout, move the LXQt panel to the top of the screen. Otherwise
the dock sits on top of it.

## Windows

Experimental: the Windows backend compiles and links, but has not been run
on Windows yet.

Build with [MSYS2](https://www.msys2.org/), in its UCRT64 shell:

```sh
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,pkgconf,gtk3}
cmake -S . -B build -G "MSYS Makefiles"
cmake --build build
./build/macdock.exe
```

To run it outside MSYS2, copy `macdock.exe` next to the GTK DLLs it needs
(`ntldd -R macdock.exe` lists them), and also ship:

- `gdbus.exe`: GLib starts it as a private session bus, which the
  single-instance check and `macdock --preferences` rely on;
- `share/icons` (Adwaita and hicolor themes) and `lib/gdk-pixbuf-2.0`
  (image loaders), from the MSYS2 tree.

Settings are stored in `%LOCALAPPDATA%\macdock\config.ini`. Apps come from
the Start menu's "All apps" list (desktop programs and Store apps); their id
in `pinned` is their AppUserModelID. To start the dock at login, put a
shortcut to `macdock.exe` in `shell:startup`.

The Windows taskbar stays. On the same edge, the dock covers it, so set the
taskbar to auto-hide or move the dock to another edge.

Linux builds can be checked for Windows by cross-compiling with
`mingw-w64-gcc` and the MSYS2 `ucrt64` packages, through a CMake toolchain
file that points pkg-config at them.

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

You can also drag icons in the dock to reorder them. Dropping a running,
unpinned app among the pinned ones pins it at that spot.

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
CMakeLists.txt  build; picks the platform backend (Linux: gtk+-3.0, gio-unix-2.0, libwnck-3.0, x11, xext)

Portable (GTK3, GIO, Cairo only):
src/main.c      GtkApplication entry point (single instance)
src/config.c    GKeyFile-based config
src/apps.c      model: DockItem + AppTracker (pinned apps ⟷ open windows)
src/dock.c      view/controller: the DOCK window, Cairo drawing, animation, input
src/preferences.c  the "Dock Settings" window

Platform interfaces, and their X11 / freedesktop backends:
src/wm.h        window tracking and window actions   → src/wm_wnck.c (libwnck)
src/appinfo.h   installed apps, window → app matching → src/appinfo_desktop.c (.desktop files)
src/platform.h  reserved space, window shape, quit signals → src/platform_x11.c (Xlib, XShape)

Windows backends (src/win32_util.c: UTF-16, process paths, shell icons):
src/wm_win32.c        WinEvent hooks, the taskbar's rules for which windows to show
src/appinfo_win32.c   shell:AppsFolder entries as a GAppInfo, launched through the shell
src/platform_win32.c  AppBar API, window region, Ctrl+C
```

**Platform layer.** The portable files never include X11, libwnck,
`glib-unix.h` or `GDesktopAppInfo`. Everything specific to the desktop goes
through three small interfaces, so porting the dock (to Windows, for example)
means writing three backend files and choosing them in `CMakeLists.txt`.
Two hooks cover the window itself: `platform_setup_dock_window` (on Windows:
no taskbar button, no focus on click, topmost) and
`platform_input_follows_alpha`. On Windows GTK draws the dock as a layered
window, and clicks pass through its fully transparent pixels whatever the
input shape says. So the dock paints its input area (the auto-hide trigger
strip, the margin to the screen edge) at an invisible 1/255 alpha.
Windows are opaque `WmWindow` handles, and apps are plain `GAppInfo`s with a
string id (the id stored in the pinned list).

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

**Drag and drop.** Pressing an icon and moving more than 8px starts a drag.
The dragged item leaves the layout, and an empty `SLOT_GAP` takes its place
at the drop position. The icon is painted at the pointer, held where it was
grabbed. The drop position comes from the current layout: how many icons the
dragged icon's centre has passed, and which side of the separator it is on.
The gap only swaps with a neighbour once the centre passes that neighbour's
centre, so it never flickers between two places. Pinned icons stay in the
pinned group. Running apps can be pinned by dropping them there. On release,
`app_tracker_move_item()` applies the move and saves the pinned order. The
button press gives the dock an implicit pointer grab, so the drag keeps
working outside the input shape.

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

**Without a compositor.** X can't show transparent pixels without a
compositor, so the dock's large window would cover other windows with black.
Instead, each frame is rendered off-screen, and the region of its opaque
pixels becomes the window's bounding shape (`XShapeCombineRectangles`, set
directly because GDK can reset a toplevel's shape). Then the frame is copied
to the window. The bar is drawn opaque, without a shadow. While auto-hidden,
the 2px trigger strip is painted so the window stays reachable. On
`composited-changed` the X window is recreated with the matching visual.

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

**Window tracking.** The `wm.h` backend reports windows that belong in a task
list as they open or close, and when the active window changes. On X11
(`wm_wnck.c`) these are libwnck's normal and dialog windows that don't skip the
task list. Each window is matched to an app by `appinfo_desktop.c` through its
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

- Pin by dropping a `.desktop` file, unpin by dragging an icon off the dock
- Intelligent hide when a window overlaps the dock
- Run and package the Windows build (an installer bundling the GTK runtime)
- Window previews on hover
- Badges and progress bars (Unity LauncherEntry D-Bus API)
- Wayland support through gtk-layer-shell (wlroots compositors)
