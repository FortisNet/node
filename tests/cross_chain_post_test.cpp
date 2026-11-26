#include "test_support.h"
// Unit checks of POST /v1/exchange/cross-chain/quotes (R41) and
// POST /v1/exchange/cross-chain/status (R42) below the HTTP layer, against
// the TypeScript oracles cross_chain_quotes_edges.json and
// cross_chain_status_edges.json:
//  - every request record that reached validation (past the body parser and
//    the device hook) goes through the route's own steps — protobuf-es
//    decoding (CrossChainDecode*Request), the JSON/form body
//    (CrossChainRequestInput) and zod plus provider selection
//    (CrossChainPlan*) — and must reach TypeScript's verdict: a bare 400,
//    the UNKNOWN_CROSS_CHAIN_PROVIDER refusal (quotes: with TypeScript's
//    exact id list), or provider work for exactly the providers TypeScript
//    reported, in its order (quotes: `attempts`; status: the provider asked
//    or the 503 provider);
//  - where TypeScript asked LI.FI, the plan's normalized request carries the
//    values TypeScript put in the URL (quotes: chains, tokens, amount,
//    addresses, slippage fraction; status: txHash, chains, bridge hint);
//  - every 200 JSON answer re-encodes (CrossChainEncode*Response) to the
//    exact protobuf bytes quoteMessage/statusMessage + toBinary give for it.
// Usage: cross_chain_post_test cross_chain_quotes_edges.json cross_chain_status_edges.json
#include "strict_proto_cases.h"
#include "cross_chain_service.h"
#include "discovery_post_unit.h"

#include <map>

using namespace fortis::exchange;

namespace {

using namespace post_unit;

struct Counts {
  int plans = 0, refusals = 0, unknown = 0, dispatch = 0, url_checks = 0, encoded = 0, skipped = 0;
};

std::string percent_decode(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '+') { out += ' '; continue; }
    if (in[i] == '%' && i + 2 < in.size() && hex_digit(in[i + 1]) >= 0 && hex_digit(in[i + 2]) >= 0) {
      out += static_cast<char>(hex_digit(in[i + 1]) * 16 + hex_digit(in[i + 2]));
      i += 2;
      continue;
    }
    out += in[i];
  }
  return out;
}

std::map<std::string, std::string> query_of(const std::string& url) {
  std::map<std::string, std::string> query;
  const size_t start = url.find('?');
  if (start == std::string::npos) return query;
  size_t at = start + 1;
  while (at <= url.size()) {
    size_t end = url.find('&', at);
    if (end == std::string::npos) end = url.size();
    const std::string pair = url.substr(at, end - at);
    const size_t eq = pair.find('=');
    query[percent_decode(pair.substr(0, eq))] = eq == std::string::npos ? "" : percent_decode(pair.substr(eq + 1));
    at = end + 1;
  }
  return query;
}

// A string as URLSearchParams writes it: a lone surrogate (WTF-8 ED A0..BF
// xx in the parsed request) becomes U+FFFD.
std::string usv(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    const uint8_t c = static_cast<uint8_t>(in[i]);
    if (c == 0xed && i + 2 < in.size() && static_cast<uint8_t>(in[i + 1]) >= 0xa0) {
      out += "\xef\xbf\xbd";
      i += 2;
      continue;
    }
    out += in[i];
  }
  return out;
}

// String(bps / 10_000) for 0..5000.
std::string slippage_fraction(uint32_t bps) {
  if (!bps) return "0";
  char digits[16];
  snprintf(digits, sizeof(digits), "%04u", bps);
  std::string text = std::string("0.") + digits;
  while (text.back() == '0') text.pop_back();
  return text;
}

// lifiChain(): the two named chains LI.FI numbers; every other id as is.
std::string lifi_chain(const std::string& id) {
  if (id == "solana-mainnet-beta") return "1151111081099710";
  if (id == "bitcoin") return "20000000000001";
  return id;
}

