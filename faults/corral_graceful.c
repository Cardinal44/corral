/* corral_graceful TAG (F10): on SIGTERM writes "cleaning up\n" and exits 0;
 * otherwise sleeps. */
#include <signal.h>
#include <string.h>
#include <unistd.h>

static void on_term(int sig) {
  static const char msg[] = "cleaning up\n";
  (void)sig;
  ssize_t n = write(STDOUT_FILENO, msg, sizeof msg - 1);
  _exit(n == (ssize_t)(sizeof msg - 1) ? 0 : 1);
}

int main(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_term;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGTERM, &sa, NULL) < 0) return 1;
  for (;;) pause();
}
