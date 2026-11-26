#include "fixed_string.h"
#pragma once

#include "address_store.h"
#include "status.h"

#include <new>
#include <stdio.h>
#include <string.h>
#include <utility>
#include <vector>

namespace fortis::wallet::address::repair {

struct Candidate {
  base::FixedString<kAccountIdBytes + 1> account_id;
  base::FixedString<640> record_id;
  base::FixedString<96> network;
  base::FixedString<400> token_id;
  int64_t ts;
  base::FixedString<200> coingecko_id;
  bool has_coingecko;
  double price_tier;
};

struct UnpriceableId { base::FixedString<640> account; base::FixedString<640> record; };

// Bounded max-heap: retains the smallest candidates by tier/time/record ID.
// Account is deliberately not a tie-breaker. Exact sift and insertion-sort
// sequences preserve both which tied accounts survive and their final order.
class RepairCandidates {
 public:
  static base::Result<RepairCandidates> Create(size_t cap) {
    RepairCandidates result(cap);
    if (cap > result.rows_.max_size() || cap > result.unpriceable_.max_size())
      return base::Status(base::StatusCode::kInvalidArgument, "repair capacity too large");
    try {
      result.rows_.reserve(cap);
      result.unpriceable_.reserve(cap);
    } catch (const std::bad_alloc&) {
      return base::Status(base::StatusCode::kResourceExhausted, "repair allocation failed");
    }
    return result;
  }
  RepairCandidates(RepairCandidates&& other) noexcept
      : rows_(std::move(other.rows_)), unpriceable_(std::move(other.unpriceable_)),
        cap_(std::exchange(other.cap_, 0)), sorted_(other.sorted_) {}
  RepairCandidates& operator=(RepairCandidates&&) = delete;
  RepairCandidates(const RepairCandidates&) = delete;
  RepairCandidates& operator=(const RepairCandidates&) = delete;

  void Offer(const Candidate& candidate) {
    if (sorted_) abort(); // Offers must precede TakeSorted().
    if (!cap_) return;
    if (rows_.size() < cap_) {
      rows_.push_back(candidate);
      SiftUp(rows_.size() - 1);
    } else if (Compare(candidate, rows_[0]) < 0) {
      rows_[0] = candidate;
      SiftDown(0);
    }
  }
  void MarkUnpriceable(const char* account, const char* record) {
    if (unpriceable_.size() >= cap_) return;
    UnpriceableId id;
    id.account.Assign(account);
    id.record.Assign(record);
    unpriceable_.push_back(id);
  }
  std::vector<Candidate> TakeSorted() {
    if (sorted_) abort();
    sorted_ = true;
    for (size_t i = 1; i < rows_.size(); ++i) {
      Candidate moving = rows_[i];
      size_t j = i;
      while (j > 0 && Compare(rows_[j - 1], moving) > 0) {
        rows_[j] = rows_[j - 1];
        --j;
      }
      rows_[j] = moving;
    }
    return std::move(rows_);
  }
  const std::vector<UnpriceableId>& unpriceable() const { return unpriceable_; }

 private:
  explicit RepairCandidates(size_t cap) : cap_(cap) {}
  static int Compare(const Candidate& a, const Candidate& b) {
    if (a.price_tier != b.price_tier) return a.price_tier < b.price_tier ? -1 : 1;
    if (a.ts != b.ts) return a.ts < b.ts ? -1 : 1;
    return strcmp(a.record_id.data(), b.record_id.data());
  }
  void SiftUp(size_t i) {
    while (i > 0) {
      const size_t parent = (i - 1) / 2;
      if (Compare(rows_[parent], rows_[i]) >= 0) break;
      std::swap(rows_[parent], rows_[i]);
      i = parent;
    }
  }
  void SiftDown(size_t i) {
    for (;;) {
      const size_t left = i * 2 + 1;
      if (left >= rows_.size()) break;
      const size_t right = left + 1;
      const size_t child = right < rows_.size() && Compare(rows_[right], rows_[left]) > 0 ? right : left;
      if (Compare(rows_[i], rows_[child]) >= 0) break;
      std::swap(rows_[i], rows_[child]);
      i = child;
    }
  }
  std::vector<Candidate> rows_;
  std::vector<UnpriceableId> unpriceable_;
  size_t cap_;
  bool sorted_ = false;
};

} // namespace fortis::wallet::address::repair
