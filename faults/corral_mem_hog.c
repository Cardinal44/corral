/* corral_mem_hog TAG MB (F9): allocates MB mebibytes (default 256), touches
 * every page, then sleeps 300 s. */
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
  size_t size = (argc > 2 ? strtoul(argv[2], NULL, 10) : 256) << 20;
  long page = sysconf(_SC_PAGESIZE);
  /* volatile: the stores must reach memory, or the compiler may drop them
   * and the pages would never be faulted in. */
  volatile char *p = malloc(size);
  if (p == NULL || page <= 0) return 1;
  for (size_t off = 0; off < size; off += (size_t)page) p[off] = 1;
  unsigned left = 300;
  while (left > 0) left = sleep(left);
  return 0;
}
