# Plaud Desktop for Linux (unofficial)

Runs the official [Plaud](https://www.plaud.ai/) Desktop app on Linux:
meeting detection, recording of microphone and system audio with echo
cancellation, highlights and screenshots, upload for transcription and
summaries, the tray panel, notifications, global shortcuts, `plaud://`
sign-in links, launch at login and in-app updates.

> **Unofficial.** This project is not affiliated with, endorsed by, or
> sponsored by PLAUD LLC. "Plaud" is a trademark of its owner. Use of the
> Plaud service is subject to Plaud's terms.

## How it works

The Plaud Desktop app is an Electron application whose JavaScript is the
same on macOS and Windows; only two native modules and a few system
integrations differ. This project provides:

- Linux native modules for audio capture and meeting detection, built on
  PipeWire (`native/`);
- a patcher that makes small, verified edits to the official app so it
  loads those modules and uses Linux services (`patcher/`,
  [docs/patches.md](docs/patches.md));
- a Linux runtime for updates, tray, autostart, global shortcuts and
  credential storage (`linux-runtime/`);
- build and packaging scripts.

The repository contains no Plaud code. The build downloads the official
release from Plaud's servers, verifies it against the checksum Plaud
publishes, patches it and combines it with stock Electron, all on your
machine. Apps and packages you build contain Plaud's proprietary code:
**do not redistribute them.** Details: [docs/architecture.md](docs/architecture.md).

## Requirements

- x86_64 or aarch64, glibc-based distribution (Arch Linux, Debian 12+,
  Ubuntu 22.04+, Fedora 39+)
- A PipeWire session (PipeWire 0.3.48 or newer, with WirePlumber;
  `pipewire-pulse` is fine, a PulseAudio daemon is not needed)
- `ffmpeg` with libopus (uploads are encoded with the system ffmpeg)
- A StatusNotifierItem tray: KDE Plasma, Waybar (Hyprland, Sway), or GNOME
  with the AppIndicator extension (preinstalled on Ubuntu)
- Recommended: a Secret Service (GNOME Keyring, KWallet or KeePassXC) for
  the sign-in tokens, and `xdg-desktop-portal` with a backend for your
  desktop

Build dependencies:

| Distribution | Packages |
|---|---|
| Arch Linux | `nodejs npm git cmake ninja meson python pkgconf libpipewire opus libogg dbus libpng zlib libx11 libxrandr` |
| Debian, Ubuntu | `nodejs npm git build-essential cmake ninja-build meson python3 pkg-config libpipewire-0.3-dev libopus-dev libogg-dev libdbus-1-dev libpng-dev zlib1g-dev libx11-dev libxrandr-dev` |
| Fedora | `nodejs npm git gcc-c++ cmake ninja-build meson python3 pkgconf pipewire-devel opus-devel libogg-devel dbus-devel libpng-devel zlib-devel libX11-devel libXrandr-devel` |

Node.js 22.12 or newer is required.

## Install

### Per user, from source

```sh
git clone https://github.com/trukhinyuri/plaud-linux-unofficial.git
cd plaud-linux-unofficial
npm ci
npm run build
scripts/install
```

`scripts/install` copies the app to `~/.local/share/plaud-linux/app`, links
`~/.local/bin/plaud-linux`, and registers the desktop entry, icons and the
`plaud://` handler. Run it again after a rebuild to upgrade; the app must
not be running.

### Packages

```sh
npm run package                       # every format whose tools are installed
npm run package -- --targets appimage,pacman
```

Packages are written to `dist/`:

| Format | Tool needed | Installs to |
|---|---|---|
| AppImage | `mksquashfs` | anywhere; integrates itself into the menu on first start |
| deb | `dpkg-deb` | `/opt/plaud-linux` |
| rpm | `rpmbuild` | `/opt/plaud-linux` |
| pacman | `makepkg` | `/opt/plaud-linux` |

Arch Linux users can also build with [`packaging/arch/PKGBUILD`](packaging/arch/PKGBUILD)
(`makepkg -si` in that directory), which downloads and verifies all
sources itself.

The packages are for your own machines. They contain Plaud's code and must
not be published.

## Use

Start **Plaud** from the application menu, or run `plaud-linux`. The app
lives in the tray; click the icon to open the panel. Signing in opens the
browser and returns to the app through a `plaud://` link.

Settings that differ from macOS and Windows:

| Setting | Effect |
|---|---|
| `PLAUD_LINUX_OZONE=auto\|x11\|wayland` | Display backend. `auto` (default) uses XWayland when available, because the app positions its panel, widgets and overlays itself, which Wayland does not allow; `wayland` runs as a native Wayland client |
| `PLAUD_FFMPEG=/path/to/ffmpeg` | ffmpeg used for uploads |
| `PLAUD_LINUX_SHORTCUTS=auto\|portal\|electron` | Global shortcut mechanism; `auto` adds the XDG portal in Wayland sessions |
| `PLAUD_LINUX_LOG_LEVEL=debug` | More detail in the runtime log |
| `PLAUD_LINUX_NO_DESKTOP_INTEGRATION=1` | AppImage: do not install a desktop entry |
| `~/.config/plaud-linux-flags.conf` | Extra Electron/Chromium flags, one per line (for example `--ozone-platform=wayland`) |

### Global shortcuts

The recording shortcuts (highlight, screenshot) work in X11 sessions as
configured in the app. In Wayland sessions they are also registered through
the XDG GlobalShortcuts portal, so they work while other applications have
focus:

- KDE Plasma and GNOME 48+: confirm the dialog that appears the first time.
- Hyprland: bind them in your configuration, for example
  `bind = SUPER SHIFT, H, global, io.github.trukhinyuri.PlaudLinux:highlight`
  (Lua configuration:
  `hl.bind("SUPER + SHIFT + H", hl.dsp.global("io.github.trukhinyuri.PlaudLinux:highlight"))`).
  The ids are `highlight` and `screenshot`; `hyprctl globalshortcuts` lists
  them once the app has registered them.

### Launch at login

The app enables launch at login on first start, as on macOS and Windows,
by writing `~/.config/autostart/io.github.trukhinyuri.PlaudLinux.desktop`.
Turn it off in the app's settings.

### Updates

When Plaud publishes a new version, the app downloads the official release,
verifies it, applies the same patches and checks that the native modules of
your build cover everything the new version uses. An accepted version is
installed the way the app does it on macOS and Windows (when you choose to
update, or automatically while the app is idle) by restarting into it; the
installed files are not modified. A version that needs changes to this
project is skipped with one notification and the current version keeps
working; update this project to get it. Downloaded and prepared versions are
kept in directories only your user can access (`~/.local/share/plaud-linux`,
`~/.cache/plaud-linux`, `~/.local/state/plaud-linux`), and a prepared version
is started only if its files are unchanged.

## Uninstall

```sh
scripts/uninstall          # keeps your Plaud data in ~/.config/Plaud
scripts/uninstall --purge  # also removes data, logs, staged updates
```

Packages: remove them with your package manager. AppImage: run it once with
`--plaud-linux-remove-desktop-integration`, then delete the file.

## Troubleshooting

- Logs: `~/.config/Plaud/logs/` (`log-*.log` from the app,
  `linux-runtime-*.log` from this project).
- "No audio device": PipeWire is not running for your user, or it has no
  sources and sinks (`wpctl status`).
- "no keyring found" notification: no Secret Service was available, so
  tokens are stored unencrypted in `~/.config/Plaud`. Install and start
  GNOME Keyring, KWallet or KeePassXC (Secret Service enabled), then restart
  the app and sign in again.
- Uploads fail with an ffmpeg notification: install ffmpeg with libopus, or
  point `PLAUD_FFMPEG` at one.
- No tray icon on GNOME: enable the AppIndicator extension.
- The app starts without the Chromium sandbox (message on standard error):
  your system restricts unprivileged user namespaces (Ubuntu 23.10+). Use
  the deb package, which installs an AppArmor profile, or allow them.
- Updates are not applied and the runtime log says a directory "is writable
  by its group", "is writable by other users", "is a symbolic link" or "is
  owned by uid ...": the app only takes updates from private directories.
  Run `chmod 700` on the directory named in the log (or remove it); the app
  keeps running its current version meanwhile.
- Hyprland: the app reads the pointer position and the space bars reserve
  with `hyprctl`, and keeps its notification popups from taking the focus
  with a window rule it adds at run time (`hyprctl clients` shows them as
  `Plaud popup` while they open). On Omarchy's floating workspaces every
  window that opens is resized and centred; to let the app place its panel
  and popups there, add `o.window("^Plaud$", { float = true })` to
  `~/.config/hypr/hyprland.lua`.

## Development

```sh
npm ci
npm run fetch-official                 # download and verify the pinned release
npm run build:native                   # build both native modules
npm run dev                            # run patched app with stock Electron, logs to stderr
npm test                               # unit and integration tests
npm run test:native                    # native module tests (needs PipeWire)
node patcher/cli.js check --resources .cache/official/1.4.1/Resources
npm run smoke                          # start the built app and check the modules load
```

[docs/patches.md](docs/patches.md) explains how to update the patches for a
new Plaud release; CI checks the newest release daily.

## License

Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin).
Licensed under the Apache License, Version 2.0; see [LICENSE](LICENSE) and
[NOTICE](NOTICE). Plaud Desktop itself is proprietary software of PLAUD LLC
and is not covered by this license.
