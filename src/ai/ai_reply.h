#pragma once

#include "token_json.h"

#include <stddef.h>
#include <stdint.h>

// The OpenAI Responses exchange of an AI conversation turn, ported from
// the TypeScript OpenAiIntelligenceReplyGenerator and the outbound half of
// the TypeScript checkOutboundMessage. Pure functions:
// the HTTP call itself belongs to AiConversationService.
namespace fortis::ai {
struct TokenUsage;

constexpr size_t kMaxTurns = 20;               // turns.slice(-20)
constexpr size_t kTurnMaxUnits = 8000;         // turn.body.slice(0, 8_000)
constexpr size_t kReplyMaxUnits = 12000;       // text.slice(0, 12_000)
constexpr int kMaxOutputTokens = 1500;

extern const char kOpenAiHost[];               // api.openai.com
extern const char kOpenAiPath[];               // /v1/responses
extern const char kSafeReply[];                // the suppression text

struct Turn {
  bool assistant;
  const char* body;  // WTF-8
  size_t length;
};

extern const char kAnthropicHost[];
extern const char kAnthropicPath[];
// Messages API: output_config.effort is low, medium, high or max for Opus 4.6.
bool BuildAnthropicRequest(const char* model, const char* effort, int max_tokens,
                           const fortis::codec::tj::Value* snapshot, const Turn* turns, size_t count,
                           fortis::codec::tj::Arena& arena, const uint8_t** body, size_t* length);
bool ParseAnthropicReply(int status, const uint8_t* body, size_t length, fortis::codec::tj::Arena& arena,
                         const char** text, size_t* text_length, TokenUsage* usage = nullptr);

// Stable, short failure codes sent in x-ai-reply-status. Provider text is
// classified on the server and never forwarded to the wallet.
const char* ClassifyAnthropicFailure(int status, const uint8_t* body, size_t length,
                                     fortis::codec::tj::Arena& arena);
bool IsFailedReplyStatus(const char* status);

// JSON.stringify({ model, store: false, max_output_tokens: 1500,
// instructions, input }) with the source's safety instructions, the
// bounded portfolio context as the first (user) input item and the newest
// 20 turns, each cut to 8,000 UTF-16 units.
bool BuildOpenAiRequest(const char* model, size_t model_length, const fortis::codec::tj::Value* snapshot,
                        const Turn* turns, size_t count, fortis::codec::tj::Arena& arena, const uint8_t** body,
                        size_t* length);

// response.ok, response.json(), status 'completed', the output_text parts of
// message items joined by "\n", trimmed, non-empty, sliced to 12,000 units.
// False is ReplyUnavailableError. The text is WTF-8 in the arena.
bool ParseOpenAiReply(int status, const uint8_t* body, size_t length, fortis::codec::tj::Arena& arena,
                      const char** text, size_t* text_length);

// checkOutboundMessage(text).violations includes 'requests_secret' or
// 'contains_secret'. Fails closed: an internal failure (allocation, the
// regular expression step budget) also answers true.
bool OutboundSuppressed(const char* text, size_t length);

// Compiles the outbound patterns once; false if the engine refuses one.
bool OutboundPatternsReady();

// SECRET_REQUEST_PATTERNS, one result per pattern in source order, over
// already-folded UTF-16 text (the operator reply check needs each pattern's
// reason). A step-budget exhaustion reads as a match (fail closed). False
// when the patterns are unavailable.
constexpr size_t kSecretRequestPatternCount = 7;
bool SecretRequestMatches(const uint16_t* text, size_t units, bool matched[kSecretRequestPatternCount]);

}  // namespace fortis::ai
