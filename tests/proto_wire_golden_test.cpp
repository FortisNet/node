// The hand-written protobuf writer (src/codec/proto_wire.h) against
//  (1) reference encoders: verbatim copies of the per-module writers the
//      unified writer replaced, kept here as an
//      independent oracle, over boundary and pseudo-random values;
//  (2) golden bytes (tests/proto_wire_golden.inc) for whole messages. The
//      json_document_writer/* and tj/* entries were captured from the pre-unification
//      writers: the two google.protobuf.Struct emitters (json_document_writer over a
//      bounded document, tj over the mutable DOM) and both JSON.stringify
//      emulations, on documents that exercise the JavaScript-parity rules
//      listed in docs/codec-semantics.md. The two message/* entries are
//      written with the new API (the old one had no such caller), so each is
//      also rebuilt here with the reference encoders and compared.
// Before the old writers were deleted this test also compared the live
// json_document_writer::Append* helpers with proto_wire::Put* on 240,000 random fields.
// `--print` writes the golden table instead of checking it; regenerate it
// only for an intended wire change.
#include "bounded_json.h"
#include "json_document_writer.h"
#include "price_history_batch_codec.h"
#include "proto_wire.h"
#include "token_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace fortis::codec;

namespace pw = fortis::codec::proto_wire;
namespace jj = fortis::codec::json_document_writer;
namespace tj = fortis::codec::tj;
using fortis::codec::history_batch::OutBuffer;

