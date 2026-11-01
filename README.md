# Plaud Desktop for Linux (unofficial)

Runs the official [Plaud](https://www.plaud.ai/) Desktop app on Linux:
meeting detection, recording of microphone and system audio with echo
cancellation, highlights and screenshots, upload for transcription and
summaries, the tray panel, notifications, global shortcuts, `plaud://`
sign-in links, launch at login and in-app updates.

> **Unofficial.** This project is not affiliated with, endorsed by, or
> sponsored by PLAUD LLC. "Plaud" is a trademark of its owner. Use of the
> Plaud service is subject to Plaud's terms.

> **macOS and Windows:** use the official Plaud Desktop app from Plaud
> ([Plaud Desktop](https://www.plaud.ai/pages/plaud-desktop); signed-in users
> also find it in Plaud Web under Explore > Plaud apps). This project is only
> for Linux, where Plaud offers no app.

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
- build and packaging scripts, and installer packages.

The repository and the published installer packages contain no Plaud code.
The official release is downloaded from Plaud's servers, verified against
the checksum Plaud publishes, patched and combined with stock Electron, all
on your machine. Apps and packages you build from source contain Plaud's
proprietary code: **do not redistribute them.** Details:
[docs/architecture.md](docs/architecture.md).

## Requirements

- x86_64 or aarch64, glibc-based distribution: Ubuntu 22.04+, Debian 12+,
  Fedora, RHEL/Rocky/AlmaLinux 9+, Arch Linux (also Arch Linux ARM and
  Omarchy)
- A PipeWire session (PipeWire 0.3.48 or newer, with WirePlumber;
  `pipewire-pulse` is fine, a PulseAudio daemon is not needed)
- `ffmpeg` with libopus (uploads are encoded with the system ffmpeg)
- A StatusNotifierItem tray: KDE Plasma, Waybar (Hyprland, Sway), or GNOME
  with the AppIndicator extension (preinstalled on Ubuntu)
- Recommended: a Secret Service (GNOME Keyring, KWallet or KeePassXC) for
  the sign-in tokens, and `xdg-desktop-portal` with a backend for your
  desktop
- Network access to `client-download.plaud.ai` and `github.com` when the
  app is built (installation of a package, or a build from source)

## Install

### Installer packages (recommended)

Download the package for your distribution and architecture from the
[releases page](https://github.com/trukhinyuri/plaud-linux-unofficial/releases),
check it, and install it with your package manager:

```sh
sha256sum -c SHA256SUMS --ignore-missing
gh attestation verify <package> --repo trukhinyuri/plaud-linux-unofficial   # optional: build provenance
```

| Distribution | Command |
|---|---|
| Ubuntu 22.04+, Debian 12+ | `sudo apt install ./plaud-linux-unofficial-installer_<version>-1_amd64.deb` (`_arm64.deb` on aarch64) |
| Fedora | `sudo dnf install ./plaud-linux-unofficial-installer-<version>-1.x86_64.rpm` (`.aarch64.rpm`) |
| RHEL, Rocky Linux, AlmaLinux 9+ | `sudo dnf install epel-release && sudo dnf config-manager --set-enabled crb` (ffmpeg comes from EPEL), then the Fedora command |
| Arch Linux, Omarchy, Arch Linux ARM | `sudo pacman -U ./plaud-linux-unofficial-installer-<version>-1-x86_64.pkg.tar.zst` (`-aarch64` on aarch64) |

The packages contain only this project's code (the patcher, the Linux
runtime, the build scripts and the compiled Linux modules). While the
package installs, `plaud-linux-setup` downloads stock Electron from GitHub
and the official Plaud release from Plaud's update server, verifies both
against the checksums pinned in the package (SHA-256 for Electron, the
SHA-512 Plaud publishes for its release; a mismatch stops the build), and
builds the app in `/opt/plaud-linux` with the setuid Chromium sandbox. This
takes about a minute and needs about 400 MB of disk space.

If that step fails (for example without network), the package still
installs: run `sudo plaud-linux-setup` later, or just start Plaud, which
then builds a copy for your user in `~/.local/share/plaud-linux/app`.
`plaud-linux-setup --status` shows what is installed.

### Per user, from source

Build dependencies:

| Distribution | Packages |
|---|---|
| Arch Linux, Omarchy | `nodejs npm git cmake ninja meson python pkgconf libpipewire opus libogg dbus libpng zlib libx11 libxrandr` |
| Debian, Ubuntu | `nodejs npm git build-essential cmake ninja-build meson python3 pkg-config libpipewire-0.3-dev libopus-dev libogg-dev libdbus-1-dev libpng-dev zlib1g-dev libx11-dev libxrandr-dev` |
| Fedora, RHEL 9 (with CRB) | `nodejs npm git gcc-c++ cmake ninja-build meson python3 pkgconf pipewire-devel opus-devel libogg-devel dbus-devel libpng-devel zlib-devel libX11-devel libXrandr-devel` |

Node.js 22.12 or newer is required (Ubuntu 22.04/24.04 and RHEL 9 ship
older versions; use the installer packages there, or Node.js from
nodejs.org).

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

### Packages built from source

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

Arch Linux and Omarchy users can also build with
[`packaging/arch/PKGBUILD`](packaging/arch/PKGBUILD) (`makepkg -si` in that
directory), which downloads and verifies all sources itself and builds from
the release tag `v<pkgver>`.

These packages contain Plaud's code: they are for your own machines and must
not be published. They conflict with the installer package; install one or
the other.

### Building the installer packages

```sh
npm run build:native
node scripts/package-installer.js --targets deb,rpm,pacman   # tools: dpkg-deb, rpmbuild, makepkg
node scripts/package-installer.js --verify dist/*installer*  # only this project's files
```

Releases build them in containers of the oldest supported distribution
(`scripts/ci/installer-build.sh`: Ubuntu 22.04, Rocky Linux 9, Arch Linux,
Arch Linux ARM), so the modules work on every newer one.

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

Updates of this project:

| Installed with | Update |
|---|---|
| Installer package | Install the newer package; its install script rebuilds `/opt/plaud-linux` (a per-user copy is rebuilt at the next start) |
| Source (`scripts/install`) | `git pull && npm ci && npm run build && scripts/install` with the app closed |
| Packages built from source | Rebuild them from the updated source and install them |

## Uninstall

| Installed with | Remove |
|---|---|
| Installer package | `sudo apt purge plaud-linux-unofficial-installer`, `sudo dnf remove plaud-linux-unofficial-installer` or `sudo pacman -Rns plaud-linux-unofficial-installer`; this also removes `/opt/plaud-linux`, its icons in `/usr/local/share` and the download cache. A per-user copy: `plaud-linux-setup --user --remove` first |
| Source | `scripts/uninstall` (keeps your Plaud data in `~/.config/Plaud`); `scripts/uninstall --purge` also removes data, logs and staged updates |
| Packages built from source | your package manager |
| AppImage | run it once with `--plaud-linux-remove-desktop-integration`, then delete the file |

Your Plaud data (`~/.config/Plaud`: session, settings, recordings cache) is
kept by every method except `scripts/uninstall --purge`.

## Troubleshooting

- Logs: `~/.config/Plaud/logs/` (`log-*.log` from the app,
  `linux-runtime-*.log` from this project); a per-user build started from
  the menu logs to `~/.local/state/plaud-linux/setup.log`.
- Installing the package printed "building the app failed": run
  `sudo plaud-linux-setup` and read its output. A checksum mismatch means a
  download was corrupted or changed: it is deleted, retry later. If Plaud's
  server stops offering the pinned release, update this project.
- RHEL, Rocky, AlmaLinux: `nothing provides /usr/bin/ffmpeg` means EPEL and
  CRB are not enabled (see Install).
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
  your system restricts unprivileged user namespaces (Ubuntu 23.10+) and the
  app runs from your home directory. Install the deb installer package, which
  builds the app in `/opt/plaud-linux` with the setuid sandbox and an
  AppArmor profile, or allow user namespaces.
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
