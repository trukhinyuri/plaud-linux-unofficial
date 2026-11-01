#!/usr/bin/env node
'use strict';

// Builds the installer packages: deb, rpm and pacman packages that contain
// only this project's code (patcher, runtime, build scripts, launcher and the
// prebuilt Linux addons) and build the app on the target machine with
// plaud-linux-setup. These are the files a release publishes.
//
//   node scripts/package-installer.js [--arch x64|arm64] --targets deb,rpm,pacman
//   node scripts/package-installer.js --verify <package>...
//
// The addons must have been built first (npm run build:native); build them
// on the oldest distribution a package targets (Ubuntu 22.04 for the deb,
// Rocky Linux 9 for the rpm, Arch Linux for the pacman package).
// --verify lists the files of built packages and fails unless every file is
// one this project ships (see ALLOWED below); the release workflow runs it on
// every artifact before publishing.

const fs = require('node:fs');
const path = require('node:path');
const common = require('./lib/common');
const native = require('./build-native');
const { templateVars, renderPackagingFile } = require('./build');

const { REPO_ROOT, DIST_DIR, BUILD_DIR, ARCHES, constants, log, run, capture, which } = common;

const PACKAGE_NAME = `${constants.PACKAGE_NAME}-installer`;
const LIB_DIR = `/usr/lib/${PACKAGE_NAME}`;
const TOOLS = { deb: 'dpkg-deb', rpm: 'rpmbuild', pacman: 'makepkg' };

// Files copied from the repository into LIB_DIR (directories recursively,
// .js/.json only).
const LIB_FILES = [
  'package.json',
  'official.json',
  'LICENSE',
  'NOTICE',
  'patcher',
  'linux-runtime',
  'scripts/setup.js',
  'scripts/build.js',
  'scripts/build-native.js',
  'scripts/fetch-official.js',
  'scripts/install',
  'scripts/uninstall',
  'scripts/electron-checksums.json',
  'scripts/lib/common.js',
  'scripts/lib/electron.js',
  'packaging/launcher.sh',
  'packaging/ffmpeg.sh',
  'packaging/app.desktop.in',
  'packaging/metainfo.xml.in',
];

// Every path an installer package may contain. Anything else (in particular
// any .asar, .icns, .ico, .png, .zip, .dmg, .exe or other .node file) fails
// the build and --verify.
const ALLOWED = [
  /^usr\/bin\/plaud-linux(-setup)?$/,
  /^usr\/share\/applications\/io\.github\.trukhinyuri\.PlaudLinux\.desktop$/,
  /^usr\/share\/plaud-linux-unofficial-installer\/apparmor-profile$/,
  /^usr\/share\/doc\/plaud-linux-unofficial-installer\/(README\.md|copyright)$/,
  /^usr\/share\/licenses\/plaud-linux-unofficial-installer\/(LICENSE|NOTICE)$/,
  /^usr\/lib\/plaud-linux-unofficial-installer\/(package\.json|official\.json|LICENSE|NOTICE)$/,
  /^usr\/lib\/plaud-linux-unofficial-installer\/(patcher|linux-runtime)\/[\w./-]+\.(js|json)$/,
  /^usr\/lib\/plaud-linux-unofficial-installer\/scripts\/(setup\.js|build\.js|build-native\.js|fetch-official\.js|install|uninstall|electron-checksums\.json|lib\/(common|electron)\.js)$/,
  /^usr\/lib\/plaud-linux-unofficial-installer\/packaging\/(launcher\.sh|ffmpeg\.sh|app\.desktop\.in|metainfo\.xml\.in)$/,
  /^usr\/lib\/plaud-linux-unofficial-installer\/native\/(recorder\/build\/Release\/recorder_linux|audio-monitor\/build\/Release\/audio_monitor_linux)\.node$/,
];

// Packages whose files overlap with the installer: the full local packages
// built by scripts/package.js and packaging/arch/PKGBUILD.
const CONFLICTS = [constants.PACKAGE_NAME];