namespace {

int failures = 0;
void fail(const char* group, const char* detail) {
  fprintf(stderr, "FAIL %s: %s\n", group, detail);
  ++failures;
}

// ---- reference encoders (the replaced writers, verbatim) ----------------------

// json_document_writer / token_json / ai_service / sync_route style.
size_t legacy_while(uint8_t* out, uint64_t value) {
  size_t n = 0;
  while (value >= 0x80) { out[n++] = static_cast<uint8_t>(value | 0x80); value >>= 7; }
  out[n++] = static_cast<uint8_t>(value);
  return n;
}
// price_history_view / price_history_route / token_comparison / dashpay style.
size_t legacy_do(uint8_t* out, uint64_t value) {
  size_t used = 0;
  do {
    uint8_t byte = static_cast<uint8_t>(value & 0x7f);
    value >>= 7;
    if (value) byte |= 0x80;
    out[used++] = byte;
  } while (value);
  return used;
}
size_t legacy_size(uint64_t value) {
  size_t n = 1;
  while (value >= 0x80) { value >>= 7; ++n; }
  return n;
}
// dashpay_proof PbWriter.
struct LegacyPbWriter {
  uint8_t bytes[512];
  size_t used = 0;
  bool ok = true;
  void Raw(const void* data, size_t length) {
    if (!length) return;
    if (!ok || length > sizeof(bytes) - used) { ok = false; return; }
    memcpy(bytes + used, data, length);
    used += length;
  }
  void Varint(uint64_t value) {
    uint8_t encoded[10];
    Raw(encoded, legacy_do(encoded, value));
  }
  void Bytes(uint32_t field, const void* data, size_t length) {
    Varint(static_cast<uint64_t>(field) << 3 | 2);
    Varint(length);
    Raw(data, length);
  }
};
// The former json_document_writer::Append* helpers (OutBuffer, byte at a time).
void legacy_varint(OutBuffer& out, uint64_t value) {
  while (value >= 0x80) { out.Byte(static_cast<uint8_t>(value | 0x80)); value >>= 7; }
  out.Byte(static_cast<uint8_t>(value));
}
void legacy_tag(OutBuffer& out, uint32_t field, uint32_t wire_type) {
  legacy_varint(out, static_cast<uint64_t>(field) << 3 | wire_type);
}
void legacy_length_delimited(OutBuffer& out, uint32_t field, const void* bytes, size_t length) {
  legacy_tag(out, field, 2);
  legacy_varint(out, length);
  out.Append(bytes, length);
}
void legacy_double(OutBuffer& out, uint32_t field, double value) {
  uint64_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  legacy_tag(out, field, 1);
  for (int i = 0; i < 8; ++i) out.Byte(static_cast<uint8_t>(bits >> (8 * i)));
}

void check_varint(uint64_t value) {
  uint8_t a[10], b[10], c[10];
  const size_t na = legacy_while(a, value), nb = legacy_do(b, value);
  const size_t nc = pw::EncodeVarint(c, value);
  if (na != nb || na != nc || memcmp(a, b, na) != 0 || memcmp(a, c, na) != 0) fail("varint", "encode");
  if (pw::VarintSize(value) != na || legacy_size(value) != na) fail("varint", "size");
  uint8_t d[16], e[16];
  OutBuffer buffer{d, sizeof(d)};
  pw::PutVarint(buffer, value);
  pw::Span span{e, sizeof(e)};
  pw::PutVarint(span, value);
  OutBuffer counter{};
  pw::PutVarint(counter, value);
  if (!buffer.ok || buffer.used != na || memcmp(d, a, na) != 0) fail("varint", "OutBuffer");
  if (!span.ok || span.used != na || memcmp(e, a, na) != 0) fail("varint", "Span");
  if (!counter.ok || counter.used != na) fail("varint", "counter");
  // The reader half round-trips what the writer half wrote.
  size_t position = 0;
  uint64_t back = 0;
  if (!pw::ReadVarint64(c, nc, &position, &back) || back != value || position != nc) fail("varint", "round trip");
}

void differential_primitives() {
  // The protobuf standard omits +0 but preserves the distinct -0 value.
  uint8_t zero_bytes[16]{};
  pw::Span zero_sink{zero_bytes, sizeof(zero_bytes)};
  pw::PutImplicitDouble(zero_sink, 2, 0.0);
  if (zero_sink.used != 0) fail("implicit-double", "positive zero must be omitted");
  pw::PutImplicitDouble(zero_sink, 2, -0.0);
  constexpr uint8_t negative_zero[] = {0x11, 0, 0, 0, 0, 0, 0, 0, 0x80};
  if (!zero_sink.ok || zero_sink.used != sizeof(negative_zero) ||
      memcmp(zero_bytes, negative_zero, sizeof(negative_zero)) != 0)
    fail("implicit-double", "negative zero must retain its sign on the wire");

  for (unsigned shift = 0; shift < 64; ++shift) {
    const uint64_t power = 1ULL << shift;
    check_varint(power - 1);
    check_varint(power);
    check_varint(power + 1);
  }
  check_varint(0);
  check_varint(UINT64_MAX);
  check_varint(static_cast<uint64_t>(static_cast<int64_t>(-1)));
  check_varint(static_cast<uint64_t>(static_cast<int64_t>(INT32_MIN)));
  uint64_t state = 0x9e3779b97f4a7c15ULL;
  for (int i = 0; i < 200000; ++i) {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    check_varint(state >> (state & 63));
  }

  // Tags, length-delimited fields and doubles against the former helpers.
  const uint32_t fields[] = {1, 2, 15, 16, 127, 2047, 2048, 262143, 536870911};
  const double doubles[] = {0.0, -0.0, 1.0, -1.5, 3.141592653589793, 5e-324, 1.7976931348623157e308,
                            static_cast<double>(INFINITY), -static_cast<double>(INFINITY),
                            static_cast<double>(NAN), 1e21, 0.1, 9007199254740993.0};
  static uint8_t payload[20000];
  for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = static_cast<uint8_t>(i * 131 + 7);
  const size_t lengths[] = {0, 1, 127, 128, 300, 16383, 16384, 20000};
  for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); ++f) {
    static uint8_t old_bytes[21000], new_bytes[21000];
    for (uint32_t wire = 0; wire <= 5; ++wire) {
      OutBuffer a{old_bytes, sizeof(old_bytes)}, b{new_bytes, sizeof(new_bytes)};
      legacy_tag(a, fields[f], wire);
      pw::PutTag(b, fields[f], wire);
      if (a.used != b.used || memcmp(old_bytes, new_bytes, a.used) != 0) fail("tag", "bytes");
    }
    for (size_t l = 0; l < sizeof(lengths) / sizeof(lengths[0]); ++l) {
      OutBuffer a{old_bytes, sizeof(old_bytes)}, b{new_bytes, sizeof(new_bytes)};
      legacy_length_delimited(a, fields[f], payload, lengths[l]);
      pw::PutBytes(b, fields[f], payload, lengths[l]);
      if (!a.ok || !b.ok || a.used != b.used || memcmp(old_bytes, new_bytes, a.used) != 0)
        fail("length-delimited", "bytes");
      if (pw::BytesFieldSize(fields[f], lengths[l]) != a.used) fail("length-delimited", "size");
      OutBuffer c{new_bytes, sizeof(new_bytes)};
      pw::PutLengthPrefix(c, fields[f], lengths[l]);
      c.Append(payload, lengths[l]);
      if (c.used != a.used || memcmp(old_bytes, new_bytes, a.used) != 0) fail("length-prefix", "bytes");
    }
    for (size_t d = 0; d < sizeof(doubles) / sizeof(doubles[0]); ++d) {
      OutBuffer a{old_bytes, sizeof(old_bytes)}, b{new_bytes, sizeof(new_bytes)};
      legacy_double(a, fields[f], doubles[d]);
      pw::PutDouble(b, fields[f], doubles[d]);
      if (a.used != b.used || memcmp(old_bytes, new_bytes, a.used) != 0) fail("double", "bytes");
    }
    for (int v = 0; v < 70; ++v) {
      const uint64_t value = v < 64 ? 1ULL << v : static_cast<uint64_t>(v - 64) * 0x0123456789abcdefULL;
      OutBuffer a{old_bytes, sizeof(old_bytes)}, b{new_bytes, sizeof(new_bytes)};
      legacy_tag(a, fields[f], 0); legacy_varint(a, value);
      pw::PutVarintField(b, fields[f], value);
      if (a.used != b.used || memcmp(old_bytes, new_bytes, a.used) != 0) fail("varint field", "bytes");
    }
  }

  // Bounded sinks refuse exactly as the former PbWriter did.
  for (size_t length = 500; length <= 520; ++length) {
    LegacyPbWriter legacy;
    uint8_t bytes[512];
    pw::Span span{bytes, sizeof(bytes)};
    legacy.Bytes(3, payload, length);
    pw::PutBytes(span, 3, payload, length);
    legacy.Bytes(4, payload, 1);
    pw::PutBytes(span, 4, payload, 1);
    if (legacy.ok != span.ok) fail("span", "ok flag");
    if (legacy.ok && (legacy.used != span.used || memcmp(legacy.bytes, bytes, span.used) != 0)) fail("span", "bytes");
  }
}

