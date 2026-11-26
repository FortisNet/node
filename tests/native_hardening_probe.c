#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// Deliberately unsafe child-only probes. Never linked into fortis-node.
__attribute__((noinline)) static int stack_probe(size_t n) {
  volatile unsigned char bytes[32];
  for (size_t i = 0; i < n; ++i) bytes[i] = 'x';
  __asm__ volatile("" : : "r"(bytes) : "memory");
  return bytes[0];
}
__attribute__((noinline, no_stack_protector)) static int fortify_probe(const char* text) {
  char bytes[8];
  memcpy(bytes, text, strlen(text) + 1);
  __asm__ volatile("" : : "r"(bytes) : "memory");
  return bytes[0];
}
__attribute__((noinline)) int clash_probe(size_t n) {
  volatile char bytes[32768];
  bytes[0] = 1;
  bytes[n & 32767] = 2;
  return bytes[0];
}
int main(int argc, char** argv) {
  if (argc != 3) return 0;
  if (!strcmp(argv[1], "stack")) return stack_probe((size_t)strtoul(argv[2], NULL, 10));
  if (!strcmp(argv[1], "fortify")) return fortify_probe(argv[2]);
  return clash_probe(1);
}
