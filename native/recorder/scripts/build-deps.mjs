#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Fetches and builds the vendored, statically linked dependencies of the
// Linux recorder addon:
//   * webrtc-audio-processing 2.1 (the AEC/NS/AGC library Plaud ships as
//     libwebrtc-audio-processing-2.1.dylib / webrtc-audio-processing-2-1.dll),
//   * abseil-cpp 20240722.0 (its meson subproject),
//   * meson 1.5.2, only when the host meson is missing or older than 0.63
//     (Ubuntu 22.04 ships 0.61, which cannot configure webrtc-audio-processing 2.x).
// Every download is pinned by sha256. Outputs go to <module>/deps/:
//   deps/prefix/include/...      installed headers
//   deps/webrtc-apm.json         static libraries + compile flags for CMake
// Environment:
//   PLAUD_DEPS_DIR    output directory (default <module>/deps)
//   PLAUD_DEPS_CACHE  download cache directory (default $PLAUD_DEPS_DIR/cache);
//                     pre-populate it for offline builds
//   PLAUD_DEPS_OFFLINE=1  never download, fail if a cached file is missing
//   MESON, NINJA      explicit tool paths

import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const moduleDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const depsDir = path.resolve(process.env.PLAUD_DEPS_DIR ?? path.join(moduleDir, "deps"));
const cacheDir = path.resolve(process.env.PLAUD_DEPS_CACHE ?? path.join(depsDir, "cache"));
const offline = process.env.PLAUD_DEPS_OFFLINE === "1";

const WEBRTC = {
  name: "webrtc-audio-processing-2.1.tar.xz",
  dir: "webrtc-audio-processing-2.1",
  sha256: "ae9302824b2038d394f10213cab05312c564a038434269f11dbf68f511f9f9fe",
  urls: [
    "https://freedesktop.org/software/pulseaudio/webrtc-audio-processing/webrtc-audio-processing-2.1.tar.xz",
  ],
};
// Values copied from subprojects/abseil-cpp.wrap inside the webrtc tarball;
// they are re-checked against that file after extraction.
const ABSEIL_SOURCE = {
  name: "abseil-cpp-20240722.0.tar.gz",
  sha256: "f50e5ac311a81382da7fa75b97310e4b9006474f9560ac46f54a9967f07d4ae3",
  urls: [
    "https://github.com/abseil/abseil-cpp/releases/download/20240722.0/abseil-cpp-20240722.0.tar.gz",
    "https://github.com/mesonbuild/wrapdb/releases/download/abseil-cpp_20240722.0-3/abseil-cpp-20240722.0.tar.gz",
  ],
};
const ABSEIL_PATCH = {
  name: "abseil-cpp_20240722.0-3_patch.zip",
  sha256: "12dd8df1488a314c53e3751abd2750cf233b830651d168b6a9f15e7d0cf71f7b",
  urls: [
    "https://wrapdb.mesonbuild.com/v2/abseil-cpp_20240722.0-3/get_patch",
    "https://github.com/mesonbuild/wrapdb/releases/download/abseil-cpp_20240722.0-3/abseil-cpp_20240722.0-3_patch.zip",
  ],
};
const MESON = {
  name: "meson-1.5.2.tar.gz",
  dir: "meson-1.5.2",
  sha256: "f955e09ab0d71ef180ae85df65991d58ed8430323de7d77a37e11c9ea630910b",
  urls: [
    "https://files.pythonhosted.org/packages/34/e8/bb0e264882a42f5d5acae869d5980f298ff9c298d844e18fd1ac009ce7e9/meson-1.5.2.tar.gz",
  ],
};
const ABSL_DEPS = [
  "absl_base",
  "absl_flags",
  "absl_strings",
  "absl_numeric",
  "absl_synchronization",
  "absl_bad_optional_access",
];
const MESON_OPTIONS = [
  "--libdir=lib",
  "--buildtype=release",
  "--default-library=static",
  "-Db_staticpic=true",
  "-Db_ndebug=true",
  "--wrap-mode=nodownload",
  `--force-fallback-for=${ABSL_DEPS.join(",")}`,
  // GCC >= 15 no longer pulls <cstdint> in transitively; webrtc 2.1 relies on
  // it (rtc_base/trace_event.h). Force-including it is harmless elsewhere.
  "-Dcpp_args=-include cstdint -fvisibility=hidden -fvisibility-inlines-hidden -ffunction-sections -fdata-sections",
  "-Dc_args=-fvisibility=hidden -ffunction-sections -fdata-sections",
];

function log(msg) {
  process.stderr.write(`[recorder deps] ${msg}\n`);
}

function fail(msg) {
  process.stderr.write(`[recorder deps] ERROR: ${msg}\n`);
  process.exit(1);
}