// ---- whole messages -------------------------------------------------------------

struct Golden {
  const char* name;
  const char* hex;
};

#include "proto_wire_golden.inc"

bool print_mode = false;
size_t next_golden = 0;

void emit(const char* name, const uint8_t* bytes, size_t length, bool ok) {
  static char hex[1 << 16];
  size_t used = 0;
  if (!ok) { hex[used++] = '!'; length = 0; }
  if (length * 2 + 2 > sizeof(hex)) { fail(name, "golden too large"); return; }
  for (size_t i = 0; i < length; ++i) used += static_cast<size_t>(snprintf(hex + used, 3, "%02x", bytes[i]));
  hex[used] = '\0';
  if (print_mode) { printf("  {\"%s\", \"%s\"},\n", name, hex); return; }
  const size_t count = sizeof(kGolden) / sizeof(kGolden[0]);
  if (next_golden >= count) { fail(name, "no golden entry"); return; }
  const Golden& golden = kGolden[next_golden++];
  if (strcmp(golden.name, name) != 0) { fail(name, "golden order"); return; }
  if (strcmp(golden.hex, hex) != 0) {
    fail(name, "bytes differ from the golden");
    fprintf(stderr, "  expected %s\n  actual   %s\n", golden.hex, hex);
  }
}

// A message that uses every writer entry point, shaped like the discovery
// and news encoders (strings, nested messages, map entries, doubles, bools).
void message_goldens() {
  static uint8_t bytes[4096], inner_bytes[512];
  OutBuffer out{bytes, sizeof(bytes)};
  pw::PutBytes(out, 1, "bitcoin", 7);
  pw::PutBytes(out, 2, "", 0);  // explicit-presence empty string
  pw::PutVarintField(out, 3, 1700000000000ULL);
  pw::PutVarintField(out, 4, static_cast<uint64_t>(static_cast<int64_t>(-2)));  // int64 -2: ten bytes
  pw::PutDouble(out, 5, 64250.125);
  pw::PutDouble(out, 6, -0.0);  // protobuf-es writes -0 for an implicit double
  pw::Span inner{inner_bytes, sizeof(inner_bytes)};
  pw::PutVarintField(inner, 1, 1);
  pw::PutBytes(inner, 2, "2026-11-17T00:00:00.000Z", 24);
  pw::PutBytes(out, 7, inner.data, inner.used);
  pw::PutBytes(out, 7, nullptr, 0);  // always-present empty sub-message
  // map<string, Message> entry: {1: key, 2: value}, measured then written.
  OutBuffer measure{};
  pw::PutBytes(measure, 1, "k\xc3\xa9y", 4);
  pw::PutBytes(measure, 2, inner.data, inner.used);
  pw::PutLengthPrefix(out, 8, measure.used);
  pw::PutBytes(out, 1, "k\xc3\xa9y", 4);
  pw::PutBytes(out, 2, inner.data, inner.used);
  pw::PutTag(out, 2047, pw::kWireFixed64);
  pw::PutFixed64(out, 0x0102030405060708ULL);
  emit("message/all-entry-points", bytes, out.used, out.ok && inner.ok);
  // The same message through the replaced writers: the golden above is
  // written by the new API, so this is what ties it to the old bytes.
  static uint8_t legacy_bytes[4096];
  OutBuffer legacy{legacy_bytes, sizeof(legacy_bytes)};
  legacy_length_delimited(legacy, 1, "bitcoin", 7);
  legacy_length_delimited(legacy, 2, "", 0);
  legacy_tag(legacy, 3, 0); legacy_varint(legacy, 1700000000000ULL);
  legacy_tag(legacy, 4, 0); legacy_varint(legacy, static_cast<uint64_t>(static_cast<int64_t>(-2)));
  legacy_double(legacy, 5, 64250.125);
  legacy_double(legacy, 6, -0.0);
  LegacyPbWriter legacy_inner;
  legacy_inner.Varint(1 << 3 | 0); legacy_inner.Varint(1);
  legacy_inner.Bytes(2, "2026-11-17T00:00:00.000Z", 24);
  legacy_length_delimited(legacy, 7, legacy_inner.bytes, legacy_inner.used);
  legacy_length_delimited(legacy, 7, nullptr, 0);
  OutBuffer legacy_measure{};
  legacy_length_delimited(legacy_measure, 1, "k\xc3\xa9y", 4);
  legacy_length_delimited(legacy_measure, 2, legacy_inner.bytes, legacy_inner.used);
  legacy_tag(legacy, 8, 2); legacy_varint(legacy, legacy_measure.used);
  legacy_length_delimited(legacy, 1, "k\xc3\xa9y", 4);
  legacy_length_delimited(legacy, 2, legacy_inner.bytes, legacy_inner.used);
  legacy_tag(legacy, 2047, 1);
  for (int i = 0; i < 8; ++i) legacy.Byte(static_cast<uint8_t>(0x0102030405060708ULL >> (8 * i)));
  if (!legacy.ok || !legacy_inner.ok || legacy.used != out.used || memcmp(legacy_bytes, bytes, out.used) != 0)
    fail("message/all-entry-points", "differs from the replaced writers");

  // The DashPay GetDocumentsRequest shape (nested bounded writers).
  uint8_t v1_bytes[512], clause_bytes[512], value_bytes[512], request_bytes[512];
  uint8_t id[32];
  for (size_t i = 0; i < sizeof(id); ++i) id[i] = static_cast<uint8_t>(0xe6 - i);
  pw::Span v1{v1_bytes, sizeof(v1_bytes)};
  pw::PutBytes(v1, 1, id, sizeof(id));
  pw::PutBytes(v1, 2, "domain", 6);
  pw::Span value{value_bytes, sizeof(value_bytes)}, clause{clause_bytes, sizeof(clause_bytes)};
  pw::PutBytes(value, 5, "alice", 5);
  pw::PutBytes(clause, 1, "normalizedLabel", 15);
  pw::PutBytes(clause, 3, value.data, value.used);
  pw::PutBytes(v1, 3, clause.data, clause.used);
  pw::PutVarintField(v1, 8, 1);
  pw::PutBytes(v1, 9, nullptr, 0);
  pw::Span request{request_bytes, sizeof(request_bytes)};
  pw::PutBytes(request, 2, v1.data, v1.used);
  emit("message/nested-spans", request_bytes, request.used, v1.ok && request.ok);
  // The former BuildGetDocumentsRequest body, statement for statement.
  LegacyPbWriter legacy_v1, legacy_value, legacy_clause, legacy_request;
  legacy_v1.Bytes(1, id, sizeof(id));
  legacy_v1.Bytes(2, "domain", 6);
  legacy_value.Bytes(5, "alice", 5);
  legacy_clause.Bytes(1, "normalizedLabel", 15);
  legacy_clause.Bytes(3, legacy_value.bytes, legacy_value.used);
  legacy_v1.Bytes(3, legacy_clause.bytes, legacy_clause.used);
  legacy_v1.Varint(8 << 3 | 0);
  legacy_v1.Varint(1);
  legacy_v1.Bytes(9, nullptr, 0);
  legacy_request.Bytes(2, legacy_v1.bytes, legacy_v1.used);
  if (!legacy_v1.ok || !legacy_request.ok || legacy_request.used != request.used ||
      memcmp(legacy_request.bytes, request_bytes, request.used) != 0)
    fail("message/nested-spans", "differs from the replaced writer");
}