// Libraries the Electron executable links (DT_NEEDED of electron 36.7.4) plus
// those of the Linux addons; the installer packages do not contain Electron,
// so the rpm cannot derive these automatically.
const RPM_SONAMES = [
  'libglib-2.0.so.0', 'libgobject-2.0.so.0', 'libgio-2.0.so.0', 'libnss3.so', 'libnssutil3.so', 'libsmime3.so',
  'libnspr4.so', 'libdbus-1.so.3', 'libatk-1.0.so.0', 'libatk-bridge-2.0.so.0', 'libcups.so.2', 'libgtk-3.so.0',
  'libpango-1.0.so.0', 'libcairo.so.2', 'libX11.so.6', 'libXcomposite.so.1', 'libXdamage.so.1', 'libXext.so.6',
  'libXfixes.so.3', 'libXrandr.so.2', 'libgbm.so.1', 'libexpat.so.1', 'libxcb.so.1', 'libxkbcommon.so.0',
  'libudev.so.1', 'libasound.so.2', 'libatspi.so.0',
];

const PACMAN_DEPENDS = [
  'gtk3', 'nss', 'nspr', 'alsa-lib', 'mesa', 'libxkbcommon', 'libcups', 'at-spi2-core', 'dbus', 'pango', 'cairo',
  'libx11', 'libxcb', 'libxcomposite', 'libxdamage', 'libxext', 'libxfixes', 'libxrandr', 'expat', 'systemd-libs',
  'glib2', 'gcc-libs', 'libpipewire', 'opus', 'libogg', 'libpng', 'zlib', 'ffmpeg', 'xdg-utils', 'hicolor-icon-theme',
  'curl', 'unzip', 'ca-certificates',
];
const PACMAN_OPTDEPENDS = [
  'libsecret: keep credentials in the Secret Service keyring',
  'libnotify: desktop notifications',
  'xdg-desktop-portal: global shortcuts and screenshots in Wayland sessions',
  'grim: screenshots on wlroots-based compositors (Hyprland, Sway)',
];

function versions() {
  const ours = common.repoVersion();
  return { deb: `${ours}-1`, rpm: { version: ours, release: '1' }, pacman: ours, file: ours };
}

function relFiles(root) {
  const out = [];
  const walk = (rel) => {
    for (const entry of fs.readdirSync(path.join(root, rel), { withFileTypes: true })) {
      const r = rel ? `${rel}/${entry.name}` : entry.name;
      if (entry.isDirectory()) walk(r);
      else out.push(r);
    }
  };
  walk('');
  return out.sort();
}

/** Throws unless every path is one an installer package may contain. */
function checkAllowed(files, source) {
  const bad = files.filter((f) => !ALLOWED.some((re) => re.test(f)));
  if (bad.length) throw new Error(`${source} contains files an installer package must not ship:\n- ${bad.join('\n- ')}`);
  for (const required of ['usr/bin/plaud-linux', 'usr/bin/plaud-linux-setup', `${LIB_DIR.slice(1)}/scripts/setup.js`]) {
    if (!files.includes(required)) throw new Error(`${source} lacks ${required}`);
  }
}

function copyLibFile(rel, dest) {
  const src = path.join(REPO_ROOT, rel);
  if (fs.statSync(src).isDirectory()) {
    for (const entry of fs.readdirSync(src, { withFileTypes: true })) {
      const child = `${rel}/${entry.name}`;
      if (entry.isDirectory()) copyLibFile(child, dest);
      else if (/\.(js|json)$/.test(entry.name) && entry.name !== 'patch-manifest.json') copyLibFile(child, dest);
    }
    return;
  }
  const out = path.join(dest, rel);
  fs.mkdirSync(path.dirname(out), { recursive: true });
  fs.copyFileSync(src, out);
  fs.chmodSync(out, fs.statSync(src).mode & 0o111 ? 0o755 : 0o644);
}

