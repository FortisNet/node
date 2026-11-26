#include "ownership.h"
// The cached `prices` value of a CoinGecko market_chart/range body:
// JSON scalars streamed in source member order.
#include "news_routes_internal.h"

namespace hb = fortis::codec::history_batch;

namespace fortis::news {

namespace {

// JSON.stringify(JSON.parse(text)) for the prices value, streamed: arrays
// and scalars are written as they are read, so memory does not grow with the
// number of samples. Numbers print as Number.prototype.toString with
// non-finite values (a provider's 1e400) as null; strings decode their
// escapes (lone surrogates replaced) and print with JSON.stringify's escaping;
// an object prints in source order with each duplicate
// key's last value, which needs its members' spans in `arena` (an object
// is re-read from its span). The whole text is validated as JSON.parse does
// (RFC 8259 grammar, any number length); the native bound is nesting deeper
// than 64 levels, refused like a body JSON.parse refuses.
struct Canon {
  const uint8_t* p;
  const uint8_t* end;
  Arena* arena;
  int depth = 0;

  void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }

  // One string token: its decoded UTF-16 units (in `arena`) and validity.
  bool string(U16* out) {
    const uint8_t* start = p;
    if (p >= end || *p != '"') return false;
    ++p;
    while (p < end && *p != '"') {
      if (*p < 0x20) return false;
      if (*p == '\\') { if (++p >= end) return false; }
      ++p;
    }
    if (p >= end) return false;
    ++p;
    const json::Value* value = json::Parse(start, static_cast<size_t>(p - start), arena);
    if (!value || value->type != json::Type::kString) return false;
    *out = value->string;
    return true;
  }

  bool number(OutBuffer* out) {
    const uint8_t* start = p;
    if (p < end && *p == '-') ++p;
    if (p >= end || *p < '0' || *p > '9') return false;
    if (*p == '0') ++p;
    else while (p < end && *p >= '0' && *p <= '9') ++p;
    if (p < end && *p == '.') {
      ++p;
      if (p >= end || *p < '0' || *p > '9') return false;
      while (p < end && *p >= '0' && *p <= '9') ++p;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
      ++p;
      if (p < end && (*p == '+' || *p == '-')) ++p;
      if (p >= end || *p < '0' || *p > '9') return false;
      while (p < end && *p >= '0' && *p <= '9') ++p;
    }
    if (!out) return true;
    const size_t length = static_cast<size_t>(p - start);
    char* copy = static_cast<char*>(arena->Alloc(length + 1, 1));
    if (!copy) return false;
    memcpy(copy, start, length);
    copy[length] = '\0';
    const double value = strtod(copy, nullptr);
    if (isfinite(value)) hb::AppendJsNumber(*out, value);
    else out->Text("null");
    return true;
  }

  bool literal(const char* text, OutBuffer* out) {
    const size_t n = strlen(text);
    if (static_cast<size_t>(end - p) < n || memcmp(p, text, n) != 0) return false;
    p += n;
    if (out) out->Text(text);
    return true;
  }

  // One value; written to `out` unless it is null (validation only).
  bool value(OutBuffer* out) {
    ws();
    if (p >= end) return false;
    const size_t mark = arena->used;
    bool ok = false;
    switch (*p) {
      case '"': {
        U16 text;
        ok = string(&text);
        if (ok && out) fortis::news::AppendJsonString(*out, text);
        break;
      }
      case '[': ok = array(out); break;
      case '{': ok = object(out); break;
      case 't': ok = literal("true", out); break;
      case 'f': ok = literal("false", out); break;
      case 'n': ok = literal("null", out); break;
      default: ok = number(out); break;
    }
    arena->used = mark;
    return ok && (!out || out->ok);
  }

  bool array(OutBuffer* out) {
    if (++depth > 64) return false;
    ++p;
    if (out) out->Byte('[');
    ws();
    if (p < end && *p == ']') {
      ++p;
    } else {
      for (size_t i = 0;; ++i) {
        if (i && out) out->Byte(',');
        if (!value(out)) return false;
        ws();
        if (p < end && *p == ',') { ++p; continue; }
        if (p < end && *p == ']') { ++p; break; }
        return false;
      }
    }
    if (out) out->Byte(']');
    --depth;
    return true;
  }

  struct Member {
    U16 key;
    const uint8_t* value;  // the span of its last value
    const uint8_t* value_end;
  };

