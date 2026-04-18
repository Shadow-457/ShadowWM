# shadowwm v5  
## Apple/GNOME-inspired Compositing Window Manager for X11

shadowwm is a compositing X11 window manager written in pure C (about 2,200 lines of code). It gives your Linux desktop a look and feel similar to macOS while still running on X11. It manages all windows on screen – moving them, resizing them, and making them pretty with shadows and rounded corners.

## 20 Big Features

| # | Feature | What it does |
|---|---------|---------------|
| 01 | macOS Traffic-Light Buttons | Close/minimize/maximize buttons on the **left**, title **centered** like a Mac |
| 02 | Rounded Corners | 6px rounded corners using the XShape extension |
| 03 | Soft Drop Shadows | 8‑layer Gaussian‑approximation shadows via XRender |
| 04 | OLED‑Black Palette | Pure black theme (`#0a0a0a` / `#111111`) – easy on the eyes |
| 05 | 4 Virtual Workspaces | Switch with `Super+1` through `Super+4` |
| 06 | Tabbed Windows | Stack multiple windows in one frame like browser tabs |
| 07 | Picture‑in‑Picture | Sticky floating mini‑window in the bottom‑right corner |
| 08 | Per‑Window Opacity | Right‑click title → set 25%, 50%, 75%, or 100% opacity |
| 09 | Quarter Tiling | Snap to 6 zones: left half, right half, 4 corners |
| 10 | Exposé Thumbnail Switcher | `Super+Tab` shows all windows as a grid (like macOS Exposé) |
| 11 | Spring Physics Animations | 250–350 ms critically‑damped spring for open/close |
| 12 | 8‑Direction Resize Zones | Invisible 6‑px grab zones around every window edge |
| 13 | Dim Unfocused Windows | Inactive windows dim to 0.82 opacity automatically |
| 14 | Always‑on‑Top / Bottom | Pin windows above or below everything else |
| 15 | Window Rules | Save/restore position, size, workspace per app class |
| 16 | Fullscreen Mode | `Super+F` hides titlebar, fills screen completely |
| 17 | Fade Animations | Windows fade in on open, fade out on close/minimize |
| 18 | Screen‑Edge Snap | Windows snap to edges when dragged within 16px |
| 19 | Full EWMH Support | Talks to panels, docks, taskbars via standard protocol |
| 20 | JSON Config | Settings saved in `~/.config/shadowwm/config.json` |

## Build Instructions

shadowwm requires the following libraries:

- libX11
- libXft
- libXcomposite
- libXdamage
- libXrender
- libXfixes
- libXext

Install them on Debian/Ubuntu:

```bash
sudo apt install libx11-dev libxft-dev libxcomposite-dev libxdamage-dev \
                 libxrender-dev libxfixes-dev libxext-dev
```

Then compile:

```bash
gcc -O2 -Wall -o shadowwm shadowwm.c \
    -lX11 -lXft -lXcomposite -lXdamage -lXrender -lXfixes -lXext -lm
```

This produces the executable file `shadowwm`.

## Running shadowwm

Add the following line to your `~/.xinitrc`:

```bash
exec ./shadowwm
```

Or start it directly from a TTY:

```bash
startx ./shadowwm
```

## Keyboard Shortcuts

*Super key* = Windows key / Meta key.

### Launching Things

| Keys | Action |
|------|--------|
| `Super + Return` | Open terminal (xterm by default) |
| `Super + D` or `Super + F2` | Open launcher (dmenu_run by default) |
| `Super + Shift + Q` | Quit shadowwm |

### Window Control

| Keys | Action |
|------|--------|
| `Super + Q` | Close focused window |
| `Super + F` | Toggle fullscreen |
| `Super + P` | Toggle Picture‑in‑Picture |
| `Super + Up` | Maximize window |
| `Super + Down` | Un‑maximize / minimize |

### Tiling (6 zones)

