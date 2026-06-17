/* corral_pipe_holder TAG (F8): a child keeps the inherited stdout open for 300 s
 * while the parent writes one line and exits. */
#include <unistd.h>

int main(void) {
  static const char msg[] = "hello\n";
  pid_t child = fork();
  if (child < 0) return 1;
  if (child == 0) {
    unsigned left = 300;
    while (left > 0) left = sleep(left);
    _exit(0);
  }
  if (write(STDOUT_FILENO, msg, sizeof msg - 1) != (ssize_t)(sizeof msg - 1)) return 1;
  return 0;
}
