/* corral_forker TAG (F7): forks a child every 10 ms, up to 200, each sleeping
 * 300 s without setsid(); the parent then sleeps. Fork failures (for example
 * at pids.max) are ignored. */
#include <time.h>
#include <unistd.h>

static void sleep_300(void) {
  unsigned left = 300;
  while (left > 0) left = sleep(left);
}

int main(void) {
  const struct timespec tick = {0, 10 * 1000 * 1000};
  for (int i = 0; i < 200; ++i) {
    if (fork() == 0) {
      sleep_300();
      _exit(0);
    }
    nanosleep(&tick, NULL);
  }
  sleep_300();
  return 0;
}