function run(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { stdio: opts.capture ? ["ignore", "pipe", "pipe"] : "inherit", encoding: "utf8", ...opts });
  if (r.error) fail(`${cmd}: ${r.error.message}`);
  if (r.status !== 0) {
    if (opts.capture) process.stderr.write(`${r.stdout ?? ""}${r.stderr ?? ""}`);
    fail(`${cmd} ${args.join(" ")} exited with ${r.status}`);
  }
  return r.stdout ?? "";
}

function sha256File(file) {
  return createHash("sha256").update(fs.readFileSync(file)).digest("hex");
}

async function fetchPinned(spec) {
  fs.mkdirSync(cacheDir, { recursive: true });
  const dest = path.join(cacheDir, spec.name);
  if (fs.existsSync(dest)) {
    if (sha256File(dest) === spec.sha256) return dest;
    log(`cached ${spec.name} has a wrong sha256, discarding`);
    fs.rmSync(dest);
  }
  if (offline) fail(`${spec.name} is not in ${cacheDir} and PLAUD_DEPS_OFFLINE=1`);
  let lastError = "";
  for (const url of spec.urls) {
    try {
      log(`downloading ${url}`);
      const res = await fetch(url, { redirect: "follow" });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const buf = Buffer.from(await res.arrayBuffer());
      const got = createHash("sha256").update(buf).digest("hex");
      if (got !== spec.sha256) throw new Error(`sha256 mismatch: got ${got}, want ${spec.sha256}`);
      const tmp = `${dest}.part-${process.pid}`;
      fs.writeFileSync(tmp, buf);
      fs.renameSync(tmp, dest);
      return dest;
    } catch (err) {
      lastError = `${url}: ${err.message}`;
      log(`download failed: ${lastError}`);
    }
  }
  fail(`could not fetch ${spec.name} (${lastError})`);
}

function extractTar(archive, intoDir, topDir) {
  const target = path.join(intoDir, topDir);
  fs.rmSync(target, { recursive: true, force: true });
  fs.mkdirSync(intoDir, { recursive: true });
  run("tar", ["-xf", archive, "-C", intoDir]);
  if (!fs.existsSync(target)) fail(`${archive} did not contain ${topDir}/`);
  return target;
}

function versionAtLeast(v, min) {
  const a = v.split(".").map((x) => parseInt(x, 10) || 0);
  const b = min.split(".").map((x) => parseInt(x, 10) || 0);
  for (let i = 0; i < Math.max(a.length, b.length); i++) {
    if ((a[i] ?? 0) !== (b[i] ?? 0)) return (a[i] ?? 0) > (b[i] ?? 0);
  }
  return true;
}

function which(cmd) {
  for (const dir of (process.env.PATH ?? "").split(path.delimiter)) {
    const p = path.join(dir, cmd);
    try {
      fs.accessSync(p, fs.constants.X_OK);
      return p;
    } catch {
      // keep searching
    }
  }
  return null;
}

async function resolveMeson() {
  const candidates = [];
  if (process.env.MESON) candidates.push([process.env.MESON]);
  const sys = which("meson");
  if (sys) candidates.push([sys]);
  for (const cmd of candidates) {
    const r = spawnSync(cmd[0], [...cmd.slice(1), "--version"], { encoding: "utf8" });
    if (r.status === 0 && versionAtLeast(r.stdout.trim(), "0.63.0")) return cmd;
    if (r.status === 0) log(`${cmd[0]} is version ${r.stdout.trim()}, need >= 0.63; using pinned meson ${MESON.dir}`);
  }
  const python = which("python3");
  if (!python) fail("python3 is required to run meson");
  const archive = await fetchPinned(MESON);
  const toolsDir = path.join(depsDir, "tools");
  const mesonDir = path.join(toolsDir, MESON.dir);
  if (!fs.existsSync(path.join(mesonDir, "meson.py"))) extractTar(archive, toolsDir, MESON.dir);
  return [python, path.join(mesonDir, "meson.py")];
}

function resolveNinja() {
  const ninja = process.env.NINJA ?? which("ninja") ?? which("ninja-build");
  if (!ninja) fail("ninja (or ninja-build) is required");
  return ninja;
}

function collectStaticLibs(buildDir) {
  // Static archives produced by the build, excluding examples. Order matters
  // only for single-pass linkers; CMake wraps them in --start-group/--end-group.
  const out = [];
  const walk = (dir) => {
    for (const ent of fs.readdirSync(dir, { withFileTypes: true })) {
      const p = path.join(dir, ent.name);
      if (ent.isDirectory()) {
        if (ent.name === "examples" || ent.name === "meson-private" || ent.name === "meson-logs") continue;
        walk(p);
      } else if (ent.isFile() && ent.name.endsWith(".a")) {
        out.push(p);
      }
    }
  };
  walk(buildDir);
  out.sort((a, b) => {
    // libwebrtc-audio-processing-2.a first, then its private parts, then abseil.
    const rank = (f) => (path.basename(f).startsWith("libwebrtc-audio-processing") ? 0 : f.includes("abseil") ? 2 : 1);
    return rank(a) - rank(b) || a.localeCompare(b);
  });
  return out;
}

