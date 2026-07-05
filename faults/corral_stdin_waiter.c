/* corral_stdin_waiter TAG (F2): reads stdin until EOF, then exits 0, like an
 * interactive tool that waits for input. */
#include <errno.h>
#include <unistd.h>

int main(void) {
  char buf[4096];
  for (;;) {
    ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
    if (n == 0) return 0;
    if (n < 0 && errno != EINTR) return 1;
  }
}
