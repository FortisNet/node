#pragma once

#include "error_reporter.h"
#include "provider_execution.h"

#include <algorithm>

namespace fortis::providers {

/** Stable public description of a completed dependency call; never response text.
 * details.provider borrows the supplied stable lane label until Report returns.
 * Network/TLS failures are unclassified: they do not prove a provider fault.
 * Local admission, cancellation and shutdown are not third-party failures.
 */
struct ProviderFailure {
  const char* code = nullptr;
  base::ErrorDetails details{};
};

/** Exact individual CoinGecko coin-profile lookup ID, or an empty view.
 * Path is a synchronous borrowed, owner-built path. Lists, contracts, history,
 * query values and malformed slugs are excluded. A 404 is evidence only when
 * the completed response belongs to this exact route, never a batch omission.
 */
inline std::string_view CoinGeckoProfileId(std::string_view path) {
  constexpr std::string_view prefix = "/api/v3/coins/";
  if (!path.starts_with(prefix)) return {};
  path.remove_prefix(prefix.size());
  const auto query = path.find('?');
  const auto id = path.substr(0, query);
  if (id == "list" || id == "markets" || id == "categories" || id == "top_gainers_losers" || id.empty() ||
      id.size() > 100 || id.front() == '-' || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
      }))
    return {};
  return id;
}

inline ProviderFailure DescribeProviderFailure(std::string_view provider, const ProviderExecutionResult& result) {
  ProviderFailure failure;
  if (result.cancelled || result.lane_rejected || result.attempts == 0) return failure;
  if (provider.starts_with("rpc:")) provider = "blockchain_rpc";
  if (provider.starts_with("news-feeds:")) provider = "news_feed";
  failure.details.provider = provider;
  const auto& response = result.response;
  if (response.status >= 100 && response.status <= 599)
    failure.details.http_status = static_cast<std::uint16_t>(response.status);
  if (response.error == http::OutboundError::kNone) {
    if (response.status < 400 || response.status > 599) return failure;
    failure.details.origin = base::ErrorOrigin::kThirdParty;
    failure.code = response.status == 429   ? "http_rate_limited"
                   : response.status >= 500 ? "http_server_error"
                                            : "http_request_rejected";
    return failure;
  }
  failure.details.origin = base::ErrorOrigin::kUnclassified;
  switch (response.error) {
    case http::OutboundError::kResolve: failure.code = "dependency_dns_failure"; break;
    case http::OutboundError::kTimeout: failure.code = "dependency_timeout"; break;
    case http::OutboundError::kConnect: failure.code = "dependency_connect_failure"; break;
    case http::OutboundError::kTls: failure.code = "dependency_tls_failure"; break;
    case http::OutboundError::kHttp: failure.code = "dependency_invalid_response"; break;
    case http::OutboundError::kTooLarge: failure.code = "dependency_response_limit"; break;
    case http::OutboundError::kUnsupportedFraming: failure.code = "dependency_unsupported_framing"; break;
    case http::OutboundError::kInvalidRequest:
    case http::OutboundError::kPrivateAddress:
    case http::OutboundError::kBusy:
      failure.details.origin = base::ErrorOrigin::kNode;
      failure.code = "outbound_local_refusal";
      break;
    case http::OutboundError::kNone: break;
  }
  return failure;
}
}  // namespace fortis::providers