  bool object(OutBuffer* out) {
    if (++depth > 64) return false;
    ++p;
    Member* members = nullptr;
    size_t count = 0, capacity = 0;
    ws();
    if (p < end && *p == '}') {
      ++p;
    } else {
      for (;;) {
        ws();
        U16 key;
        if (!string(&key)) return false;
        ws();
        if (p >= end || *p != ':') return false;
        ++p;
        ws();
        const uint8_t* start = p;
        // Validate the member value without writing it (its scratch is
        // released; the key stays, allocated before it).
        if (!value(nullptr)) return false;
        size_t k = 0;
        while (k < count && !fortis::news::Equal(members[k].key, key)) ++k;
        if (k == count) {
          if (count == capacity) {
            const size_t next = capacity ? capacity * 2 : 8;
            auto* grown = static_cast<Member*>(arena->Alloc(next * sizeof(Member), alignof(Member)));
            if (!grown) return false;
            if (count) memcpy(static_cast<void*>(grown), members, count * sizeof(Member));
            members = grown;
            capacity = next;
          }
          members[count++] = Member{key, start, p};
        } else {
          members[k].value = start;
          members[k].value_end = p;
        }
        ws();
        if (p < end && *p == ',') { ++p; continue; }
        if (p < end && *p == '}') { ++p; break; }
        return false;
      }
    }
    --depth;
    if (!out) return true;
    out->Byte('{');
    for (size_t i = 0; i < count; ++i) {
      const Member& member = members[i];
      if (i) out->Byte(',');
      fortis::news::AppendJsonString(*out, member.key);
      out->Byte(':');
      Canon again{member.value, member.value_end, arena, depth};
      if (!again.value(out)) return false;
    }
    out->Byte('}');
    return out->ok;
  }
};

}  // namespace

namespace news_routes_internal {

// The cached `prices` value for one market_chart/range body:
// marketChartRange's `body.prices ?? []` as the SWR store keeps it
// (JSON.stringify of what Response.json() made of the body). False for a
// body JSON.parse refuses or a JSON null body (`null.prices`, the TypeError
// the fetcher throws). `arena` holds only string and object scratch.
bool prices_value(const unsigned char* body, size_t length, uint8_t* output, size_t capacity,
                  size_t* used, Arena* arena) {
  // Response.json() drops one leading byte-order mark.
  if (length >= 3 && body[0] == 0xEF && body[1] == 0xBB && body[2] == 0xBF) { body += 3; length -= 3; }
  const size_t mark = arena->used;
  Canon canon{body, body + length, arena};
  OutBuffer out{output, capacity};
  bool ok = false;
  canon.ws();
  const uint8_t* root = canon.p;
  if (canon.value(nullptr)) {  // the whole body, validated
    canon.ws();
    ok = canon.p == canon.end;
  }
  if (ok) {
    Canon top{root, body + length, arena};
    top.ws();
    if (*top.p == 'n') {
      ok = false;  // null.prices: TypeError
    } else if (*top.p != '{') {
      out.Text("[]");  // a primitive or array has no `prices`
    } else {
      // The last "prices" member's value span, if any.
      const uint8_t* span = nullptr;
      const uint8_t* span_end = nullptr;
      ++top.p;
      top.ws();
      while (ok && top.p < top.end && *top.p != '}') {
        U16 key;
        ok = top.string(&key);
        top.ws();
        ++top.p;  // ':' (validated above)
        top.ws();
        const uint8_t* start = top.p;
        ok = ok && top.value(nullptr);
        if (ok && key.length == 6 && fortis::news::EqualAscii(key, "prices")) { span = start; span_end = top.p; }
        top.ws();
        if (top.p < top.end && *top.p == ',') { ++top.p; top.ws(); }
      }
      if (ok) {
        Canon prices{span, span_end, arena};
        if (!span) out.Text("[]");
        else {
          prices.ws();
          if (*prices.p == 'n') out.Text("[]");  // prices: null -> ?? []
          else ok = prices.value(&out);
        }
      }
    }
  }
  ok = ok && out.ok && arena->ok;
  arena->used = mark;
  arena->ok = true;
  if (!ok) return false;
  *used = out.used;
  return true;
}

}  // namespace news_routes_internal

namespace news_read_rules {

bool PricesValue(const uint8_t* body, size_t length, uint8_t* output, size_t capacity, size_t* used) {
  auto memory_storage = base::AllocateStorage<uint8_t[]>(kNewsArenaBytes);
  auto* memory = memory_storage.get();
  if (!memory) return false;
  Arena arena{memory, kNewsArenaBytes};
  const bool ok = prices_value(body, length, output, capacity, used, &arena);
  memory_storage.reset();
  return ok;
}

}  // namespace news_read_rules

}  // namespace fortis::news
