/* corral_daemonizer_stay TAG (F5): as F4, but the launcher then sleeps 300 s. */
#include <limits.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Execs corral_sleeper from this binary's directory, keeping TAG in its cmdline. */
static void exec_sleeper(const char *tag) {
  static const char name[] = "/corral_sleeper";
  char path[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
  if (n < 0) _exit(1);
  path[n] = '\0';
  char *slash = strrchr(path, '/');
  if (slash == NULL || (size_t)(slash - path) + sizeof name > sizeof path) _exit(1);
  memcpy(slash, name, sizeof name);
  execl(path, path, tag, "300", (char *)NULL);
  _exit(1);
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  pid_t child = fork();
  if (child < 0) return 1;
  if (child == 0) {
    if (setsid() < 0) _exit(1);
    pid_t grandchild = fork();
    if (grandchild < 0) _exit(1);
    if (grandchild == 0) exec_sleeper(argv[1]);
    _exit(0);
  }
  /* The grandchild is orphaned before this process goes on. */
  if (waitpid(child, NULL, 0) < 0) return 1;
  unsigned left = 300;
  while (left > 0) left = sleep(left);
  return 0;
}