| Keys | Zone |
|------|------|
| `Super + Left` | Left half |
| `Super + Right` | Right half |
| `Super + Ctrl + Left` | Top‑left quarter |
| `Super + Ctrl + Right` | Top‑right quarter |
| `Super + Ctrl + Shift + Left` | Bottom‑left quarter |
| `Super + Ctrl + Shift + Right` | Bottom‑right quarter |

### Workspaces (4 desks)

| Keys | Action |
|------|--------|
| `Super + 1` … `Super + 4` | Switch workspace |
| `Super + Shift + 1` … `Super + Shift + 4` | Move window to workspace |

### Tabs

| Keys | Action |
|------|--------|
| `Super + T` | Merge focused window into the window under the cursor |
| `Super + U` | Detach focused window from its tab group |

### Switching

| Keys | Action |
|------|--------|
| `Super + Tab` | Open Exposé thumbnail switcher |
| ``Super + ` `` (backtick) | Cycle through windows |

## Mouse Controls

| Action | Result |
|--------|--------|
| Left‑click drag on titlebar | Move window |
| Double‑click on titlebar | Maximize / restore |
| Right‑click on titlebar | Open context menu (opacity, always‑on‑top, save rule) |
| Click traffic‑light buttons | Close / Minimize / Maximize |
| Drag window edges | Resize (8 directions) |
| Drag window to screen edge | Snap to tile zone (ghost preview shown) |
| `Super + Left‑drag` anywhere in window | Move window |
| `Super + Right‑drag` anywhere in window | Resize window |

## Configuration File

`~/.config/shadowwm/config.json`

Example:

```json
{
  "terminal": "xterm",
  "launcher": "dmenu_run",
  "rules": [
    {"class": "Firefox", "x": 0, "y": 0, "w": 1280, "h": 900, "desk": 0},
    {"class": "Thunderbird", "x": 0, "y": 0, "w": 1000, "h": 700, "desk": 1}
  ]
}
```

- **terminal** – Command to launch terminal (default: `xterm`)
- **launcher** – Command to launch app launcher (default: `dmenu_run`)
- **rules** – Per‑app window placement rules (saved automatically via right‑click menu)

## Architecture (How shadowwm works internally)

```
main()
├── X11 Setup (display, atoms, cursors, keybinds)
├── Compositor Init (XComposite + XDamage)
├── Config Load (JSON parser)
├── Adopt Existing Windows
└── Event Loop
    ├── MapRequest      → frame_window()   (new window appears)
    ├── UnmapNotify     → unframe_window() (window closes)
    ├── KeyPress        → on_key_press()   (keyboard shortcuts)
    ├── ButtonPress     → on_button_press()(mouse clicks)
    ├── MotionNotify    → on_motion_notify()(drag/resize)
    ├── DamageNotify    → comp_repaint()   (compositor redraw)
    └── ClientMessage   → on_client_message() (EWMH messages)
```

### Key Data Structures

- **Client** – One per managed window. Stores geometry, opacity, title, tab group, compositor pixmap, animation spring.
- **TabGroup** – Array of client indices sharing one frame.
- **WinRule** – Saved geometry/workspace rule per `WM_CLASS`.
- **Spring** – Physics state (position, velocity, target) for animations.

### Dependencies

| Library | Purpose |
|---------|---------|
| libX11 | Core X11 display and window management |
| libXft | Font rendering (titles and tab labels) |
| libXcomposite | Off‑screen rendering for compositor |
| libXdamage | Track which windows need repainting |
| libXrender | GPU‑accelerated compositing and shadows |
| libXfixes | Various X extension fixes |
| libXext | XShape extension (rounded corners) |
| libm | Math (spring physics calculations) |

## Limitations

- Single‑monitor only (no RandR multi‑monitor support)
- No built‑in status bar – use an external panel like polybar or lemonbar
- Spring animations are defined but partially stubbed (the physics framework is ready)
- Minimal JSON parser – no arrays of arrays, no escape sequences in strings
- No ICCCM `WM_SIZE_HINTS` enforcement – min/max size hints from apps are ignored

**shadowwm v5** – small in size, big on features.