const tj::Value* body_value(const tj::Value* record, tj::Arena& arena, bool status, bool* parsed_ok) {
  *parsed_ok = true;
  const std::vector<uint8_t> bytes = unhex(tj::Get(record, "bodyHex"));
  const tj::Value* type = header(tj::Get(record, "requestHeaders"), "content-type");
  if (!type) return nullptr;
  std::string media = text(type);
  if (const auto end = media.find_first_of("; \t"); end != std::string::npos) media.resize(end);
  if (strcasecmp(media.c_str(), "application/protobuf") == 0)
    return status ? CrossChainDecodeStatusRequest(bytes.data(), bytes.size(), arena)
                  : CrossChainDecodeQuoteRequest(bytes.data(), bytes.size(), arena);
  if (strcasecmp(media.c_str(), "application/json") == 0) {
    const tj::Value* value = CrossChainRequestInput(bytes.data(), bytes.size(), arena);
    *parsed_ok = value != nullptr;
    return value;
  }
  return nullptr;  // text/plain and form bodies are refused before (before_validation)
}

// The provider ids TypeScript's answer names: quotes' attempts (JSON or the
// decoded protobuf), in order.
std::vector<std::string> attempt_ids(const tj::Value* record, tj::Arena& arena) {
  std::vector<std::string> ids;
  const tj::Value* answer = nullptr;
  const std::string response = text(tj::Get(record, "responseText"));
  if (!response.empty()) answer = tj::Parse(arena, reinterpret_cast<const uint8_t*>(response.data()), response.size());
  else answer = tj::Get(record, "decoded");
  const tj::Value* attempts = tj::Get(answer, "attempts");
  for (uint32_t i = 0; tj::IsArray(attempts) && i < attempts->count; ++i)
    ids.push_back(text(tj::Get(attempts->items[i], "providerId")));
  return ids;
}

std::string first_url(const tj::Value* record, const char* prefix) {
  const tj::Value* upstream = tj::Get(record, "upstream");
  for (uint32_t i = 0; tj::IsArray(upstream) && i < upstream->count; ++i) {
    const std::string url = text(tj::Get(upstream->items[i], "url"));
    if (url.rfind(prefix, 0) == 0) return url;
  }
  return "";
}

void encode_check(const std::string& label, const tj::Value* record, bool status, Counts* counts) {
  const tj::Value* expected = tj::Get(record, "protoOfJson");
  if (!tj::IsString(expected)) return;
  const std::string response = text(tj::Get(record, "responseText"));
  tj::Arena arena = tj::GrowingArena(1 << 20);
  const tj::Value* result = tj::Parse(arena, reinterpret_cast<const uint8_t*>(response.data()), response.size());
  std::vector<uint8_t> buffer(response.size() * 2 + 1024);
  fortis::codec::tj::OutBuffer out{buffer.data(), buffer.size(), 0, true};
  const bool ok = result && (status ? CrossChainEncodeStatusResponse(result, out) : CrossChainEncodeQuoteResponse(result, out));
  const std::string hex = ok && out.ok ? to_hex(buffer.data(), out.used) : "<failed>";
  check(hex == text(expected), label + " protobuf of the JSON answer",
        hex.substr(0, 160) + " != " + text(expected).substr(0, 160));
  ++counts->encoded;
  tj::FreeArena(arena);
}