// JSON documents covering the JavaScript-parity rules. `bounded` is false for
// the ones the bounded parser refuses by design (lone surrogates).
struct Document {
  const char* name;
  const char* json;
};
const Document kDocuments[] = {
  {"scalars", "{\"s\":\"text\",\"n\":1.5,\"t\":true,\"f\":false,\"z\":null}"},
  {"empty", "{\"o\":{},\"a\":[],\"e\":\"\"}"},
  {"nested", "{\"a\":[1,[2,[3,{\"b\":[]}]],\"x\",null,true],\"o\":{\"p\":{\"q\":{\"r\":0}}}}"},
  {"duplicate-keys", "{\"a\":1,\"b\":2,\"a\":3,\"c\":4,\"b\":{\"x\":1,\"x\":2}}"},
  {"index-keys", "{\"b\":1,\"10\":2,\"2\":3,\"a\":4,\"01\":5,\"4294967294\":6,\"4294967295\":7,\"0\":8,\"-1\":9}"},
  {"numbers", "{\"a\":-0,\"b\":0.0,\"c\":1e21,\"d\":1e-7,\"e\":123456789012345680000,\"f\":0.1,"
              "\"g\":9007199254740993,\"h\":-1.5e-10,\"i\":1E3,\"j\":100,\"k\":5e-324,\"l\":0.000001}"},
  {"escapes", "{\"q\":\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\\u0001\\u001f\\u007f\",\"u\":\"\\u00e9\\u20ac\\ud83d\\ude00\","
              "\"k\\u0065y\":\"v\",\"key\":\"w\"}"},
  {"utf8", "{\"\xc3\xa9\":\"\xe2\x82\xac\xf0\x9f\x98\x80\",\"\xe2\x80\xa8\":\"\xe2\x80\xa9\"}"},
  {"long-string", "{\"s\":\"0123456789012345678901234567890123456789012345678901234567890123456789"
                  "0123456789012345678901234567890123456789012345678901234567890123456789\"}"},
  {"lone-surrogates", "{\"hi\":\"a\\ud83dz\",\"lo\":\"\\ude00\",\"swapped\":\"\\ude00\\ud83d\",\"k\\udfff\":1}"},
  {"array-root", "[1,\"two\",{\"three\":3},[4],null]"},
  {"whitespace", " \t\r\n{ \"a\" : [ 1 , 2 ] , \"b\" : { } } \n"},
};

