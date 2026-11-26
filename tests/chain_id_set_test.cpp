#include "chain_id_set.h"
#include "unit_test_support.h"

#include <limits>
#include <thread>
#include <vector>

namespace {
using fortis::chains::ChainId;
using fortis::chains::BoundedChainIdSet;
using fortis::testing::Require;
void bounded_sets() {
  BoundedChainIdSet<32> token;
  BoundedChainIdSet<16> address;
  const ChainId high(uint64_t{1} << 40);
  token.Insert(high);
  token.Insert(ChainId(0));
  Require(token.Contains(high) && token.Contains(ChainId(0)) && !token.Contains(ChainId(1)), "full-width keys do not alias low bits");
  Require(!address.Contains(high), "separate instances do not share state");
  for (uint64_t i = 1; i <= 32; ++i) { token.Insert(ChainId(i)); address.Insert(ChainId(i)); }
  for (uint64_t i = 1; i <= 16; ++i) Require(address.Contains(ChainId(i)), "address retains first sixteen");
  Require(!address.Contains(ChainId(17)) && token.Contains(ChainId(30)) && !token.Contains(ChainId(31)), "independent capacities and drop-new policy");
  address.Insert(ChainId(1));
  Require(address.Contains(ChainId(16)), "duplicate does not evict another key");
  BoundedChainIdSet<0> zero;
  zero.Insert(high); Require(!zero.Contains(high), "zero-capacity set");
}
void number_conversion() {
  Require(ChainId::FromNumber(4294967297.0) == std::optional<ChainId>(ChainId(4294967297ULL)), "numeric key wider than uint32");
  Require(ChainId::FromNumber(-0.0) == std::optional<ChainId>(ChainId(0)), "signed zero normalization");
  Require(!ChainId::FromNumber(-1) && !ChainId::FromNumber(1.5) &&
              !ChainId::FromNumber(std::numeric_limits<double>::infinity()) &&
              !ChainId::FromNumber(std::numeric_limits<double>::quiet_NaN()) &&
              !ChainId::FromNumber(18446744073709551616.0), "conversion checks range before cast");
}
void concurrent_memo() {
  BoundedChainIdSet<32> set;
  std::vector<std::thread> workers;
  for (size_t worker = 0; worker < 8; ++worker) workers.emplace_back([&] {
    for (size_t repeat = 0; repeat < 1000; ++repeat)
      for (uint64_t id = 0; id < 32; ++id) { set.Insert(ChainId(id)); (void)set.Contains(ChainId(id)); }
  });
  for (auto& worker : workers) worker.join();
  for (uint64_t id = 0; id < 32; ++id) Require(set.Contains(ChainId(id)), "concurrent duplicates cannot consume capacity");
}
}
int main() {
  fortis::testing::UnitSuite suite;
  suite.Run("independent bounded insertion-order sets", bounded_sets);
  suite.Run("full-width checked numeric keys", number_conversion);
  suite.Run("concurrent duplicate insertion and lookup", concurrent_memo);
  return suite.Finish("chain_id_set_test");
}
