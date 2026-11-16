# Microsoft Store

Simple Crosshair ships to the Store as an **MSIX** package. The Store signs and hosts it, and
Windows installs, updates and uninstalls it cleanly. The same package can also be installed
outside the Store, signed with your own certificate (see [SIGNING.md](SIGNING.md)).

- [store/AppxManifest.xml](store/AppxManifest.xml) is the package manifest (a template).
- [store/store.json](store/store.json) holds the package identity values.
- [store/Assets](store/Assets) has the logos. [tools/make_icon.py](tools/make_icon.py) generates them.
- [pack.ps1](pack.ps1) builds the package.

The packaged app keeps its settings in its own private `%LOCALAPPDATA%` folder, because the
install folder is read-only. Windows removes that folder on uninstall. The portable exe still
uses `SimpleCrosshair.ini` next to the exe.

## Build

```bat
build.bat
powershell -ExecutionPolicy Bypass -File pack.ps1          :: Store upload, unsigned
powershell -ExecutionPolicy Bypass -File pack.ps1 -Direct  :: signed, for testing or direct download
```

The version comes from `FILEVERSION` in [simple-crosshair.rc](simple-crosshair.rc). Every Store
submission needs a higher version than the last one, and the last part must be `0` (e.g. `1.1.0.0`).

- **Store build**: produces `SimpleCrosshair-<version>-store.msix`.
- **Direct build** (`-Direct`): produces `SimpleCrosshair-<version>.msix`, signed with Azure
  Artifact Signing (3 signatures per package). Double-click it to install it on any Windows 10
  1809+ or Windows 11 PC. Remove it in *Settings → Apps* or with
  `Get-AppxPackage KarlLockhart.SimpleCrosshair | Remove-AppxPackage`.

## One-time Partner Center setup

1. **Developer account**: sign up at https://storedeveloper.microsoft.com as an **Individual**.
   It's free; Microsoft checks a government ID and a selfie.
2. **Reserve the name**: [Partner Center](https://partner.microsoft.com/dashboard/apps-and-games/overview)
   → *Apps and games* → *New product* → **MSIX or PWA app** → `Simple Crosshair`.
3. **Copy the identity**: the product → *Product management* → **Product identity**. Copy
   `Package/Identity/Name`, `Package/Identity/Publisher` and
   `Package/Properties/PublisherDisplayName` into [store/store.json](store/store.json).

## Submission

Product → *Start your submission*:

- **Pricing and availability**: Free, all markets (or the ones you want).
- **Properties**: category **Utilities & tools**. Privacy policy: the app collects no personal
  data and makes no network connections, so the Store doesn't require one. Partner Center may
  still ask for a URL; you can link the "What this app does NOT do" section of the README.
- **Age ratings**: fill in the IARC questionnaire (no violence, no user interaction, no data
  sharing, no purchases).
- **Packages**: upload `SimpleCrosshair-<version>-store.msix`.
- **Store listings** (English): use the text below and at least one screenshot (1366×768 or
  larger; PNG). The settings window with the crosshair on screen works well.
- **Submission options → Notes for certification**: use the notes below. If Partner Center asks
  why the app needs `runFullTrust`, say it's a Win32 desktop app packaged as MSIX.

The first certification usually takes a few days.

### Listing text (draft)

**Short description**

> A tiny, click-through crosshair overlay for games. It never touches the game.

**Description**

> Simple Crosshair draws a clean crosshair in the middle of your screen, on top of any game or
> app. It is one small, click-through, always-on-top window. It never touches the game.
>
> • Six styles: cross, cross with dot, T-shape, dot, circle, circle with dot
> • Any color, opacity, size, thickness, gap and outline
> • Pick the monitor and nudge the position
> • Ctrl+Alt+H shows or hides it from anywhere; it lives in the system tray
> • Tiny (about 30 KB), uses almost no CPU, needs no admin rights
>
> What it does NOT do: it doesn't read or write other programs' memory, inject code, hook
> DirectX/OpenGL/Vulkan or input, capture your screen, look at other programs, or use the
> network. No telemetry, no ads, no account.
>
> Works in borderless and windowed games. Windows can't draw any overlay on top of a game in
> exclusive fullscreen; the app tells you when that happens.
>
> Some competitive games and tournaments don't allow third-party crosshairs. Check the rules
> before you use it.

**Features** (one per line)

> Six crosshair styles
> Custom color, opacity, size, thickness, gap and outline
> Multi-monitor support
> Ctrl+Alt+H toggle hotkey
> Click-through; never touches the game
> No network, no telemetry, no admin rights

**Search terms**: crosshair, overlay, aim, reticle, FPS, gaming, crosshair overlay

### Notes for certification

> Simple Crosshair opens its settings window at launch and draws a crosshair at the centre of
> the primary monitor. The crosshair window is click-through and always on top. Closing the
> settings window keeps the app running in the system tray (notification area); click the tray
> icon to reopen it, or right-click → Exit. Ctrl+Alt+H toggles the crosshair. The app needs no
> account, network or test data.
