/**
 * The ssh tunnel, which node had as plugin-tunnel-ssh, built in.
 *
 *   "tunnel": {"host": "bastion.example", "username": "deploy",
 *              "privateKeyPath": "/home/deploy/.ssh/id_ed25519"}
 *
 * It is the system's ssh, run as `ssh -N -L`: the user's ~/.ssh/config, the
 * agent and known_hosts apply, as they do to every other ssh on the machine -
 * where node's tunnel-ssh spoke the protocol itself and checked no host key.
 *
 * What node's config said, and what it is here:
 *
 *   host, port (22), username       where ssh logs in
 *   privateKeyPath, privateKey      -i, the second written to a file first
 *   password, passphrase            answered through SSH_ASKPASS
 *   localHost, localPort            the tunnel's end (a free port if none)
 *   keepaliveInterval (ms)          ServerAliveInterval
 *   readyTimeout (ms, 20000)        how long it may take to come up
 *   options: {"Key": "value"}       anything else, as -o Key=value
 *
 * DBM_SSH names another ssh than the one on the PATH.
 *
 * Without a password or a passphrase ssh runs in batch mode, so it fails
 * instead of waiting at a prompt nobody sees.
 */
#include <db_migrate_plugin.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

typedef struct {
  pid_t pid;
  char dir[256];
} ssh_tunnel_t;

/** A port nobody listens on right now, from the kernel. */
static long freePort(const char *host) {

  struct sockaddr_in address = {0};
  socklen_t length = sizeof address;
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  long port = -1;

  address.sin_family = AF_INET;
  inet_pton(AF_INET, host, &address.sin_addr);

  if (sock >= 0 && bind(sock, (struct sockaddr *)&address, sizeof address) == 0 &&
      getsockname(sock, (struct sockaddr *)&address, &length) == 0)
    port = ntohs(address.sin_port);

  if (sock >= 0)
    close(sock);

  return port;
}

static bool answers(const char *host, long port) {

  struct sockaddr_in address = {0};
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  bool reached;

  address.sin_family = AF_INET;
  address.sin_port = htons((uint16_t)port);
  inet_pton(AF_INET, host, &address.sin_addr);

  reached = sock >= 0 &&
         connect(sock, (struct sockaddr *)&address, sizeof address) == 0;

  if (sock >= 0)
    close(sock);

  return reached;
}

static bool writeFile(const char *path, const char *text, mode_t mode) {

  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, mode);

  if (fd < 0)
    return false;

  size_t length = strlen(text);
  bool ok = write(fd, text, length) == (ssize_t)length;

  close(fd);
  return ok;
}

static void removeAll(ssh_tunnel_t *tunnel) {

  char path[512];

  static const char *const names[] = {"key", "askpass", "log"};

  for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
    dbmWrite(path, sizeof path, TEXT`${tunnel->dir}/${names[i]}`);
    unlink(path);
  }

  rmdir(tunnel->dir);
}

/** What ssh said before it gave up, for the reason. */
static void lastWords(ssh_tunnel_t *tunnel, char *why, size_t room) {

  char path[512];
  char said[400] = "";

  dbmWrite(path, sizeof path, TEXT`${tunnel->dir}/log`);

  FILE *log = fopen(path, "r");

  if (log != NULL) {
    size_t got = fread(said, 1, sizeof said - 1, log);

    said[got] = '\0';
    fclose(log);
  }

  while (said[0] != '\0' && strchr("\r\n ", said[strlen(said) - 1]) != NULL)
    said[strlen(said) - 1] = '\0';

  dbmWrite(why, room, TEXT`the ssh tunnel did not open${said[0] != '\0' ? ": " : ""}${said}`);
}

static void closeSsh(void *handle) {

  ssh_tunnel_t *tunnel = handle;

  if (tunnel == NULL)
    return;

  kill(tunnel->pid, SIGTERM);
  waitpid(tunnel->pid, NULL, 0);
  removeAll(tunnel);
  free(tunnel);
}

