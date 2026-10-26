#!/usr/bin/env node
'use strict';

// Builds distribution packages from build/linux-<arch> (running the build
// first when it is missing):
//
//   node scripts/package.js [--arch x64|arm64] [--targets appimage,deb,rpm,pacman]
//                           [--rebuild] [--stage-root <dir>]
//
// Without --targets every target whose tool is installed is built and the
// others are reported as skipped; an explicitly requested target fails when
// its tool is missing. --stage-root only writes the system file tree
// (/opt, /usr) into <dir>, for use by external packaging such as the AUR
// PKGBUILD.
//
// Tools: AppImage needs mksquashfs (squashfs-tools); deb needs dpkg-deb;
// rpm needs rpmbuild; pacman needs makepkg. The output goes to dist/ and
// contains official Plaud code: it is for local installation only.

const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const common = require('./lib/common');
const { build, templateVars, renderPackagingFile } = require('./build');

const { REPO_ROOT, BUILD_DIR, DIST_DIR, CACHE_DIR, ARCHES, constants, log, run, which } = common;

const TOOLS = { appimage: 'mksquashfs', deb: 'dpkg-deb', rpm: 'rpmbuild', pacman: 'makepkg' };

const PACMAN_DEPENDS = [
  'gtk3', 'nss', 'nspr', 'alsa-lib', 'mesa', 'libxkbcommon', 'libcups', 'at-spi2-core', 'dbus', 'pango', 'cairo',
  'libx11', 'libxcb', 'libxcomposite', 'libxdamage', 'libxext', 'libxfixes', 'libxrandr', 'expat', 'systemd-libs',
  'glib2', 'gcc-libs', 'libpipewire', 'opus', 'libogg', 'libpng', 'zlib', 'ffmpeg', 'xdg-utils', 'hicolor-icon-theme',
];
const PACMAN_OPTDEPENDS = [
  'libsecret: keep credentials in the Secret Service keyring',
  'libnotify: desktop notifications',
  'xdg-desktop-portal: global shortcuts and screenshots in Wayland sessions',
  'grim: screenshots on wlroots-based compositors (Hyprland, Sway)',
];

// Shared libraries the Linux addons may link against (checked against their
// DT_NEEDED entries). Each must be covered by the dependencies of every
// format: PACMAN_DEPENDS above, packaging/deb/control.in, and the rpm's
// automatic soname requirements. libX11 and libXrandr are loaded with
// dlopen() by the recorder and are Electron dependencies anyway.
//   libpipewire-0.3.so.0  libpipewire / libpipewire-0.3-0t64
//   libopus.so.0          opus / libopus0
//   libogg.so.0           libogg / libogg0
//   libdbus-1.so.3        dbus / libdbus-1-3
//   libpng16.so.16        libpng / libpng16-16t64
//   libz.so.1             zlib / zlib1g
//   libstdc++, libgcc_s   gcc-libs / libstdc++6, libgcc-s1
const ADDON_LIBS = new Set([
  'libpipewire-0.3.so.0', 'libopus.so.0', 'libogg.so.0', 'libdbus-1.so.3', 'libpng16.so.16', 'libz.so.1',
  'libstdc++.so.6', 'libgcc_s.so.1',
  'libc.so.6', 'libm.so.6', 'libdl.so.2', 'libpthread.so.0', 'ld-linux-aarch64.so.1', 'ld-linux-x86-64.so.2',
]);

function versions(buildInfo) {
  const ours = buildInfo.version;
  const plaud = buildInfo.upstreamVersion;
  return {
    deb: `${ours}+plaud${plaud}-1`,
    rpm: { version: `${ours}+plaud${plaud}`, release: '1' },
    pacman: `${ours}_plaud${plaud}`,
    file: `${ours}+plaud${plaud}`,
  };
}

function dirSize(dir) {
  let total = 0;
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, entry.name);
    if (entry.isDirectory()) total += dirSize(p);
    else if (entry.isFile()) total += fs.statSync(p).size;
  }
  return total;
}

/** Lists DT_NEEDED entries of an ELF file with readelf. */
function neededLibs(file) {
  const readelf = which('readelf') ?? which('llvm-readelf');
  if (!readelf) throw new Error('readelf (binutils) or llvm-readelf is required to check addon dependencies');
  const out = common.capture(readelf, ['-d', file]);
  if (out === null) throw new Error(`readelf failed on ${file}`);
  return [...out.matchAll(/\(NEEDED\)\s+Shared library: \[([^\]]+)\]/g)].map((m) => m[1]);
}

