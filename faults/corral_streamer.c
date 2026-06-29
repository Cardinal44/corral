/* corral_streamer TAG (F1): writes one 100-byte line to stdout every 100 ms
 * for ever, like `tail -f`. */
#include <stdio.h>
#include <time.h>
#include <unistd.h>

int main(void) {
  const struct timespec tick = {0, 100 * 1000 * 1000};
  char line[101];
  for (unsigned long n = 0;; ++n) {
    /* A fixed line length makes byte counts in the tests exact. */
    int len = snprintf(line, sizeof line, "%-99lu\n", n);
    if (write(STDOUT_FILENO, line, (size_t)len) != len) return 1;
    nanosleep(&tick, NULL);
  }
}
