#pragma once
#include "token_json.h"
#include <stdio.h>
#include <stdlib.h>

// Explicit replacements for superseded request-parity expectations.
// Frozen oracle inputs stay untouched; no native result is used to predict
// an expected result. The independent generator uses urllib/Unicode/decimal
// grammar and records only rows whose contract changed.
namespace strict {
namespace tj = fortis::codec::tj;
inline tj::Value* UnitExpectations(tj::Arena& arena, const char* path = "tests/fixtures/strict_unit_expectations.json") {
  FILE* file = fopen(path, "rb");
  if (!file) return nullptr;
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (size < 0) { fclose(file); return nullptr; }
  auto* bytes = static_cast<uint8_t*>(malloc(static_cast<size_t>(size)));
  const bool ok = bytes && fread(bytes, 1, size, file) == static_cast<size_t>(size);
  fclose(file);
  tj::Value* result = ok ? tj::Parse(arena, bytes, size) : nullptr;
  free(bytes);
  return result;
}
inline bool PatchRows(tj::Arena& arena, tj::Value* rows, const tj::Value* changes,
                      const char* key, const char* subfield = nullptr) {
  if (!tj::IsArray(rows) || !tj::IsObject(changes)) return false;
  for (uint32_t i = 0; i < rows->count; ++i) {
    auto* row = rows->items[i];
    const auto* name = tj::Get(row, key);
    if (!tj::IsString(name)) continue;
    const auto* patch = tj::Get(changes, name->text);
    if (!patch) continue;
    auto* target = subfield ? tj::Get(row, subfield) : row;
    if (!tj::IsObject(target) || !tj::IsObject(patch)) return false;
    for (uint32_t j = 0; j < patch->count; ++j) {
      const auto& m = patch->members[j];
      if (!tj::Set(arena, target, m.key, m.key_length, m.value)) return false;
    }
  }
  return true;
}
inline bool PatchIndexRows(tj::Arena& arena, tj::Value* rows, const tj::Value* changes) {
  if (!tj::IsArray(rows) || !tj::IsObject(changes)) return false;
  for (uint32_t i = 0; i < rows->count; ++i) {
    char index[16]; snprintf(index, sizeof(index), "%u", i);
    const auto* patch = tj::Get(changes, index);
    if (!patch) continue;
    for (uint32_t j = 0; j < patch->count; ++j) {
      const auto& m = patch->members[j];
      if (!tj::Set(arena, rows->items[i], m.key, m.key_length, m.value)) return false;
    }
  }
  return true;
}
inline bool ContractExpectations(tj::Arena& arena, const tj::Value* fixture, const char* name) {
  const auto* all = UnitExpectations(arena, "tests/fixtures/strict_contract_cases.json");
  const auto* patches = tj::Get(all, name);
  if (!tj::IsArray(patches)) return false;
  for (uint32_t i=0; i<patches->count; ++i) {
    const auto* patch=patches->items[i];
    const auto* path=patch->items[0];
    auto* target=const_cast<tj::Value*>(fixture);
    for (uint32_t j=0;j+1<path->count;++j) {
      const auto* key=path->items[j];
      target=tj::IsNumber(key) ? (tj::IsArray(target) && key->number<target->count ? target->items[static_cast<uint32_t>(key->number)] : nullptr) : tj::Get(target,key->text);
      if (!target) return false;
    }
    const auto* key=path->items[path->count-1];
    if (!tj::IsString(key) || !tj::Set(arena,target,key->text,key->length,patch->items[1])) return false;
  }
  return true;
}
}  // namespace strict
