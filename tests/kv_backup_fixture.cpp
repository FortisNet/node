#include "../src/storage/kv_store.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
  if (argc < 3 || argc > 4) return 2;
  // FORTIS_KV_DURABILITY=commit-slots opens the store in that mode.
  const char* durability = getenv("FORTIS_KV_DURABILITY");
  fortis::storage::KvStore store(false, false,
                        durability && strcmp(durability, "commit-slots") == 0
                            ? fortis::storage::KvDurability::CommitSlots
                            : fortis::storage::KvDurability::RollbackMarker);
  if (!store.Open(argv[2])) return 3;
  if (strcmp(argv[1], "hold") == 0) {
    puts("ready");
    fflush(stdout);
    for (;;) pause();
  }
  if (strcmp(argv[1], "write") == 0) {
    if (argc != 4) return 2;
    const fortis::storage::KvMutation mutation{"backup/test", argv[3],
        static_cast<uint32_t>(strlen(argv[3])), 0, false};
    return store.Write(&mutation, 1) ? 0 : 4;
  }
  if (strcmp(argv[1], "check") == 0) {
    if (argc != 4) return 2;
    char value[16]{};
    size_t length = 0;
    return store.Get("backup/test", 0, value, sizeof(value), &length) &&
                   length == strlen(argv[3]) &&
                   memcmp(value, argv[3], length) == 0 ? 0 : 5;
  }
  return 2;
}
