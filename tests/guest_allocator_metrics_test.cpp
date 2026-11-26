#include "guest_allocator_metrics.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

using namespace fortis::operations;

namespace {

const char* const kNames[] = {
    "fortis_guest_allocator_allocated_bytes",
    "fortis_guest_allocator_allocated_peak_bytes",
    "fortis_guest_allocator_available_bytes",
    "fortis_guest_allocator_failures_total",
};

size_t render(const FortisGuestAllocatorStats& sample, char* output, size_t capacity) {
  size_t used = 0;
  output[0] = '\0';
  assert(AppendGuestAllocatorMetrics(sample, output, capacity, &used));
  assert(used == strlen(output));
  return used;
}

// Every non-comment line is "<known name> <value>" with no labels, and every
// emitted series has exactly one HELP and one TYPE line before it.
void check_shape(const char* text, bool expect_available) {
  for (const char* name : kNames) {
    const bool expected = expect_available ||
        strcmp(name, "fortis_guest_allocator_available_bytes") != 0;
    char help[160], type[160], sample[160];
    snprintf(help, sizeof(help), "# HELP %s ", name);
    snprintf(type, sizeof(type), "# TYPE %s ", name);
    snprintf(sample, sizeof(sample), "\n%s ", name);
    assert((strstr(text, help) != nullptr) == expected);
    assert((strstr(text, type) != nullptr) == expected);
    assert((strstr(text, sample) != nullptr) == expected);
    if (expected) {
      const char* first = strstr(text, help);
      assert(!strstr(first + 1, help));
    }
  }
  assert(text[0] == '#' && text[strlen(text) - 1] == '\n');
  assert(!strchr(text, '{') && !strchr(text, '}'));
  for (const char* line = text; *line;) {
    const char* end = strchr(line, '\n');
    assert(end);
    if (*line != '#') {
      bool known = false;
      for (const char* name : kNames) {
        const size_t length = strlen(name);
        if (strncmp(line, name, length) == 0 && line[length] == ' ') known = true;
      }
      assert(known);
    }
    line = end + 1;
  }
}

}  // namespace

int main() {
  // Host builds have no Unikraft allocator: nothing is collected or invented.
  FortisGuestAllocatorStats untouched{1, 2, 3, 4};
  assert(!CollectGuestAllocatorSample(&untouched));
  assert(untouched.allocated_bytes == 1 && untouched.allocated_peak_bytes == 2 &&
         untouched.available_bytes == 3 && untouched.failures == 4);
  assert(!CollectGuestAllocatorSample(nullptr));

  char text[kGuestAllocatorMetricsMaxBytes + 64];
  const FortisGuestAllocatorStats sample{9007199254740993LL, 9007199254740995LL,
                                         123456789, 7};
  render(sample, text, sizeof(text));
  check_shape(text, true);
  assert(strstr(text, "\nfortis_guest_allocator_allocated_bytes 9007199254740993\n"));
  assert(strstr(text, "\nfortis_guest_allocator_allocated_peak_bytes 9007199254740995\n"));
  assert(strstr(text, "\nfortis_guest_allocator_available_bytes 123456789\n"));
  assert(strstr(text, "\nfortis_guest_allocator_failures_total 7\n"));
  assert(strstr(text, "# TYPE fortis_guest_allocator_allocated_bytes gauge\n"));
  assert(strstr(text, "# TYPE fortis_guest_allocator_allocated_peak_bytes gauge\n"));
  assert(strstr(text, "# TYPE fortis_guest_allocator_available_bytes gauge\n"));
  assert(strstr(text, "# TYPE fortis_guest_allocator_failures_total counter\n"));
  // Help text must say what the figures are not.
  assert(strstr(text, "Allocator figure only: not guest RSS, kernel or static data, page tables, "
                      "or host/QEMU memory."));
  assert(strstr(text, "not peak guest RSS or host memory"));
  assert(strstr(text, "not host free memory"));
  assert(strstr(text, "tracked by the Unikraft default allocator's statistics"));
  assert(strstr(text, "Memory from any other allocator, or not tracked by these counters, is omitted."));
  assert(!strstr(text, "every library") && !strstr(text, "thread stacks"));

  // An allocator without availmem omits only that gauge.
  const FortisGuestAllocatorStats no_available{4096, 8192, -1, 0};
  render(no_available, text, sizeof(text));
  check_shape(text, false);
  assert(strstr(text, "\nfortis_guest_allocator_allocated_bytes 4096\n"));
  assert(strstr(text, "\nfortis_guest_allocator_failures_total 0\n"));

  // Worst-case widths stay inside the declared bound.
  const FortisGuestAllocatorStats widest{INT64_MIN, INT64_MIN, INT64_MAX, UINT64_MAX};
  const size_t widest_length = render(widest, text, sizeof(text));
  assert(widest_length < kGuestAllocatorMetricsMaxBytes);
  assert(strstr(text, "\nfortis_guest_allocator_failures_total 18446744073709551615\n"));
  assert(strstr(text, "\nfortis_guest_allocator_allocated_bytes -9223372036854775808\n"));

  // Appends after existing route output without disturbing it.
  const char prefix[] = "fortis_native_http_requests_total{route=\"other\"} 1\n";
  char combined[sizeof(prefix) + kGuestAllocatorMetricsMaxBytes];
  memcpy(combined, prefix, sizeof(prefix));
  size_t used = sizeof(prefix) - 1;
  assert(AppendGuestAllocatorMetrics(sample, combined, sizeof(combined), &used));
  assert(strncmp(combined, prefix, sizeof(prefix) - 1) == 0);
  assert(used == strlen(combined));
  check_shape(combined + sizeof(prefix) - 1, true);

  // Every short capacity is refused atomically: no partial block, no change to
  // *used, and no byte written past the existing NUL terminator.
  const size_t block = render(sample, text, sizeof(text));
  for (size_t capacity = 0; capacity <= 8 + block + 1; ++capacity) {
    char buffer[8 + kGuestAllocatorMetricsMaxBytes + 1];
    memset(buffer, 'X', sizeof(buffer));
    memcpy(buffer, "abcdefg", 8);
    size_t at = 7;
    const bool ok = AppendGuestAllocatorMetrics(sample, buffer, capacity, &at);
    if (capacity > 7 + block) {
      assert(ok && at == 7 + block && strcmp(buffer + 7, text) == 0);
    } else {
      assert(!ok && at == 7);
      assert(capacity <= 7 || buffer[7] == '\0');
      for (size_t i = 8; i < sizeof(buffer); ++i) assert(buffer[i] == 'X');
    }
  }
  size_t zero = 0;
  assert(!AppendGuestAllocatorMetrics(sample, nullptr, 16, &zero));
  assert(!AppendGuestAllocatorMetrics(sample, text, sizeof(text), nullptr));
  size_t full = sizeof(text);
  assert(!AppendGuestAllocatorMetrics(sample, text, sizeof(text), &full));
  puts("guest allocator metrics formatting, omission, and output bounds passed");
}