void run_quotes(const tj::Value* fixture, Counts* counts) {
  const tj::Value* cases = tj::Get(fixture, "cases");
  for (uint32_t i = 0; i < cases->count; ++i) {
    const tj::Value* record = cases->items[i];
    const std::string label = "quotes/" + text(tj::Get(record, "group")) + "/" + text(tj::Get(record, "name"));
    const tj::Value* responses = tj::Get(record, "responses");
    for (uint32_t r = 0; tj::IsArray(responses) && r < responses->count; ++r)
      encode_check(label + "[" + std::to_string(r) + "]", responses->items[r], false, counts);
    if (tj::Get(record, "concurrent") || text(tj::Get(record, "method")) != "POST" ||
        text(tj::Get(record, "target")).rfind("/v1/exchange/cross-chain/quotes", 0) != 0 || before_validation(record)) {
      ++counts->skipped;
      continue;
    }
    encode_check(label, record, false, counts);
    tj::Arena arena = tj::GrowingArena(1 << 20);
    bool parsed = true;
    const tj::Value* input = body_value(record, arena, false, &parsed);
    check(parsed, label, "the body parser refused a body TypeScript validated");
    CrossChainQuotePlan plan;
    CrossChainPlanQuote(input, arena, &plan);
    ++counts->plans;
    if (strict::ProtoReject("quotes", text(tj::Get(record, "name")).c_str())) {
      check(plan.kind == CrossChainPlanKind::kBadRequest, label, "strict protobuf request refusal");
      ++counts->refusals;
      tj::FreeArena(arena);
      continue;
    }
    const int status = static_cast<int>(tj::Get(record, "status")->number);
    const std::string response = text(tj::Get(record, "responseText"));
    if (status == 400 && response == "{\"error\":\"BAD_REQUEST\"}") {
      check(plan.kind == CrossChainPlanKind::kBadRequest, label, "expected zod/protobuf 400");
      ++counts->refusals;
    } else if (status == 400) {
      std::string listed = "{\"error\":\"UNKNOWN_CROSS_CHAIN_PROVIDER\",\"providers\":[";
      for (size_t k = 0; k < plan.unknown_count; ++k) {
        listed += k ? ",\"" : "\"";
        listed += text(plan.unknown[k]);
        listed += "\"";
      }
      listed += "]}";
      check(plan.kind == CrossChainPlanKind::kUnknownProviders && listed == response, label, std::string(listed).append(" != ").append(response));
      ++counts->unknown;
    } else {
      check(plan.kind == CrossChainPlanKind::kDispatch, label, "expected provider work");
      std::vector<std::string> mine;
      for (size_t k = 0; k < plan.provider_count; ++k) mine.push_back(fortis::exchange::cross_chain::kProviders[plan.providers[k]]->Id());
      const std::vector<std::string> theirs = attempt_ids(record, arena);
      std::string a, b;
      for (const auto& id : mine) { a += id; a += ","; }
      for (const auto& id : theirs) { b += id; b += ","; }
      check(status != 200 || a == b, label, std::string("providers ").append(a).append(" != attempts ").append(b));
      ++counts->dispatch;
      const std::string url = first_url(record, "https://li.quest/v1/quote?");
      if (plan.kind == CrossChainPlanKind::kDispatch && !url.empty()) {
        auto query = query_of(url);
        const auto& q = plan.request;
        check(query["fromChain"] == lifi_chain(text(q.from.chain_id)) && query["toChain"] == lifi_chain(text(q.to.chain_id)) &&
                  query["fromToken"] == usv(text(q.from.token)) && query["toToken"] == usv(text(q.to.token)) &&
                  query["fromAmount"] == text(q.amount_in) && query["fromAddress"] == usv(text(q.from.address)) &&
                  query["toAddress"] == usv(text(q.to.address)) && query["slippage"] == slippage_fraction(q.slippage_bps),
              label, "the LI.FI query differs from the plan: " + url.substr(0, 200));
        ++counts->url_checks;
      }
    }
    tj::FreeArena(arena);
  }
}