/** The arguments, kept until exec: each one copied, the list NULL-ended. */
typedef struct {
  char *items[64];
  int count;
} ssh_args_t;

static void add(ssh_args_t *args, const char *word) {
  if (args->count < 63)
    args->items[args->count++] = strdup(word);
}

static void addOption(ssh_args_t *args, text_t option) {

  char line[512];

  dbmWrite(line, sizeof line, option);
  add(args, "-o");
  add(args, line);
}

static void releaseArgs(ssh_args_t *args) {
  for (int i = 0; i < args->count; ++i)
    free(args->items[i]);
}

static long openSsh(json_t config, const char *host, long port, void **handle,
                    char *why, size_t room) {

  const char *bastion = config.host;
  const char *user = config.username;
  const char *keyPath = config.privateKeyPath;
  const char *key = config.privateKey;
  const char *password = config.password;
  const char *passphrase = config.passphrase;
  const char *localHost = config.localHost;
  long localPort = config.localPort.number();
  long sshPort = config.port.isNothing() ? 22 : config.port.number();
  long keepalive = config.keepaliveInterval.number();
  long timeout = config.readyTimeout.isNothing() ? 20000
                                                 : config.readyTimeout.number();
  const char *tmp = getenv("TMPDIR");
  char path[512];

  if (bastion[0] == '\0') {
    dbmWrite(why, room, TEXT`the ssh tunnel needs the host to log in on: "tunnel": {"host": ...}`);
    return -1;
  }

  if (localHost[0] == '\0')
    localHost = "127.0.0.1";

  if (localPort <= 0 && (localPort = freePort(localHost)) < 0) {
    dbmWrite(why, room, TEXT`no free port on ${localHost} for the tunnel: ${strerror(errno)}`);
    return -1;
  }

  ssh_tunnel_t *tunnel = calloc(1, sizeof *tunnel);

  dbmWrite(tunnel->dir, sizeof tunnel->dir,
           TEXT`${tmp != NULL && tmp[0] != '\0' ? tmp : "/tmp"}/dbm-ssh-XXXXXX`);

  if (mkdtemp(tunnel->dir) == NULL) {
    dbmWrite(why, room, TEXT`cannot make a directory for the tunnel: ${strerror(errno)}`);
    free(tunnel);
    return -1;
  }

  ssh_args_t args = {0};
  bool secret = password[0] != '\0' || passphrase[0] != '\0';
  char forward[512];

  const char *ssh = getenv("DBM_SSH");

  add(&args, ssh != NULL && ssh[0] != '\0' ? ssh : "ssh");
  add(&args, "-N");
  addOption(&args, TEXT`ExitOnForwardFailure=yes`);
  addOption(&args, TEXT`BatchMode=${secret ? "no" : "yes"}`);

  if (sshPort != 22) {
    char number[32];

    dbmWrite(number, sizeof number, TEXT`${sshPort}`);
    add(&args, "-p");
    add(&args, number);
  }

  if (key[0] != '\0') {
    dbmWrite(path, sizeof path, TEXT`${tunnel->dir}/key`);

    if (!writeFile(path, key, 0600)) {
      dbmWrite(why, room, TEXT`cannot write the tunnel's key: ${strerror(errno)}`);
      releaseArgs(&args);
      removeAll(tunnel);
      free(tunnel);
      return -1;
    }

    keyPath = path;
  }

  if (keyPath[0] != '\0') {
    add(&args, "-i");
    add(&args, keyPath);
    addOption(&args, TEXT`IdentitiesOnly=yes`);
  }

  if (keepalive > 0)
    addOption(&args, TEXT`ServerAliveInterval=${(keepalive + 999) / 1000}`);

  json_t options = config.options;

  for (int i = 0; i < options.count(); ++i) {

    json_t value = options.get(options.keyAt(i));

    if (strcmp(value.kind(), "string") == 0)
      addOption(&args, TEXT`${options.keyAt(i)}=${value.text()}`);
    else
      addOption(&args, TEXT`${options.keyAt(i)}=${value.number()}`);
  }

  dbmWrite(forward, sizeof forward, TEXT`${localHost}:${localPort}:${host}:${port}`);
  add(&args, "-L");
  add(&args, forward);

  if (user[0] != '\0') {
    add(&args, "-l");
    add(&args, user);
  }

  add(&args, bastion);
  args.items[args.count] = NULL;

  /**
   * A password or a passphrase is handed to ssh by a script it runs to ask,
   * which reads it from the environment - never from the command line, where
   * every process on the machine could read it.
   */
  char askpass[512];

  dbmWrite(askpass, sizeof askpass, TEXT`${tunnel->dir}/askpass`);

  if (secret &&
      !writeFile(askpass,
                 "#!/bin/sh\ncase \"$1\" in\n"
                 "  *assphrase*) printf '%s\\n' \"$DBM_SSH_PASSPHRASE\" ;;\n"
                 "  *) printf '%s\\n' \"$DBM_SSH_PASSWORD\" ;;\nesac\n",
                 0700)) {
    dbmWrite(why, room, TEXT`cannot write the tunnel's askpass: ${strerror(errno)}`);
    releaseArgs(&args);
    removeAll(tunnel);
    free(tunnel);
    return -1;
  }

  char log[512];

  dbmWrite(log, sizeof log, TEXT`${tunnel->dir}/log`);

  pid_t pid = fork();

  if (pid == 0) {

    int null = open("/dev/null", O_RDONLY);
    int out = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    dup2(null, 0);
    dup2(out, 1);
    dup2(out, 2);

    /* away from the terminal, so ssh asks the script and not the user */
    setsid();

#ifdef __linux__
    /* and gone when this process is, however it ends */
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif

    if (secret) {
      setenv("SSH_ASKPASS", askpass, 1);
      setenv("SSH_ASKPASS_REQUIRE", "force", 1);
      setenv("DISPLAY", ":0", 0);
      setenv("DBM_SSH_PASSWORD", password, 1);
      setenv("DBM_SSH_PASSPHRASE", passphrase, 1);
    }

    execvp(args.items[0], args.items);
    dprintf(2, "cannot run %s: %s\n", args.items[0], strerror(errno));
    _exit(127);
  }

  releaseArgs(&args);

  if (pid < 0) {
    dbmWrite(why, room, TEXT`cannot start ssh: ${strerror(errno)}`);
    removeAll(tunnel);
    free(tunnel);
    return -1;
  }

  tunnel->pid = pid;

  /* up when its end answers; down if ssh is gone first, or takes too long */
  struct timespec start;
  struct timespec now;
  struct timespec pause = {0, 50 * 1000 * 1000};

  clock_gettime(CLOCK_MONOTONIC, &start);

  for (;;) {

    int status;

    if (waitpid(pid, &status, WNOHANG) == pid) {
      lastWords(tunnel, why, room);
      removeAll(tunnel);
      free(tunnel);
      return -1;
    }

    if (answers(localHost, localPort))
      break;

    clock_gettime(CLOCK_MONOTONIC, &now);

    long waited = (now.tv_sec - start.tv_sec) * 1000 +
                  (now.tv_nsec - start.tv_nsec) / 1000000;

    if (waited >= timeout) {
      lastWords(tunnel, why, room);
      dbm_text_t more = {0};
      defer more.release();
      more.append(TEXT` (no answer on ${localHost}:${localPort} after ${timeout} ms)`);
      strncat(why, more.text, room - strlen(why) - 1);
      closeSsh(tunnel);
      return -1;
    }

    nanosleep(&pause, NULL);
  }

  if (getenv("DBM_DEBUG"))
    dbmSay(stderr, TEXT`[DEBUG] ssh tunnel ${localHost}:${localPort} -> ${host}:${port} through ${bastion}\n`);

  *handle = tunnel;
  return localPort;
}

__attribute__((constructor)) static void registerSsh(void) {
  dbmRegisterTunnel("ssh", openSsh, closeSsh);
}