/** Writes the installer file tree into `root`. */
function stageRoot(root) {
  common.rmrf(root);
  const lib = path.join(root, LIB_DIR.slice(1));
  for (const rel of LIB_FILES) copyLibFile(rel, lib);
  for (const addon of native.ADDONS) {
    const src = native.addonPath(addon);
    if (!fs.existsSync(src)) throw new Error(`missing ${path.relative(REPO_ROOT, src)}; run npm run build:native first`);
    const out = path.join(lib, path.relative(REPO_ROOT, src));
    fs.mkdirSync(path.dirname(out), { recursive: true });
    fs.copyFileSync(src, out);
    fs.chmodSync(out, 0o755);
  }

  const electron = require('./electron-checksums.json')[constants.ELECTRON_VERSION];
  if (!electron) throw new Error(`no pinned checksums for Electron ${constants.ELECTRON_VERSION}`);
  const vars = templateVars({
    LIBDIR: LIB_DIR,
    PACKAGE_NAME,
    ELECTRON_VERSION: constants.ELECTRON_VERSION,
    ELECTRON_SHA256_X64: electron[`electron-v${constants.ELECTRON_VERSION}-linux-x64.zip`],
    ELECTRON_SHA256_ARM64: electron[`electron-v${constants.ELECTRON_VERSION}-linux-arm64.zip`],
    EXEC: constants.COMMAND,
  });
  const bin = path.join(root, 'usr', 'bin');
  fs.mkdirSync(bin, { recursive: true });
  fs.writeFileSync(path.join(bin, constants.COMMAND), renderPackagingFile('installer/plaud-linux.in', vars), { mode: 0o755 });
  fs.writeFileSync(path.join(bin, `${constants.COMMAND}-setup`), renderPackagingFile('installer/plaud-linux-setup.in', vars), { mode: 0o755 });

  const apps = path.join(root, 'usr', 'share', 'applications');
  fs.mkdirSync(apps, { recursive: true });
  fs.writeFileSync(path.join(apps, `${constants.APP_ID}.desktop`), renderPackagingFile('app.desktop.in', vars));

  const doc = path.join(root, 'usr', 'share', 'doc', PACKAGE_NAME);
  fs.mkdirSync(doc, { recursive: true });
  fs.copyFileSync(path.join(REPO_ROOT, 'README.md'), path.join(doc, 'README.md'));
  const licenses = path.join(root, 'usr', 'share', 'licenses', PACKAGE_NAME);
  fs.mkdirSync(licenses, { recursive: true });
  for (const file of ['LICENSE', 'NOTICE']) fs.copyFileSync(path.join(REPO_ROOT, file), path.join(licenses, file));
  return vars;
}

function packageDeb({ arch, workDir }) {
  const root = path.join(workDir, 'deb-root');
  const vars = stageRoot(root);
  const share = path.join(root, 'usr', 'share', PACKAGE_NAME);
  fs.mkdirSync(share, { recursive: true });
  fs.writeFileSync(path.join(share, 'apparmor-profile'), renderPackagingFile('deb/apparmor-profile', vars));
  fs.writeFileSync(
    path.join(root, 'usr', 'share', 'doc', PACKAGE_NAME, 'copyright'),
    [
      'Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/',
      `Upstream-Name: ${constants.PACKAGE_NAME}`,
      `Upstream-Contact: ${vars.MAINTAINER}`,
      `Source: ${constants.PROJECT_URL}`,
      '',
      'Files: *',
      'Copyright: 2026 Iurii Trukhin (also known as Yuri Trukhin)',
      'License: Apache-2.0',
      ' On Debian systems the full text is in /usr/share/common-licenses/Apache-2.0.',
      '',
    ].join('\n'),
  );
  checkAllowed(relFiles(root), 'deb tree');
  const size = relFiles(root).reduce((n, f) => n + fs.statSync(path.join(root, f)).size, 0);
  const debVars = {
    ...vars,
    DEB_VERSION: versions().deb,
    DEB_ARCH: ARCHES[arch].deb,
    INSTALLED_SIZE: Math.ceil(size / 1024),
    CONFLICTS: CONFLICTS.join(', '),
  };
  const debian = path.join(root, 'DEBIAN');
  fs.mkdirSync(debian, { recursive: true });
  fs.writeFileSync(path.join(debian, 'control'), renderPackagingFile('installer/deb/control.in', debVars));
  for (const script of ['postinst', 'prerm', 'postrm']) {
    fs.writeFileSync(path.join(debian, script), renderPackagingFile(`installer/deb/${script}`, debVars), { mode: 0o755 });
  }
  const out = path.join(DIST_DIR, `${PACKAGE_NAME}_${versions().deb}_${ARCHES[arch].deb}.deb`);
  fs.mkdirSync(DIST_DIR, { recursive: true });
  run(which('dpkg-deb'), ['--root-owner-group', '-Zxz', '--build', root, out]);
  return out;
}

