// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

#include "spawn.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <set>

extern char** environ;

namespace plaud_rec {
namespace screen {

namespace {

constexpr size_t kMaxStderrBytes = 16 * 1024;

class OwnedFd {
 public:
  OwnedFd() = default;
  explicit OwnedFd(int fd) : fd_(fd) {}
  OwnedFd(const OwnedFd&) = delete;
  OwnedFd& operator=(const OwnedFd&) = delete;
  ~OwnedFd() { Reset(); }
  int Get() const { return fd_; }
  void Reset() {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
  }

 private:
  int fd_ = -1;
};

int64_t NowMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool IsExecutableFile(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(path.c_str(), X_OK) == 0;
}

std::string EnvName(const std::string& entry) {
  const size_t eq = entry.find('=');
  return eq == std::string::npos ? entry : entry.substr(0, eq);
}

}  // namespace

std::string ProcessResult::Describe() const {
  if (!started) return "spawn failed: " + spawn_error;
  std::string s;
  if (timed_out) {
    s = "timed out";
  } else if (output_truncated) {
    s = "output too large";
  } else if (term_signal != 0) {
    s = "killed by signal " + std::to_string(term_signal);
  } else {
    s = "exit code " + std::to_string(exit_code);
  }
  std::string e = err;
  while (!e.empty() && (e.back() == '\n' || e.back() == '\r' || e.back() == ' ')) e.pop_back();
  if (!e.empty()) s += ": " + e;
  return s;
}

std::string FindExecutable(const std::string& name) {
  if (name.empty()) return std::string();
  if (name.find('/') != std::string::npos) return IsExecutableFile(name) ? name : std::string();
  const char* env_path = std::getenv("PATH");
  const std::string path = (env_path != nullptr && *env_path != '\0') ? env_path : "/usr/local/bin:/usr/bin:/bin";
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find(':', start);
    if (end == std::string::npos) end = path.size();
    const std::string dir = path.substr(start, end - start);
    if (!dir.empty() && dir[0] == '/') {
      const std::string candidate = dir + "/" + name;
      if (IsExecutableFile(candidate)) return candidate;
    }
    start = end + 1;
  }
  return std::string();
}

ProcessResult RunProcess(const std::vector<std::string>& argv, const std::vector<std::string>& extra_env,
                         int timeout_ms, size_t max_stdout_bytes) {
  ProcessResult r;
  if (argv.empty()) {
    r.spawn_error = "empty argv";
    return r;
  }
  int out_pipe[2];
  int err_pipe[2];
  if (pipe2(out_pipe, O_CLOEXEC) != 0) {
    r.spawn_error = std::string("pipe2: ") + std::strerror(errno);
    return r;
  }
  OwnedFd out_r(out_pipe[0]);
  OwnedFd out_w(out_pipe[1]);
  if (pipe2(err_pipe, O_CLOEXEC) != 0) {
    r.spawn_error = std::string("pipe2: ") + std::strerror(errno);
    return r;
  }
  OwnedFd err_r(err_pipe[0]);
  OwnedFd err_w(err_pipe[1]);
  // Large captures (tens of MB) stream faster through a bigger pipe buffer.
  fcntl(out_r.Get(), F_SETPIPE_SZ, 1 << 20);

  std::set<std::string> overridden;
  for (const std::string& e : extra_env) overridden.insert(EnvName(e));
  std::vector<std::string> env_storage;
  for (char** e = environ; e != nullptr && *e != nullptr; e++) {
    const std::string entry(*e);
    if (overridden.count(EnvName(entry)) == 0) env_storage.push_back(entry);
  }
  for (const std::string& e : extra_env) env_storage.push_back(e);
  std::vector<char*> envp;
  envp.reserve(env_storage.size() + 1);
  for (std::string& e : env_storage) envp.push_back(&e[0]);
  envp.push_back(nullptr);
  std::vector<std::string> argv_storage(argv);
  std::vector<char*> argvp;
  argvp.reserve(argv_storage.size() + 1);
  for (std::string& a : argv_storage) argvp.push_back(&a[0]);
  argvp.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, out_w.Get(), 1);
  posix_spawn_file_actions_adddup2(&actions, err_w.Get(), 2);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  sigset_t mask;
  sigemptyset(&mask);
  posix_spawnattr_setsigmask(&attr, &mask);
  // Electron/Node ignore or handle several signals; give the helper defaults.
  sigset_t defaults;
  sigemptyset(&defaults);
  for (int sig : {SIGPIPE, SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGCHLD, SIGALRM, SIGUSR1, SIGUSR2, SIGTSTP, SIGTTIN,
                  SIGTTOU}) {
    sigaddset(&defaults, sig);
  }
  posix_spawnattr_setsigdefault(&attr, &defaults);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
  pid_t pid = -1;
  const int rc = posix_spawn(&pid, argvp[0], &actions, &attr, argvp.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  out_w.Reset();
  err_w.Reset();
  if (rc != 0) {
    r.spawn_error = std::string(argv[0]) + ": " + std::strerror(rc);
    return r;
  }
  r.started = true;
  fcntl(out_r.Get(), F_SETFL, fcntl(out_r.Get(), F_GETFL) | O_NONBLOCK);
  fcntl(err_r.Get(), F_SETFL, fcntl(err_r.Get(), F_GETFL) | O_NONBLOCK);

  const int64_t deadline = NowMs() + timeout_ms;
  bool out_open = true;
  bool err_open = true;
  char err_buf[4096];
  while (out_open || err_open) {
    const int64_t remaining = deadline - NowMs();
    if (remaining <= 0) {
      r.timed_out = true;
      break;
    }
    pollfd fds[2];
    nfds_t n = 0;
    if (out_open) fds[n++] = pollfd{out_r.Get(), POLLIN, 0};
    if (err_open) fds[n++] = pollfd{err_r.Get(), POLLIN, 0};
    const int pr = poll(fds, n, static_cast<int>(remaining));
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) continue;
    for (nfds_t i = 0; i < n; i++) {
      if (fds[i].revents == 0) continue;
      const bool is_out = fds[i].fd == out_r.Get();
      for (;;) {
        ssize_t k;
        if (is_out) {
          const size_t old = r.out.size();
          if (r.out.capacity() - old < 65536) r.out.reserve(std::max<size_t>(r.out.capacity() * 2, old + 65536));
          r.out.resize(old + 65536);
          k = read(fds[i].fd, r.out.data() + old, 65536);
          r.out.resize(old + static_cast<size_t>(k > 0 ? k : 0));
          if (k > 0 && r.out.size() > max_stdout_bytes) {
            r.output_truncated = true;
            break;
          }
        } else {
          k = read(fds[i].fd, err_buf, sizeof(err_buf));
          if (k > 0 && r.err.size() < kMaxStderrBytes) {
            r.err.append(err_buf, std::min<size_t>(static_cast<size_t>(k), kMaxStderrBytes - r.err.size()));
          }
        }
        if (k > 0) continue;
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        (is_out ? out_open : err_open) = false;  // EOF or hard error
        break;
      }
      if (r.output_truncated) break;
    }
    if (r.output_truncated) break;
  }

  if (r.timed_out || r.output_truncated) kill(pid, SIGKILL);
  int status = 0;
  for (;;) {
    const pid_t w = waitpid(pid, &status, (r.timed_out || r.output_truncated) ? 0 : WNOHANG);
    if (w == pid) {
      if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
      if (WIFSIGNALED(status)) r.term_signal = WTERMSIG(status);
      break;
    }
    if (w < 0) {
      if (errno == EINTR) continue;
      break;  // ECHILD: reaped elsewhere; exit status unknown
    }
    if (NowMs() >= deadline) {
      r.timed_out = true;
      kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      if (WIFSIGNALED(status)) r.term_signal = WTERMSIG(status);
      break;
    }
    const timespec pause{0, 2 * 1000 * 1000};
    nanosleep(&pause, nullptr);
  }
  return r;
}

}  // namespace screen
}  // namespace plaud_rec
