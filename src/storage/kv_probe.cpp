#include "ownership.h"
#include "kv_probe.h"

#include "kv_store.h"
#include "kv_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
namespace fortis::storage {

namespace {
constexpr char kCounter[] = "probe/counter";
bool parse(const char* text, size_t length, uint64_t* number) {
  if (!length || length > 20) return false;
  uint64_t result = 0;
  for (size_t i = 0; i < length; ++i) {
    const char digit = text[i];
    if (digit < '0' || digit > '9' ||
        result > (UINT64_MAX - static_cast<unsigned>(digit - '0')) / 10) return false;
    result = result * 10 + static_cast<unsigned>(digit - '0');
  }
  *number = result;
  return true;
}
}  // namespace

bool kv_probe_read(uint64_t* value) {
  auto* store = fortis::storage::RuntimeKvStore();
  if (!value || !store) return false;
  char text[32];
  size_t length = 0;
  const auto status = store->GetStatus(kCounter, 0, text, sizeof(text), &length);
  if (status == fortis::storage::KvReadStatus::Missing) {
    *value = 0;
    return true;
  }
  if (status != fortis::storage::KvReadStatus::Found) return false;
  return parse(text, length, value);
}

bool kv_probe_increment(uint64_t* value) {
  auto* store = fortis::storage::RuntimeKvStore();
  if (!value || !store) return false;
  uint64_t current = 0;
  if (!kv_probe_read(&current)) return false;
  if (current == UINT64_MAX) return false;
  *value = current + 1;
  char text[32];
  const int written = snprintf(text, sizeof(text), "%llu",
                               static_cast<unsigned long long>(*value));
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(text)) return false;
  const fortis::storage::KvMutation mutation{kCounter, text, static_cast<uint32_t>(written), 0, false};
  return store->Write(&mutation, 1);
}

bool kv_probe_healthy() {
  auto* store = fortis::storage::RuntimeKvStore();
  if (!store) return false;
  char value[1];
  size_t length = 0;
  auto status = store->GetStatus("health/check", 0, value, sizeof(value), &length);
  if (status == fortis::storage::KvReadStatus::BufferTooSmall) {
    auto large = base::Buffer::Allocate(length);
    if (!large) return false;
    status = store->GetStatus("health/check", 0, large.data(), length, &length);
    large.reset();
  }
  return status == fortis::storage::KvReadStatus::Missing ||
         status == fortis::storage::KvReadStatus::Found;
}

}  // namespace fortis::storage