function packageRpm({ arch, workDir }) {
  const root = path.join(workDir, 'rpm-root');
  const vars = stageRoot(root);
  const files = relFiles(root);
  checkAllowed(files, 'rpm tree');
  const top = path.join(workDir, 'rpmbuild');
  common.rmrf(top);
  for (const d of ['BUILD', 'RPMS', 'SOURCES', 'SPECS', 'SRPMS', 'BUILDROOT']) fs.mkdirSync(path.join(top, d), { recursive: true });
  const v = versions().rpm;
  const spec = path.join(top, 'SPECS', `${PACKAGE_NAME}.spec`);
  fs.writeFileSync(
    spec,
    renderPackagingFile('installer/rpm/package.spec.in', {
      ...vars,
      RPM_VERSION: v.version,
      RPM_RELEASE: v.release,
      RPM_ARCH: ARCHES[arch].rpm,
      CONFLICTS: CONFLICTS.join(' '),
      RPM_REQUIRES: RPM_SONAMES.map((s) => `Requires:       ${s}()(64bit)`).join('\n'),
    }),
  );
  const dirs = new Set();
  for (const f of files) {
    if (f.startsWith(`${LIB_DIR.slice(1)}/`)) {
      for (let d = path.posix.dirname(f); d.startsWith(LIB_DIR.slice(1)); d = path.posix.dirname(d)) dirs.add(d);
    }
  }
  dirs.add(`usr/share/doc/${PACKAGE_NAME}`);
  dirs.add(`usr/share/licenses/${PACKAGE_NAME}`);
  const lines = [...[...dirs].sort().map((d) => `%dir "/${d}"`), ...files.map((f) => {
    if (f.startsWith('usr/share/licenses/')) return `%license "/${f}"`;
    if (f.startsWith('usr/share/doc/')) return `%doc "/${f}"`;
    return `"/${f}"`;
  })];
  const filelist = path.join(top, 'filelist');
  fs.writeFileSync(filelist, `${lines.join('\n')}\n`);
  run(which('rpmbuild'), [
    '-bb', spec,
    '--target', ARCHES[arch].rpm,
    '--define', `_topdir ${top}`,
    '--define', `stagedroot ${root}`,
    '--define', `filelist ${filelist}`,
  ]);
  const built = path.join(top, 'RPMS', ARCHES[arch].rpm);
  const rpm = fs.readdirSync(built).find((f) => f.endsWith('.rpm'));
  if (!rpm) throw new Error('rpmbuild produced no package');
  const out = path.join(DIST_DIR, rpm);
  fs.mkdirSync(DIST_DIR, { recursive: true });
  fs.copyFileSync(path.join(built, rpm), out);
  return out;
}

function packagePacman({ arch, workDir }) {
  const root = path.join(workDir, 'pacman-root');
  const vars = stageRoot(root);
  checkAllowed(relFiles(root), 'pacman tree');
  const dir = path.join(workDir, 'pacman');
  common.rmrf(dir);
  fs.mkdirSync(dir, { recursive: true });
  const quote = (s) => `'${s.replace(/'/g, "'\\''")}'`;
  const pkgVars = {
    ...vars,
    PKG_VERSION: versions().pacman,
    PACMAN_ARCH: ARCHES[arch].pacman,
    DEPENDS: PACMAN_DEPENDS.map(quote).join(' '),
    OPTDEPENDS: PACMAN_OPTDEPENDS.map(quote).join(' '),
    CONFLICTS: CONFLICTS.map(quote).join(' '),
    STAGED_ROOT: root,
  };
  fs.writeFileSync(path.join(dir, 'PKGBUILD'), renderPackagingFile('installer/arch/PKGBUILD.in', pkgVars));
  fs.writeFileSync(path.join(dir, `${PACKAGE_NAME}.install`), renderPackagingFile('installer/arch/install.in', pkgVars));
  fs.mkdirSync(DIST_DIR, { recursive: true });
  run(which('makepkg'), ['--force', '--nodeps', '--noconfirm'], {
    cwd: dir,
    env: { ...process.env, PKGDEST: DIST_DIR, PKGEXT: '.pkg.tar.zst', PACKAGER: vars.MAINTAINER },
  });
  const file = fs
    .readdirSync(DIST_DIR)
    .find((f) => f.startsWith(`${PACKAGE_NAME}-${versions().pacman}-1-${ARCHES[arch].pacman}.pkg.tar`));
  if (!file) throw new Error('makepkg produced no package');
  return path.join(DIST_DIR, file);
}