function checkAddonDependencies(appDir) {
  for (const file of ['recorder_linux.node', 'audio_monitor_linux.node']) {
    const unexpected = neededLibs(path.join(appDir, 'resources', file)).filter((lib) => !ADDON_LIBS.has(lib));
    if (unexpected.length) {
      throw new Error(`${file} links against ${unexpected.join(', ')}; add the providing packages to scripts/package.js`);
    }
  }
}

/** Writes the system file tree (/opt/<cmd>, /usr/...) into `root`, which must be empty or absent. */
function stageSystemRoot(buildOut, root) {
  if (fs.existsSync(root) && fs.readdirSync(root).length > 0) throw new Error(`stage root ${root} is not empty`);
  fs.mkdirSync(root, { recursive: true });
  const opt = path.join(root, 'opt', constants.COMMAND);
  fs.mkdirSync(path.dirname(opt), { recursive: true });
  common.copyTree(path.join(buildOut, 'app'), opt);
  fs.chmodSync(path.join(opt, 'chrome-sandbox'), 0o4755);
  const bin = path.join(root, 'usr', 'bin');
  fs.mkdirSync(bin, { recursive: true });
  fs.symlinkSync(`../../opt/${constants.COMMAND}/${constants.COMMAND}`, path.join(bin, constants.COMMAND));
  common.copyTree(path.join(buildOut, 'share'), path.join(root, 'usr', 'share'));
  const licenses = path.join(root, 'usr', 'share', 'licenses', constants.PACKAGE_NAME);
  fs.mkdirSync(licenses, { recursive: true });
  for (const file of ['LICENSE', 'NOTICE']) fs.copyFileSync(path.join(REPO_ROOT, file), path.join(licenses, file));
  return root;
}

function freshDir(parent, name) {
  const dir = path.join(parent, name);
  common.rmrf(dir);
  return dir;
}

async function downloadAppImageRuntime(arch) {
  const pinned = require('./appimage-runtime.json');
  const name = `runtime-${ARCHES[arch].appimage}`;
  const expected = pinned.runtimes[ARCHES[arch].appimage];
  const file = path.join(CACHE_DIR, 'appimage', pinned.release, name);
  const sha256 = (p) => crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');
  if (!fs.existsSync(file) || sha256(file) !== expected) {
    const url = `https://github.com/AppImage/type2-runtime/releases/download/${pinned.release}/${name}`;
    log('package', `downloading ${url}`);
    const res = await fetch(url);
    if (!res.ok) throw new Error(`GET ${url} failed with HTTP ${res.status}`);
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, Buffer.from(await res.arrayBuffer()));
    const actual = sha256(file);
    if (actual !== expected) throw new Error(`AppImage runtime checksum mismatch: expected ${expected}, got ${actual}`);
  }
  return file;
}

async function packageAppImage({ arch, buildOut, buildInfo, workDir }) {
  const appDir = path.join(workDir, 'AppDir');
  common.rmrf(appDir);
  const lib = path.join(appDir, 'usr', 'lib', constants.COMMAND);
  fs.mkdirSync(path.dirname(lib), { recursive: true });
  common.copyTree(path.join(buildOut, 'app'), lib);
  common.copyTree(path.join(buildOut, 'share'), path.join(appDir, 'usr', 'share'));
  const vars = templateVars();
  fs.writeFileSync(path.join(appDir, 'AppRun'), renderPackagingFile('appimage/AppRun', vars), { mode: 0o755 });
  fs.copyFileSync(path.join(buildOut, 'share', 'applications', `${constants.APP_ID}.desktop`), path.join(appDir, `${constants.APP_ID}.desktop`));
  fs.copyFileSync(path.join(buildOut, 'share', 'icons', 'hicolor', '256x256', 'apps', `${constants.APP_ID}.png`), path.join(appDir, `${constants.APP_ID}.png`));
  fs.symlinkSync(`${constants.APP_ID}.png`, path.join(appDir, '.DirIcon'));

  const squashfs = path.join(workDir, 'app.squashfs');
  fs.rmSync(squashfs, { force: true });
  const epoch = process.env.SOURCE_DATE_EPOCH;
  run(which('mksquashfs'), [
    appDir, squashfs, '-root-owned', '-noappend', '-no-xattrs', '-comp', 'zstd', '-Xcompression-level', '19', '-quiet',
    ...(epoch ? ['-all-time', epoch, '-mkfs-time', epoch] : []),
  ]);
  const runtime = await downloadAppImageRuntime(arch);
  const out = path.join(DIST_DIR, `Plaud-unofficial-${versions(buildInfo).file}-${ARCHES[arch].appimage}.AppImage`);
  fs.mkdirSync(DIST_DIR, { recursive: true });
  const fd = fs.openSync(out, 'w', 0o755);
  try {
    fs.writeSync(fd, fs.readFileSync(runtime));
    fs.writeSync(fd, fs.readFileSync(squashfs));
  } finally {
    fs.closeSync(fd);
  }
  fs.chmodSync(out, 0o755);
  fs.rmSync(squashfs, { force: true });
  return out;
}

