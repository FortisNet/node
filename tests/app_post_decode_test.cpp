#include "proto_wire.h"
#include "robinhood_service.h"
#include "token_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <vector>

namespace tj = fortis::codec::tj;
namespace pw = fortis::codec::proto_wire;
int main() try {
  FILE* file = fopen("tests/fixtures/app_post_requests.json", "rb");
  if (!file) return 2;
  fseek(file, 0, SEEK_END); const long length = ftell(file); fseek(file, 0, SEEK_SET);
  std::vector<uint8_t> bytes(static_cast<size_t>(length));
  if (fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) { fclose(file); return 2; }
  fclose(file);
  auto arena = tj::GrowingArena(8 * 1024 * 1024);
  auto* document = tj::Parse(arena, bytes.data(), bytes.size());
  auto* rows = tj::Get(document, "requests");
  int failures = 0, diagnosed = 0, prepared = 0;
  if (!tj::IsArray(rows) || rows->count != 18) return 2;
  for (uint32_t i = 0; i < rows->count; ++i) {
    auto* row = rows->items[i];
    auto* hex = tj::Get(row, "hex");
    auto* path = tj::Get(row, "path");
    if (!tj::IsString(hex) || !tj::IsString(path) || hex->length % 2) { ++failures; continue; }
    std::vector<uint8_t> wire;
    auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (uint32_t n = 0; n < hex->length; n += 2)
      wire.push_back(static_cast<uint8_t>((nibble(hex->text[n]) << 4) | nibble(hex->text[n + 1])));
    pw::Reader reader(wire.data(), wire.size());
    bool ok = true;
    while (ok && reader.pos() < wire.size()) {
      uint32_t field = 0, type = 0;
      ok = reader.Tag(&field, &type) && reader.Skip(type, field);
    }
    if (!ok) { fprintf(stderr, "invalid captured app wire: %s\n", path->text); ++failures; }
    using fortis::exchange::RobinhoodWriteKind;
    if (tj::StringEquals(path, "/v1/networks/robinhood/transactions/diagnose") ||
        tj::StringEquals(path, "/v1/networks/robinhood/sponsored-trades/prepare")) {
      const bool diagnose = tj::StringEquals(path, "/v1/networks/robinhood/transactions/diagnose");
      const auto kind = diagnose ? RobinhoodWriteKind::kDiagnose : RobinhoodWriteKind::kPrepare;
      auto* input = fortis::exchange::RobinhoodDecodeWriteRequest(kind, wire.data(), wire.size(), arena);
      bool sponsored = false;
      if (!input || !fortis::exchange::RobinhoodValidWriteRequest(kind, input, &sponsored)) {
        fprintf(stderr, "app request would block signing at schema: %s\n", path->text); ++failures;
      }
      if (diagnose) ++diagnosed; else ++prepared;
    }
  }
  if (diagnosed != 1 || prepared != 1) ++failures;
  tj::FreeArena(arena);
  if (failures) return 1;
  puts("app_post_decode_test: 18 actual app wire captures; diagnose/prepare pass production schemas");
  return 0;
} catch (...) { fputs("app capture decoder test exception\n", stderr); return 2; }