const BUILDERS = { deb: packageDeb, rpm: packageRpm, pacman: packagePacman };

/** Lists the regular files and symlinks in a built package (paths without leading "/" or "./"). */
function listPackage(file) {
  let out;
  if (file.endsWith('.deb')) {
    out = capture('sh', ['-c', 'dpkg-deb --fsys-tarfile "$1" | tar -tvf -', 'sh', file]);
  } else if (file.endsWith('.rpm')) {
    out = capture('rpm', ['-qlvp', '--nosignature', file]);
  } else if (/\.pkg\.tar\.zst$/.test(file)) {
    out = capture('tar', ['--zstd', '-tvf', file]);
  } else {
    throw new Error(`unknown package type: ${file}`);
  }
  if (out === null) throw new Error(`cannot list ${file} (is the package tool installed?)`);
  const files = [];
  for (const line of out.split('\n')) {
    if (!line || line[0] === 'd') continue;
    const name = line.replace(/ -> .*$/, '').split(/\s+/).pop().replace(/^\.?\//, '');
    // pacman metadata
    if (/^\.(PKGINFO|BUILDINFO|MTREE|INSTALL)$/.test(name)) continue;
    files.push(name);
  }
  return files.sort();
}

function verify(files) {
  for (const file of files) {
    const listed = listPackage(file);
    checkAllowed(listed, path.basename(file));
    log('verify', `${path.basename(file)}: ${listed.length} files, all from this project`);
  }
}

async function packageTargets({ arch, targets }) {
  arch = common.resolveArch(arch);
  if (arch !== process.arch) throw new Error(`the addons are built for the host; packaging ${arch} on ${process.arch} is not supported`);
  const workDir = path.join(BUILD_DIR, `installer-${arch}`);
  fs.mkdirSync(workDir, { recursive: true });
  const results = [];
  for (const target of targets) {
    if (!BUILDERS[target]) throw new Error(`unknown target "${target}" (known: ${Object.keys(BUILDERS).join(', ')})`);
    if (!which(TOOLS[target])) throw new Error(`target ${target} needs ${TOOLS[target]}, which is not on PATH`);
    log('installer', `building ${target}`);
    const out = BUILDERS[target]({ arch, workDir });
    log('installer', `${target}: ${path.relative(process.cwd(), out)} (${(fs.statSync(out).size / 1048576).toFixed(1)} MiB)`);
    results.push(out);
  }
  return results;
}

module.exports = { ALLOWED, checkAllowed, stageRoot, listPackage, PACKAGE_NAME, LIB_DIR };

if (require.main === module) {
  const args = common.parseArgs(process.argv.slice(2), { booleans: ['verify'] });
  let job;
  if (args.verify) {
    job = Promise.resolve().then(() => verify(args._.map((f) => path.resolve(f))));
  } else {
    if (typeof args.targets !== 'string') {
      process.stderr.write('usage: package-installer.js [--arch x64|arm64] --targets deb,rpm,pacman | --verify <package>...\n');
      process.exit(2);
    }
    const targets = args.targets.split(',').map((t) => t.trim()).filter(Boolean);
    job = packageTargets({ arch: typeof args.arch === 'string' ? args.arch : undefined, targets });
  }
  job.catch((error) => {
    process.stderr.write(`package-installer: ${error.stack ?? error.message}\n`);
    process.exitCode = 1;
  });
}
