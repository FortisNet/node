# AI conversations: the portfolio snapshot attached to a message

This document describes the wallet snapshot that a client attaches to each message it sends to the AI assistant: what it contains, how the node validates and stores it, how much of it reaches the model and how a reply is tracked. Client authors and developers who work on `src/ai` need it.

## What the client sends

Every user message carries an immutable snapshot of the device's **local** state: its wallet, network, tracked asset, balance and price records. Sending a question does not start a chain scan or a price request on the node.

The request that opens a conversation and every later message request contain `snapshot_json` and its SHA-256 digest. The node checks ownership, schema, the links between snapshots and the digest. It then stores one normalized copy at `ai/snapshot/{conversationId}/{sha256}`. Message history returns the text of the messages and does not repeat the snapshot. Assistant messages have no snapshot.

A snapshot must not contain a private key, a recovery phrase or a PIN.

## Snapshot version 2

Version 2 describes how complete the client's knowledge is. Version 1 is still accepted for stored conversations and older clients.

| Part | Content |
| --- | --- |
| `coverage` | One entry for every visible pair of wallet and network. `observed` means that a saved balance observation exists, including an observed zero. `unknown` means that none exists. `knownZero` is true only for an observed network without any saved native or token holding. Each entry carries the observation time, the source and a stale flag |
| `holdings` | Exact amounts in base units as integer strings where available, the displayed decimal amount and where its precision comes from. A position without raw units has `rawAmount: null` and `precision: estimated` |
| `prices` | For each priced asset: the cached USD quote as a string, the lookup key, the provider, the observation time and a stale flag. An asset without a price stays unpriced |

Rules for readers of a snapshot:

- `balancesComplete=false` and `unknown` coverage never mean a verified zero.
- An estimated position can exist on a network with unknown coverage. It makes totals partial.
- All figures are observations supplied by the client. The node does not verify them against a chain.

## What the model receives

- The node passes the latest snapshot to the model, limited to 40,000 UTF-16 code units and to 300 entries each of holdings, prices and coverage.
- It adds the latest 20 text turns of the conversation.
- The snapshot is a separate data item in the user's turn. The instructions tell the model never to follow instructions found inside it.
- The node computes `cachedPricedTotalUsd` with decimal integer arithmetic. It sets `totalPartial` when balances are incomplete, a network is unknown or stale, a quote is stale or a holding has no price.

## Replies and retries

Accepting a message is durable and asynchronous.

1. The node stores the user message and a job record in one storage frame and answers `201`.
2. A background worker asks the provider for a reply.
3. On success the worker stores the assistant message and removes the job record in one frame.
4. On a provider failure the job becomes `failed`. The authenticated retry route (`POST /v1/ai/conversations/{id}/messages/{messageId}/retry`) queues the same user message again.

History pagination is unaffected. The newest page of the history carries the headers `X-AI-Reply-For` and `X-AI-Reply-Status`, from which a client shows that a reply is pending, complete or failed.
