# AI usage statistics page

This document describes `GET /ai/stats`, a public page with aggregate counts of AI conversation activity on a node: what it counts, how the counts are kept, where they are incomplete and what the page does not reveal. Operators who publish the page and developers who work on `src/ai/ai_stats.cpp` need it.

## The page

`GET /ai/stats` is a public, read-only HTML page. To build it the node makes at most 32 point reads in its store: one all-time aggregate, 24 hourly buckets and seven daily buckets. It scans no conversations and no messages. If an aggregate is corrupt or unreadable, the page answers `503` instead of showing an incomplete total.

The page shows the last 24 hours and the last seven days.

## How counts are kept

- Each new user or assistant message updates the all-time, the UTC-hour and the UTC-day aggregate in the same checksummed, synced storage frame as the message itself. A count therefore exists exactly when its message does.
- A repeated `clientMessageId` and a second reply worker for the same message return the existing row and do not count again.
- A wallet owner counts once in each period in which it was active. Membership keys contain only the service's HMAC owner ID. They contain no wallet address and no message text.
- All-time membership is kept with the durable owner index. Hourly markers and counters expire after three days, daily ones after 32 days.

Counting starts with the first message stored under the key prefix `ai/stats/v1`. Earlier conversations and messages are not counted afterwards. The page states the time at which recording started.

## Token counts

Token counts come from the `usage` object of a completed Anthropic Messages API response and are stored when the assistant message is stored.

| Count | Source fields |
| --- | --- |
| Input tokens | `input_tokens` + `cache_creation_input_tokens` + `cache_read_input_tokens` |
| Output tokens | `output_tokens` |

The page also counts the replies for which usage was recorded.

Usage is not recorded for a failed request, for a malformed response or when the process stops before the assistant message is stored. The totals can therefore be lower than the provider's bill. Treat the page as an operational estimate, not as an invoice. [Anthropic's Messages API reference](https://platform.claude.com/docs/en/api/messages/create) defines the usage fields.

## Privacy

The page shows aggregate counts only. It never shows owner IDs, device IDs, prompts, replies, snapshots, API keys or usage per user.

The counts are exact, also when a group is small. Do not publish the page where a privacy policy requires small groups to be suppressed.
