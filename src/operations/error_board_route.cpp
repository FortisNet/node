#include "error_board.h"
#include "device_enrollment.h"
#include "http_server.h"
#include "network_registry.h"
#include "token_json.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace fortis::operations {
namespace {
constexpr char kMappingPath[] = "/v1/prices/missing-identifiers";
}

void ErrorBoard::MissingIdentifiers(const http::HttpRequest& request, http::HttpReply& reply) {
  const auto policy = auth::AuthorizeDeviceRequest(request, 1, attestation_required_);
  const auto respond = [&](int status, const char* reason, const char* body) {
    auto response = http::JsonResponse(status, reason, body);
    response.rate_limit_cost = policy.rate_limit_cost;
    response.rate_limit_remaining = policy.rate_limit_remaining;
    response.retry_after = policy.retry_after;
    reply.Send(response);
  };
  if (policy.status) { respond(policy.status, policy.reason, policy.error_body); return; }
  if (request.body_length > 1024) {
    respond(413, "Payload Too Large", "{\"error\":\"BODY_TOO_LARGE\"}"); return;
  }
  if (!request.json_content_type) {
    respond(415, "Unsupported Media Type", "{\"error\":\"UNSUPPORTED_MEDIA_TYPE\"}"); return;
  }
  std::array<std::uint8_t, 4096> memory{};
  codec::tj::Arena arena{memory.data(), memory.size()};
  const auto* body = codec::tj::Parse(arena, reinterpret_cast<const std::uint8_t*>(request.body), request.body_length);
  const auto* symbols = codec::tj::Get(body, "symbols");
  if (!codec::tj::IsObject(body) || body->count != 1 || !codec::tj::IsArray(symbols) ||
      !symbols->count || symbols->count > 16) {
    respond(400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}"); return;
  }
  for (std::uint32_t index = 0; index < symbols->count; ++index) {
    const auto* symbol = symbols->items[index];
    if (!codec::tj::IsString(symbol) || !symbol->length || symbol->length > 16 ||
        !std::all_of(symbol->text, symbol->text + symbol->length, [](unsigned char byte) {
          return (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '.';
        })) {
      respond(400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}"); return;
    }
  }
  for (std::uint32_t index = 0; index < symbols->count; ++index) {
    const auto* symbol = symbols->items[index];
    const std::string_view identifier(symbol->text, symbol->length);
    std::size_t network_count = 0;
    const auto* catalog = networks::NativeNetworkRegistry{}.list(&network_count);
    const bool public_symbol = std::any_of(catalog, catalog + network_count,
        [&](const auto& network) { return network.native_symbol && identifier == network.native_symbol; });
    // Only catalogued native symbols are public metadata. Unknown/custom
    // client input still counts as an error, without becoming public text.
    Report("prices", "client_identifier_missing", public_symbol ? identifier : std::string_view{},
        {base::ErrorOrigin::kUnclassified});
  }
  if (!Flush()) {
    respond(503, "Service Unavailable", "{\"error\":\"DIAGNOSTIC_UNAVAILABLE\"}"); return;
  }
  auto response = http::JsonResponse(204, "No Content", "");
  response.omit_content_type = true;
  response.rate_limit_cost = policy.rate_limit_cost;
  response.rate_limit_remaining = policy.rate_limit_remaining;
  reply.Send(response);
}

bool ErrorBoard::RegisterMissingIdentifiers(http::HttpRouter& router, bool attestation_required) {
  attestation_required_ = attestation_required;
  return router.Add<&ErrorBoard::MissingIdentifiers>("POST", kMappingPath, *this);
}

}  // namespace fortis::operations
