#include "repair_candidates.h"
#include "unit_test_support.h"

#include <limits>
#include <string>
#include <vector>

namespace {
using fortis::wallet::address::repair::Candidate;
using fortis::wallet::address::repair::RepairCandidates;
using fortis::testing::Require;

// Preserve the parent's raw-array representation as well as its algorithm.
struct ParentCandidate {
  char account_id[fortis::wallet::address::kAccountIdBytes + 1];
  char record_id[640];
  char network[96];
  char token_id[400];
  int64_t ts;
  char coingecko_id[200];
  bool has_coingecko;
  double price_tier;
};
static_assert(sizeof(ParentCandidate) == sizeof(Candidate));
static_assert(alignof(ParentCandidate) == alignof(Candidate));
static_assert(offsetof(ParentCandidate, ts) == offsetof(Candidate, ts));
static_assert(offsetof(ParentCandidate, price_tier) == offsetof(Candidate, price_tier));

// Frozen parent algorithm (baaa3872). Its account tie ordering is contractual.
int compare_candidates(const ParentCandidate& a, const ParentCandidate& b) {
  if (a.price_tier != b.price_tier) return a.price_tier < b.price_tier ? -1 : 1;
  if (a.ts != b.ts) return a.ts < b.ts ? -1 : 1;
  return strcmp(a.record_id, b.record_id);  // byte order
}

void heap_sift_up(ParentCandidate* rows, size_t i) {
  while (i > 0) {
    const size_t parent = (i - 1) / 2;
    if (compare_candidates(rows[parent], rows[i]) >= 0) break;
    ParentCandidate swap = rows[parent];
    rows[parent] = rows[i];
    rows[i] = swap;
    i = parent;
  }
}

void heap_sift_down(ParentCandidate* rows, size_t count, size_t i) {
  for (;;) {
    const size_t left = i * 2 + 1;
    if (left >= count) break;
    const size_t right = left + 1;
    const size_t child = right < count && compare_candidates(rows[right], rows[left]) > 0 ? right : left;
    if (compare_candidates(rows[i], rows[child]) >= 0) break;
    ParentCandidate swap = rows[i];
    rows[i] = rows[child];
    rows[child] = swap;
    i = child;
  }
}


void compare_with_parent() {
  uint32_t state = 0x6219;
  auto random = [&] { state = state * 1664525u + 1013904223u; return state; };
  for (size_t cap : {size_t{0}, size_t{1}, size_t{2}, size_t{17}, size_t{500}}) {
    auto made = RepairCandidates::Create(cap);
    Require(made.ok(), "create bounded candidates");
    auto candidates = made.TakeValue();
    std::vector<ParentCandidate> reference(cap);
    size_t count = 0;
    for (size_t i = 0; i < 4000; ++i) {
      Candidate value{};
      snprintf(value.account_id.data(), value.account_id.capacity(), "account-%zu", i);
      snprintf(value.record_id.data(), value.record_id.capacity(), "record-%u", static_cast<unsigned>(random() % 4));
      value.ts = random() % 5;
      value.price_tier = random() % 3;
      candidates.Offer(value);
      ParentCandidate legacy;
      memcpy(&legacy, &value, sizeof(legacy));
      if (!cap) continue;
      if (count < cap) {
        reference[count] = legacy;
        heap_sift_up(reference.data(), count);
        ++count;
      } else if (compare_candidates(legacy, reference[0]) < 0) {
        reference[0] = legacy;
        heap_sift_down(reference.data(), count, 0);
      }
    }
    for (size_t i = 1; i < count; ++i) {
      ParentCandidate moving = reference[i];
      size_t j = i;
      while (j > 0 && compare_candidates(reference[j - 1], moving) > 0) {
        reference[j] = reference[j - 1]; --j;
      }
      reference[j] = moving;
    }
    const auto rows = candidates.TakeSorted();
    Require(rows.size() == count, "retained count matches parent");
    for (size_t i = 0; i < count; ++i) {
      Require(!strcmp(rows[i].account_id.data(), reference[i].account_id) &&
                  !strcmp(rows[i].record_id.data(), reference[i].record_id) &&
                  rows[i].ts == reference[i].ts && rows[i].price_tier == reference[i].price_tier,
              "exact retained accounts and sorted tie order match parent");
    }
  }
}
void bounded_unpriceable_and_moves() {
  auto made = RepairCandidates::Create(2);
  Require(made.ok(), "create");
  auto candidates = made.TakeValue();
  const std::string long_id(900, 'x');
  candidates.MarkUnpriceable(long_id.c_str(), "first");
  auto moved = std::move(candidates);
  moved.MarkUnpriceable("same", "same");
  const auto rows = moved.TakeSorted();
  Require(rows.empty(), "empty priceable history");
  moved.MarkUnpriceable("overflow", "ignored");
  Require(moved.unpriceable().size() == 2, "hard cap and first insertion order");
  char expected[640];
  snprintf(expected, sizeof(expected), "%s", long_id.c_str());
  Require(!strcmp(moved.unpriceable()[0].account.data(), expected) &&
              !strcmp(moved.unpriceable()[0].record.data(), "first") &&
              !strcmp(moved.unpriceable()[1].account.data(), "same"), "legacy truncation and owned strings");
  Require(!RepairCandidates::Create(std::numeric_limits<size_t>::max()), "oversized capacity fails without throwing");
}
}
int main() {
  fortis::testing::UnitSuite suite;
  suite.Run("parent differential including heap ties and eviction", compare_with_parent);
  suite.Run("bounded unpriceable IDs and move ownership", bounded_unpriceable_and_moves);
  return suite.Finish("repair_candidates_test");
}