void struct_goldens() {
  static BoundedJsonToken tokens[4096];
  static uint32_t words[4096 * jj::kScratchWordsPerToken];
  static uint8_t out_bytes[1 << 14];
  static uint8_t arena_bytes[1 << 18];
  const jj::Scratch scratch{words, sizeof(words) / sizeof(words[0])};
  char name[96];
  for (size_t i = 0; i < sizeof(kDocuments) / sizeof(kDocuments[0]); ++i) {
    const Document& document = kDocuments[i];
    const unsigned char* text = reinterpret_cast<const unsigned char*>(document.json);
    const size_t length = strlen(document.json);

    // Bounded tokenizer + json_document_writer.
    BoundedJsonDocument parsed;
    BoundedJsonError error = BoundedJsonError::kNone;
    const bool accepted = parsed.Parse(text, length, tokens, sizeof(tokens) / sizeof(tokens[0]), &error);
    snprintf(name, sizeof(name), "json_document_writer/%s/stringify", document.name);
    OutBuffer json{out_bytes, sizeof(out_bytes)};
    bool ok = accepted && jj::Stringify(parsed, parsed.root(), json, scratch);
    emit(name, out_bytes, json.used, ok);
    snprintf(name, sizeof(name), "json_document_writer/%s/proto-value", document.name);
    OutBuffer value{out_bytes, sizeof(out_bytes)};
    ok = accepted && jj::AppendProtoValue(parsed, parsed.root(), value, scratch);
    emit(name, out_bytes, value.used, ok);
    snprintf(name, sizeof(name), "json_document_writer/%s/proto-struct", document.name);
    OutBuffer structure{out_bytes, sizeof(out_bytes)};
    ok = accepted && jj::AppendProtoStruct(parsed, parsed.root(), structure, scratch);
    emit(name, out_bytes, structure.used, ok);
    if (accepted) {
      // The measure pass (a counting buffer) agrees with the write pass.
      OutBuffer counter{};
      if (!jj::AppendProtoValue(parsed, parsed.root(), counter, scratch) || counter.used != value.used)
        fail(document.name, "json_document_writer measure pass");
    }

    // Mutable DOM (tj).
    tj::Arena arena;
    arena.base = arena_bytes;
    arena.capacity = sizeof(arena_bytes);
    const tj::Value* root = tj::Parse(arena, text, length);
    snprintf(name, sizeof(name), "tj/%s/stringify", document.name);
    OutBuffer tj_json{out_bytes, sizeof(out_bytes)};
    ok = root && tj::Stringify(root, tj_json);
    emit(name, out_bytes, tj_json.used, ok);
    snprintf(name, sizeof(name), "tj/%s/proto-struct", document.name);
    OutBuffer tj_struct{out_bytes, sizeof(out_bytes)};
    ok = root && tj::IsObject(root) && tj::AppendProtoStruct(root, tj_struct);
    emit(name, out_bytes, tj_struct.used, ok);
    snprintf(name, sizeof(name), "tj/%s/structured-read-response", document.name);
    OutBuffer tj_response{out_bytes, sizeof(out_bytes)};
    ok = root && tj::IsObject(root) && tj::AppendStructuredReadResponse(root, tj_response);
    emit(name, out_bytes, tj_response.used, ok);
    if (root && tj::IsObject(root)) {
      OutBuffer counter{};
      if (!tj::AppendProtoStruct(root, counter) || counter.used != tj_struct.used)
        fail(document.name, "tj measure pass");
    }
  }

  // A DOM built in code: Set replaces in place, index keys sort first, a
  // non-finite number stringifies as null, -0 keeps its sign in protobuf.
  tj::Arena arena;
  arena.base = arena_bytes;
  arena.capacity = sizeof(arena_bytes);
  tj::Value* object = tj::Obj(arena);
  tj::Value* list = tj::Arr(arena);
  tj::Push(arena, list, tj::Num(arena, -0.0));
  tj::Push(arena, list, tj::Num(arena, static_cast<double>(NAN)));
  tj::Push(arena, list, tj::Num(arena, static_cast<double>(INFINITY)));
  tj::Push(arena, list, tj::Str(arena, "\xed\xa0\xbd tail", 8));  // WTF-8 lone high surrogate
  tj::Set(arena, object, "zeta", tj::Num(arena, 1));
  tj::Set(arena, object, "7", tj::Bool(arena, true));
  tj::Set(arena, object, "list", list);
  tj::Set(arena, object, "zeta", tj::Str(arena, "replaced"));
  tj::Set(arena, object, "3", tj::Null(arena));
  tj::Set(arena, object, "gone", tj::Num(arena, 2));
  tj::Remove(object, "gone");
  OutBuffer built_json{out_bytes, sizeof(out_bytes)};
  bool ok = arena.ok && tj::Stringify(object, built_json);
  emit("tj/built/stringify", out_bytes, built_json.used, ok);
  OutBuffer built_struct{out_bytes, sizeof(out_bytes)};
  ok = arena.ok && tj::AppendProtoStruct(object, built_struct);
  emit("tj/built/proto-struct", out_bytes, built_struct.used, ok);
}

}  // namespace

int main(int argc, char** argv) {
  print_mode = argc > 1 && strcmp(argv[1], "--print") == 0;
  if (!print_mode) differential_primitives();
  message_goldens();
  struct_goldens();
  if (!print_mode && next_golden != sizeof(kGolden) / sizeof(kGolden[0])) fail("golden", "unused entries");
  if (failures) return 1;
  if (!print_mode) printf("proto_wire_golden_test: OK (%zu golden messages)\n", next_golden);
  return 0;
}