async function main() {
  const meson = await resolveMeson();
  const ninja = resolveNinja();
  const mesonVersion = spawnSync(meson[0], [...meson.slice(1), "--version"], { encoding: "utf8" }).stdout.trim();
  const cxx = process.env.CXX ?? "c++";
  const cc = process.env.CC ?? "cc";
  const cxxVersion = spawnSync(cxx, ["--version"], { encoding: "utf8" }).stdout?.split("\n")[0] ?? cxx;

  const configKey = createHash("sha256")
    .update(JSON.stringify({ WEBRTC, ABSEIL_SOURCE, ABSEIL_PATCH, MESON_OPTIONS, mesonVersion, cxxVersion, cc, cxx, arch: os.arch() }))
    .digest("hex");
  const stampFile = path.join(depsDir, "webrtc-apm.stamp");
  const manifestFile = path.join(depsDir, "webrtc-apm.json");
  if (fs.existsSync(stampFile) && fs.existsSync(manifestFile) && fs.readFileSync(stampFile, "utf8") === configKey) {
    const manifest = JSON.parse(fs.readFileSync(manifestFile, "utf8"));
    if (manifest.libs.every((l) => fs.existsSync(l))) {
      log("webrtc-audio-processing 2.1 is up to date");
      return;
    }
  }

  const srcRoot = path.join(depsDir, "src");
  const webrtcArchive = await fetchPinned(WEBRTC);
  const srcDir = extractTar(webrtcArchive, srcRoot, WEBRTC.dir);

  const wrap = fs.readFileSync(path.join(srcDir, "subprojects", "abseil-cpp.wrap"), "utf8");
  for (const [key, spec] of [["source_hash", ABSEIL_SOURCE], ["patch_hash", ABSEIL_PATCH]]) {
    const m = wrap.match(new RegExp(`^${key}\\s*=\\s*([0-9a-f]{64})`, "m"));
    if (!m || m[1] !== spec.sha256) fail(`abseil-cpp.wrap ${key} does not match the pinned ${spec.name}`);
  }
  const packageCache = path.join(srcDir, "subprojects", "packagecache");
  fs.mkdirSync(packageCache, { recursive: true });
  for (const spec of [ABSEIL_SOURCE, ABSEIL_PATCH]) {
    fs.copyFileSync(await fetchPinned(spec), path.join(packageCache, spec.name));
  }

  const buildDir = path.join(depsDir, "build");
  const prefix = path.join(depsDir, "prefix");
  fs.rmSync(buildDir, { recursive: true, force: true });
  fs.rmSync(prefix, { recursive: true, force: true });

  const env = { ...process.env, CC: cc, CXX: cxx, NINJA: ninja };
  const setupArgs = [
    ...meson.slice(1),
    "setup",
    buildDir,
    srcDir,
    `--prefix=${prefix}`,
    ...MESON_OPTIONS,
  ];
  log(`configuring with ${meson.join(" ")} (meson ${mesonVersion})`);
  run(meson[0], setupArgs, { env, capture: true });
  log("building webrtc-audio-processing 2.1 + abseil (static)");
  run(meson[0], [...meson.slice(1), "compile", "-C", buildDir, "-j", String(Math.max(1, os.availableParallelism?.() ?? os.cpus().length))], { env, capture: true });
  run(meson[0], [...meson.slice(1), "install", "-C", buildDir, "--quiet"], { env, capture: true });

  const includeDir = path.join(prefix, "include", "webrtc-audio-processing-2");
  if (!fs.existsSync(path.join(includeDir, "modules", "audio_processing", "include", "audio_processing.h"))) {
    fail(`audio_processing.h not installed under ${includeDir}`);
  }
  const libs = collectStaticLibs(buildDir);
  if (!libs.some((l) => path.basename(l).startsWith("libwebrtc-audio-processing-2"))) {
    fail("libwebrtc-audio-processing-2.a was not produced");
  }
  const pcFile = path.join(prefix, "lib", "pkgconfig", "webrtc-audio-processing-2.pc");
  const pc = fs.existsSync(pcFile) ? fs.readFileSync(pcFile, "utf8") : "";
  const cflagsLine = pc.match(/^Cflags:(.*)$/m)?.[1] ?? "";
  const defines = cflagsLine
    .split(/\s+/)
    .filter((f) => f.startsWith("-D"))
    .map((f) => f.slice(2));

  const manifest = {
    version: "2.1",
    abseil: "20240722.0",
    // absl headers are installed next to the webrtc include dir.
    includeDirs: [includeDir, path.join(prefix, "include")],
    defines,
    libs,
  };
  fs.writeFileSync(manifestFile, `${JSON.stringify(manifest, null, 2)}\n`);
  fs.writeFileSync(stampFile, configKey);
  log(`done: ${libs.length} static libraries, manifest ${manifestFile}`);
}

await main();
