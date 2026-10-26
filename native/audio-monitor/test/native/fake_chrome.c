// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Test stand-in for a Chromium browser process tree (built as "chrome", so
// its executable and comm are those of a real Chrome browser process):
//   chrome udp  <source> <file>  binds an unconnected UDP socket, as WebRTC
//                                ICE candidate sockets are;
//   chrome none <source> <file>  no UDP socket (microphone without a call).
// The microphone capture runs in a child process (pw-record with Chrome's
// stream identity), as Chrome's audio service does. Prints
// {"browserPid":N,"recPid":M} once ready and stops on SIGTERM/SIGINT.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t stopRequested = 0;

static void onSignal(int sig) {
  (void)sig;
  stopRequested = 1;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: chrome <udp|none> <source node> <output file>\n");
    return 2;
  }
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = onSignal;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  int sock = -1;
  if (strcmp(argv[1], "udp") == 0) {
    sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    if (sock < 0 || bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
      perror("udp socket");
      return 1;
    }
  }

  pid_t rec = fork();
  if (rec < 0) {
    perror("fork");
    return 1;
  }
  if (rec == 0) {
    execlp("pw-record", "pw-record", "--target", argv[2], "-P",
           "{ application.name=\"Google Chrome\" application.process.binary=\"chrome\" }", argv[3], (char*)NULL);
    _exit(127);
  }
  printf("{\"browserPid\":%d,\"recPid\":%d}\n", (int)getpid(), (int)rec);
  fflush(stdout);
  while (!stopRequested) pause();
  kill(rec, SIGTERM);
  waitpid(rec, NULL, 0);
  if (sock >= 0) close(sock);
  return 0;
}
