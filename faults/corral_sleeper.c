/* corral_sleeper TAG [S]: sleeps S seconds (default 300), default signal handling. */
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
  unsigned left = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 300;
  while (left > 0) left = sleep(left);
  return 0;
}
