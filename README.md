# Simple Crosshair

A small (~30 KB) crosshair overlay for Windows. It draws a crosshair in the middle of your
screen using one transparent, click-through, always-on-top window, and nothing else. It never
touches the game: it doesn't know or care which program is underneath it.

- One file of C++ using the plain Win32 API, no third-party libraries
- No installer; settings are saved to `SimpleCrosshair.ini` next to the exe
- Runs from the system tray
- Runs as a normal user (`asInvoker` manifest); never asks for admin rights
- Not packed or obfuscated (no UPX), so what you download is what the compiler produced

## What this app does NOT do

Simple Crosshair is built so that it has nothing in common with cheats. It does **not**:

- **Read or write the memory** of any other process
- **Inject DLLs** or any code into other processes
- **Hook DirectX, OpenGL, Vulkan, or input** (no `SetWindowsHookEx`, no API hooks, no present
  hooks)
- **Capture the screen or read pixels.** It has no idea what is on your screen
- **Inspect game windows or processes.** It doesn't look up, open, or enumerate other programs
- **Need admin rights.** It runs with your normal user permissions
- **Use the network.** It makes no connections of any kind: no updates, no telemetry

It uses only two OS integrations beyond drawing its own window and tray icon:

1. **An out-of-context foreground WinEvent** (`SetWinEventHook` with `EVENT_SYSTEM_FOREGROUND`
   and `WINEVENT_OUTOFCONTEXT`). Screen readers use this same mechanism to learn when the
   active window changes. "Out of context" means Windows sends the notification to Simple
   Crosshair's own process; nothing is loaded into any other program. The app uses it only as
   a cue to put its overlay back on top and re-check the display mode.
2. **`SHQueryUserNotificationState`**, the shell API that reports whether a fullscreen
   Direct3D app is running (the same signal Windows uses to hold back notifications). The app
   uses it to tell you when it can't be seen (see below).

### Check your game's rules

Even an overlay that doesn't touch the game can be against the rules. Some competitive games,
tournaments, and leagues ban third-party crosshairs. Check the rules for each game and
competition before you use it.

## Usage

Run `SimpleCrosshair.exe`. The settings window opens and the crosshair shows on your screen.

| Setting   | What it does |
|-----------|--------------|
| Style     | Cross, Cross + dot, T-shape, Dot, Circle, or Circle + dot |
| Screen    | Which monitor to draw on (for multi-monitor setups) |
| Color     | Crosshair color (click **Choose...**) |
| Opacity   | 10–100% |
| Size      | Length of the lines (or circle size) |
| Thickness | Line thickness |
| Gap       | Space between the center and the lines |
| Dot size  | Size of the center dot |
| Outline   | Black outline around the crosshair in pixels, 0 for none |
| X offset / Y offset | Move the crosshair away from the exact center; **Re-center** resets it |

- **Ctrl+Alt+H** shows or hides the crosshair from anywhere. The **Show** checkbox does the same.
- **Closing the settings window hides it to the tray.** The crosshair keeps showing.
- **Click the tray icon** to bring the settings back. **Right-click the tray icon** for the
  menu, then choose **Exit** to quit.
- Running the exe again while it's already running just brings the settings window back.

Settings are saved to `SimpleCrosshair.ini` in the same folder as the exe. Delete it to reset
everything.

## Supported display modes

| Game display mode     | Works? |
|-----------------------|--------|
| Windowed              | Yes |
| Borderless / windowed fullscreen | Yes |
| Exclusive fullscreen  | No: Windows doesn't draw other windows over it |

Exclusive fullscreen is a Windows limit, not something the app can (or should) get around:
the only ways to draw over it would be hooking the game's rendering, which is exactly what
this app refuses to do. When it detects exclusive fullscreen, the status line in the settings
window and the tray tooltip say it's **blocked**, and a tray notification tells you to switch
the game to **Borderless** or **Windowed**.

## Building

You need a MinGW-w64 `g++` and `windres`, from either
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) or [MSYS2](https://www.msys2.org/)
(for example the `mingw-w64-ucrt-x86_64-gcc` package). Put its `bin` folder on your `PATH`,
then run:

```bat
build.bat
```

This produces a single, statically linked `SimpleCrosshair.exe` that needs no runtime DLLs.
The build doesn't pack or compress the exe; please don't run UPX or similar packers on it,
because packed executables are a common antivirus false-positive trigger.

## Releases

Pushing a tag such as `v1.0.0` runs [`.github/workflows/release.yml`](.github/workflows/release.yml),
which builds the exe on GitHub's Windows runner and attaches `SimpleCrosshair.exe` and
`SHA256SUMS.txt` to a GitHub Release. To verify a download in PowerShell:

```powershell
Get-FileHash .\SimpleCrosshair.exe -Algorithm SHA256
```

and compare the hash with the one in `SHA256SUMS.txt`.