function packageDeb({ arch, buildOut, buildInfo, workDir }) {
  const root = stageSystemRoot(buildOut, freshDir(workDir, 'deb-root'));
  const share = path.join(root, 'usr', 'share', constants.PACKAGE_NAME);
  fs.mkdirSync(share, { recursive: true });
  const vars = templateVars({
    PACKAGE_NAME: constants.PACKAGE_NAME,
    DEB_VERSION: versions(buildInfo).deb,
    DEB_ARCH: ARCHES[arch].deb,
    INSTALLED_SIZE: Math.ceil(dirSize(root) / 1024),
  });
  fs.writeFileSync(path.join(share, 'apparmor-profile'), renderPackagingFile('deb/apparmor-profile', vars));
  const docDir = path.join(root, 'usr', 'share', 'doc', constants.PACKAGE_NAME);
  fs.mkdirSync(docDir, { recursive: true });
  fs.writeFileSync(
    path.join(docDir, 'copyright'),
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
      `Files: opt/${constants.COMMAND}/resources/app.asar`,
      'Copyright: PLAUD LLC',
      'License: proprietary',
      ' Official Plaud Desktop application code, downloaded from Plaud when this',
      ' package was built. Not covered by the Apache License; do not redistribute.',
      '',
      `Files: opt/${constants.COMMAND}/*.so opt/${constants.COMMAND}/${constants.EXECUTABLE} opt/${constants.COMMAND}/chrome*`,
      'Copyright: The Electron and Chromium authors',
      `License: MIT and others; see /opt/${constants.COMMAND}/LICENSE and LICENSES.chromium.html`,
      '',
    ].join('\n'),
  );
  const debian = path.join(root, 'DEBIAN');
  fs.mkdirSync(debian, { recursive: true });
  fs.writeFileSync(path.join(debian, 'control'), renderPackagingFile('deb/control.in', vars));
  for (const script of ['postinst', 'postrm']) {
    fs.writeFileSync(path.join(debian, script), renderPackagingFile(`deb/${script}`, vars), { mode: 0o755 });
  }
  const out = path.join(DIST_DIR, `${constants.PACKAGE_NAME}_${versions(buildInfo).deb}_${ARCHES[arch].deb}.deb`);
  fs.mkdirSync(DIST_DIR, { recursive: true });
  run(which('dpkg-deb'), ['--root-owner-group', '-Zxz', '--build', root, out]);
  return out;
}

function rpmFileList(root) {
  const lines = [];
  const walk = (rel) => {
    const abs = path.join(root, rel);
    const stat = fs.lstatSync(abs);
    if (stat.isDirectory()) {
      lines.push(`%dir "/${rel}"`);
      for (const name of fs.readdirSync(abs).sort()) walk(path.posix.join(rel, name));
    } else if (rel === `opt/${constants.COMMAND}/chrome-sandbox`) {
      lines.push(`%attr(4755, root, root) "/${rel}"`);
    } else {
      lines.push(`"/${rel}"`);
    }
  };
  walk(`opt/${constants.COMMAND}`);
  lines.push(`"/usr/bin/${constants.COMMAND}"`);
  lines.push(`"/usr/share/applications/${constants.APP_ID}.desktop"`);
  lines.push(`"/usr/share/metainfo/${constants.APP_ID}.metainfo.xml"`);
  for (const size of fs.readdirSync(path.join(root, 'usr/share/icons/hicolor')).sort()) {
    lines.push(`"/usr/share/icons/hicolor/${size}/apps/${constants.APP_ID}.png"`);
  }
  lines.push(`%license "/usr/share/licenses/${constants.PACKAGE_NAME}/LICENSE"`);
  lines.push(`%license "/usr/share/licenses/${constants.PACKAGE_NAME}/NOTICE"`);
  return `${lines.join('\n')}\n`;
}

