/* corral_term_ignorer TAG (F6): ignores SIGTERM and sleeps forever. */
#include <signal.h>
#include <unistd.h>

int main(void) {
  if (signal(SIGTERM, SIG_IGN) == SIG_ERR) return 1;
  for (;;) pause();
}