void run_status(const tj::Value* fixture, Counts* counts) {
  const tj::Value* cases = tj::Get(fixture, "cases");
  for (uint32_t i = 0; i < cases->count; ++i) {
    const tj::Value* record = cases->items[i];
    const std::string label = "status/" + text(tj::Get(record, "group")) + "/" + text(tj::Get(record, "name"));
    const tj::Value* responses = tj::Get(record, "responses");
    for (uint32_t r = 0; tj::IsArray(responses) && r < responses->count; ++r)
      encode_check(label + "[" + std::to_string(r) + "]", responses->items[r], true, counts);
    if (tj::Get(record, "concurrent") || text(tj::Get(record, "method")) != "POST" ||
        text(tj::Get(record, "target")).rfind("/v1/exchange/cross-chain/status", 0) != 0 || before_validation(record)) {
      ++counts->skipped;
      continue;
    }
    encode_check(label, record, true, counts);
    tj::Arena arena = tj::GrowingArena(1 << 20);
    bool parsed = true;
    const tj::Value* input = body_value(record, arena, true, &parsed);
    check(parsed, label, "the body parser refused a body TypeScript validated");
    CrossChainStatusPlan plan;
    CrossChainPlanStatus(input, arena, &plan);
    ++counts->plans;
    if (strict::ProtoReject("status", text(tj::Get(record, "name")).c_str())) {
      check(plan.kind == CrossChainPlanKind::kBadRequest, label, "strict protobuf request refusal");
      ++counts->refusals;
      tj::FreeArena(arena);
      continue;
    }
    const int status = static_cast<int>(tj::Get(record, "status")->number);
    const std::string response = text(tj::Get(record, "responseText"));
    if (status == 400 && response == "{\"error\":\"BAD_REQUEST\"}") {
      check(plan.kind == CrossChainPlanKind::kBadRequest, label, "expected zod/protobuf 400");
      ++counts->refusals;
    } else if (status == 400) {
      check(plan.kind == CrossChainPlanKind::kUnknownProviders &&
                response == "{\"error\":\"UNKNOWN_CROSS_CHAIN_PROVIDER\"}",
            label, response);
      ++counts->unknown;
    } else {
      check(plan.kind == CrossChainPlanKind::kDispatch, label, "expected provider work");
      ++counts->dispatch;
      if (plan.kind != CrossChainPlanKind::kDispatch) { tj::FreeArena(arena); continue; }
      const std::string id = fortis::exchange::cross_chain::kProviders[plan.provider]->Id();
      if (status == 503)
        check(response == "{\"error\":\"PROVIDER_NOT_CONFIGURED\",\"provider\":\"" + id + "\"}", label, response);
      const std::string url = first_url(record, "https://li.quest/v1/status?");
      check(url.empty() || id == "lifi", label, "LI.FI asked for provider " + id);
      if (!url.empty()) {
        auto query = query_of(url);
        const auto& q = plan.request;
        const bool bridge_ok = q.bridge_id ? query.count("bridge") && query["bridge"] == text(q.bridge_id)
                                           : !query.count("bridge");
        check(query["txHash"] == usv(text(q.tx_hash)) && query["fromChain"] == lifi_chain(text(q.from_chain_id)) &&
                  query["toChain"] == lifi_chain(text(q.to_chain_id)) && bridge_ok,
              label, "the LI.FI query differs from the plan: " + url.substr(0, 200));
        ++counts->url_checks;
      }
    }
    tj::FreeArena(arena);
  }
}

const tj::Value* load(const char* path, tj::Arena& arena) {
  const std::vector<uint8_t> data = read_file(path);
  const tj::Value* fixture = data.empty() ? nullptr : tj::Parse(arena, data.data(), data.size());
  if (!fixture) fprintf(stderr, "cannot read %s\n", path);
  return fixture;
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc != 3) {
    fprintf(stderr, "usage: %s cross_chain_quotes_edges.json cross_chain_status_edges.json\n", argv[0]);
    return 2;
  }
  tj::Arena arena = tj::GrowingArena(64u << 20);
  const tj::Value* quotes = load(argv[1], arena);
  const tj::Value* status = load(argv[2], arena);
  if (!quotes || !status) return 2;
  Counts q, s;
  run_quotes(quotes, &q);
  run_status(status, &s);
  printf("quotes (R41): %d plans (%d bare refusals, %d unknown-provider refusals, %d dispatches), %d LI.FI queries, "
         "%d protobuf re-encodings, %d records skipped\n",
         q.plans, q.refusals, q.unknown, q.dispatch, q.url_checks, q.encoded, q.skipped);
  printf("status (R42): %d plans (%d bare refusals, %d unknown-provider refusals, %d dispatches), %d LI.FI queries, "
         "%d protobuf re-encodings, %d records skipped\n",
         s.plans, s.refusals, s.unknown, s.dispatch, s.url_checks, s.encoded, s.skipped);
  printf("cross_chain_post_test: %d checks, %d failures\n", g_checks, g_failures);
  tj::FreeArena(arena);
  return g_failures ? 1 : 0;
} catch (...) {
  return fortis::test::ReportException();
}