function packageRpm({ arch, buildOut, buildInfo, workDir }) {
  const root = stageSystemRoot(buildOut, freshDir(workDir, 'rpm-root'));
  const top = path.join(workDir, 'rpmbuild');
  common.rmrf(top);
  for (const d of ['BUILD', 'RPMS', 'SOURCES', 'SPECS', 'SRPMS', 'BUILDROOT']) fs.mkdirSync(path.join(top, d), { recursive: true });
  const v = versions(buildInfo).rpm;
  const vars = templateVars({ PACKAGE_NAME: constants.PACKAGE_NAME, RPM_VERSION: v.version, RPM_RELEASE: v.release, RPM_ARCH: ARCHES[arch].rpm });
  const spec = path.join(top, 'SPECS', `${constants.PACKAGE_NAME}.spec`);
  fs.writeFileSync(spec, renderPackagingFile('rpm/package.spec.in', vars));
  const filelist = path.join(top, 'filelist');
  fs.writeFileSync(filelist, rpmFileList(root));
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

function packagePacman({ arch, buildOut, buildInfo, workDir }) {
  const root = stageSystemRoot(buildOut, freshDir(workDir, 'pacman-root'));
  const dir = path.join(workDir, 'pacman');
  common.rmrf(dir);
  fs.mkdirSync(dir, { recursive: true });
  const quote = (s) => `'${s.replace(/'/g, "'\\''")}'`;
  const vars = templateVars({
    PACKAGE_NAME: constants.PACKAGE_NAME,
    PKG_VERSION: versions(buildInfo).pacman,
    PACMAN_ARCH: ARCHES[arch].pacman,
    DEPENDS: PACMAN_DEPENDS.map(quote).join(' '),
    OPTDEPENDS: PACMAN_OPTDEPENDS.map(quote).join(' '),
    STAGED_ROOT: root,
  });
  fs.writeFileSync(path.join(dir, 'PKGBUILD'), renderPackagingFile('arch/PKGBUILD.local.in', vars));
  run(which('makepkg'), ['--force', '--nodeps', '--noconfirm'], {
    cwd: dir,
    env: { ...process.env, PKGDEST: DIST_DIR, PKGEXT: '.pkg.tar.zst', PACKAGER: vars.MAINTAINER },
  });
  const file = fs
    .readdirSync(DIST_DIR)
    .find((f) => f.startsWith(`${constants.PACKAGE_NAME}-${versions(buildInfo).pacman}-1-${ARCHES[arch].pacman}.pkg.tar`));
  if (!file) throw new Error('makepkg produced no package');
  return path.join(DIST_DIR, file);
}

const BUILDERS = { appimage: packageAppImage, deb: packageDeb, rpm: packageRpm, pacman: packagePacman };

async function packageTargets({ arch, targets, rebuild = false }) {
  arch = common.resolveArch(arch);
  const buildOut = path.join(BUILD_DIR, `linux-${arch}`);
  if (rebuild || !fs.existsSync(path.join(buildOut, 'app', constants.COMMAND))) await build({ arch });
  const buildInfo = JSON.parse(fs.readFileSync(path.join(buildOut, 'app', 'resources', constants.RESOURCES_SUBDIR, 'build-info.json'), 'utf8'));
  checkAddonDependencies(path.join(buildOut, 'app'));

  const explicit = Boolean(targets);
  const wanted = explicit ? targets : Object.keys(BUILDERS);
  const results = [];
  const skipped = [];
  for (const target of wanted) {
    if (!BUILDERS[target]) throw new Error(`unknown target "${target}" (known: ${Object.keys(BUILDERS).join(', ')})`);
    if (!which(TOOLS[target])) {
      if (explicit) throw new Error(`target ${target} needs ${TOOLS[target]}, which is not on PATH`);
      skipped.push(`${target} (${TOOLS[target]} not found)`);
      continue;
    }
    const workDir = path.join(BUILD_DIR, `package-${arch}`);
    fs.mkdirSync(workDir, { recursive: true });
    log('package', `building ${target}`);
    const out = await BUILDERS[target]({ arch, buildOut, buildInfo, workDir });
    log('package', `${target}: ${path.relative(REPO_ROOT, out)} (${(fs.statSync(out).size / 1048576).toFixed(1)} MiB)`);
    results.push({ target, file: out });
  }
  for (const s of skipped) log('package', `skipped ${s}`);
  if (results.length === 0) throw new Error('no package was built');
  return results;
}

module.exports = { packageTargets, stageSystemRoot, versions, rpmFileList };

if (require.main === module) {
  const args = common.parseArgs(process.argv.slice(2), { booleans: ['rebuild'] });
  const arch = typeof args.arch === 'string' ? args.arch : undefined;
  let job;
  if (typeof args['stage-root'] === 'string') {
    const out = path.join(BUILD_DIR, `linux-${common.resolveArch(arch)}`);
    job = Promise.resolve(stageSystemRoot(out, path.resolve(args['stage-root'])));
  } else {
    const targets = typeof args.targets === 'string' ? args.targets.split(',').map((t) => t.trim()).filter(Boolean) : undefined;
    job = packageTargets({ arch, targets, rebuild: Boolean(args.rebuild) });
  }
  job.catch((error) => {
    process.stderr.write(`package: ${error.stack ?? error.message}\n`);
    process.exitCode = 1;
  });
}
