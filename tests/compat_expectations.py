"""Expected-response rewrites for the behaviours removed on purpose.

The fixtures record the earlier TypeScript implementation's answers. Its
TypeScript-only behaviour is gone (docs/compat-decisions.md, "Replaced
behaviours" and "Expectation mapping in the replay tests"); a replay maps
the recorded answer to this server's answer with the helpers below instead
of re-recording the fixture. Each helper names the compat-decisions.md
entry it implements and changes nothing else.
"""

import json

BAD_REQUEST = b'{"error":"BAD_REQUEST"}'
_ZOD_PREFIX = b'{"error":"BAD_REQUEST","detail":['


def is_zod_refusal(body):
    """True for a recorded zod issue list body (bytes, str or decoded JSON)."""
    if isinstance(body, dict):
        return body.get("error") == "BAD_REQUEST" and isinstance(body.get("detail"), list)
    if isinstance(body, str):
        body = body.encode()
    return isinstance(body, (bytes, bytearray)) and bytes(body).startswith(_ZOD_PREFIX)


def zod_refusal(body):
    """zod-issues: a schema refusal is {"error":"BAD_REQUEST"}, no detail.

    Returns the replacement in the representation it was given (bytes, str or
    decoded JSON), or None when `body` is not a zod issue list.
    """
    if not is_zod_refusal(body):
        return None
    if isinstance(body, dict):
        return json.loads(BAD_REQUEST)
    if isinstance(body, str):
        return BAD_REQUEST.decode()
    return BAD_REQUEST


def rewrite_headers(headers, new_length):
    """Copy of `headers` with content-length set to `new_length`."""
    if headers is None:
        return headers
    out = dict(headers)
    for key in list(out):
        if key.lower() == "content-length":
            out[key] = str(new_length)
    return out


def zod_expected(status, headers, body, head=False, head_is_zod=False):
    """(status, headers, body) of a recorded answer as this server gives it.

    A recorded zod refusal becomes {"error":"BAD_REQUEST"} with its
    content-length; a HEAD answer (empty body) is rewritten when the caller
    knows its GET twin was a zod refusal (`head_is_zod`). Anything else is
    returned unchanged.
    """
    if status != 400:
        return status, headers, body
    if head and head_is_zod:
        return status, rewrite_headers(headers, len(BAD_REQUEST)), body
    replacement = zod_refusal(body)
    if replacement is None:
        return status, headers, body
    return status, rewrite_headers(headers, len(BAD_REQUEST)), replacement


UNSUPPORTED_MEDIA_TYPE = b'{"error":"UNSUPPORTED_MEDIA_TYPE"}'
JSON_UTF8 = "application/json; charset=utf-8"

# fastify-errors: the recorded Fastify / Node envelopes and
# the status and code this server answers instead. The body-framing ones
# (415, the empty/invalid JSON 400s, the schema's FST_ERR_VALIDATION) are
# judged after the device policy now, so they are charged; 414 and the raw
# non-ASCII 400 stay before any hook.
FASTIFY_CODES = {
    "FST_ERR_CTP_INVALID_MEDIA_TYPE": (415, "UNSUPPORTED_MEDIA_TYPE", True),
    "FST_ERR_CTP_EMPTY_JSON_BODY": (400, "BAD_REQUEST", True),
    "FST_ERR_CTP_INVALID_JSON_BODY": (400, "BAD_REQUEST", True),
    "FST_ERR_CTP_INVALID_CONTENT_LENGTH": (400, "BAD_REQUEST", True),
    "FST_ERR_VALIDATION": (400, "BAD_REQUEST", True),
    "FST_ERR_MAX_PARAM_LENGTH": (414, "URI_TOO_LONG", False),
    "CLIENT_ERROR": (400, "BAD_REQUEST", False),
}


def fastify_code(body):
    """The Fastify code of a recorded refusal body ("CLIENT_ERROR" for Node's
    clientError), or None."""
    if isinstance(body, (bytes, bytearray)):
        body = bytes(body).decode("utf-8", "replace")
    if isinstance(body, str):
        try:
            body = json.loads(body)
        except ValueError:
            return None
    if not isinstance(body, dict):
        return None
    code = body.get("code")
    if isinstance(code, str) and code in FASTIFY_CODES and "statusCode" in body:
        return code
    if body == {"error": "Bad Request", "message": "Client Error", "statusCode": 400}:
        return "CLIENT_ERROR"
    return None


def refusal(code):
    return ('{"error":"%s"}' % code).encode()


def auth_refusal(request_headers):
    """The device policy's answer to a request that carries no usable device
    credential (the cases a replay can tell from the request alone), or None:
    any Origin → 403, no "Bearer " authorization → 401 UNAUTHORIZED."""
    headers = {k.lower(): v for k, v in (request_headers or {}).items()}
    if "origin" in headers:
        return 403, b'{"error":"BROWSER_ORIGIN_FORBIDDEN"}'
    authorization = headers.get("authorization")
    if not authorization or not authorization.startswith("Bearer ") or not authorization[7:].startswith("fintel_dev_"):
        return 401, b'{"error":"UNAUTHORIZED"}'
    return None


def fastify_expected(status, headers, body, request_headers=None):
    """fastify-errors: a recorded Fastify/Node envelope as this server answers
    it. Returns None when `body` is not one; else (status, headers, body,
    charged): the new code in {"error":...} as application/json;
    charset=utf-8; given `request_headers`, a body-framing refusal of a
    request without a usable credential is the device policy's refusal
    instead (auth-first; callers whose requests all carry the device bearer
    pass None); `charged`
    says the answer carries the quota headers (the cost is the route's)."""
    code = fastify_code(body)
    if code is None:
        return None
    new_status, new_code, framing = FASTIFY_CODES[code]
    kept = {k: v for k, v in (headers or {}).items()
            if k.lower() not in ("x-rate-limit-cost", "x-rate-limit-remaining", "retry-after")}
    if "content-type" in kept:
        kept["content-type"] = JSON_UTF8
    if framing and request_headers is not None:
        refused = auth_refusal(request_headers)
        if refused:
            return refused[0], rewrite_headers(kept, len(refused[1])), refused[1], False
    payload = refusal(new_code)
    return new_status, rewrite_headers(kept, len(payload)), payload, framing


def is_removed_body_media(content_type):
    """form-bodies: text/plain and form bodies are 415 (since fastify-errors,
    after the device policy)."""
    if not content_type:
        return False
    media = content_type.split(";")[0].strip().lower()
    return media in ("text/plain", "application/x-www-form-urlencoded")


def is_prototype_poisoning(request_body, oracle_body):
    """js-prototype-keys: a JSON body with `__proto__` (or constructor.prototype)
    the oracle refused in its body parser now reaches the route's schema."""
    if isinstance(oracle_body, dict):
        oracle_body = json.dumps(oracle_body)
    if isinstance(oracle_body, (bytes, bytearray)):
        oracle_body = bytes(oracle_body).decode("utf-8", "replace")
    if not oracle_body or "FST_ERR_CTP_INVALID_JSON_BODY" not in oracle_body:
        return False
    if isinstance(request_body, (bytes, bytearray)):
        request_body = bytes(request_body).decode("utf-8", "replace")
    return "__proto__" in (request_body or "") or '"prototype"' in (request_body or "")


class QuotaShift:
    """The quota a replay sees after the two request-side changes: a
    removed-media request the oracle charged is not charged any more, a
    prototype-key request the oracle refused before the quota now is. Later
    recorded x-rate-limit-remaining values of the same bearer move by the
    difference; a recorded "0" stays "0" (the oracle's count is unknown)."""

    def __init__(self):
        self.shift = {}

    def reset(self):
        self.shift.clear()

    def uncharged(self, bearer, cost):
        self.shift[bearer] = self.shift.get(bearer, 0) + int(cost)

    def charged(self, bearer, cost):
        self.shift[bearer] = self.shift.get(bearer, 0) - int(cost)

    def remaining(self, bearer, recorded):
        if recorded is None or not self.shift.get(bearer):
            return recorded
        value = int(recorded)
        if value <= 0:
            return recorded
        return str(max(0, value + self.shift[bearer]))


def remap(status, headers, body, content_type, request_body, bearer, shift, schema_refusal=BAD_REQUEST,
          request_headers=None):
    """One recorded POST answer as this server gives it.

    Returns (status, headers, body, remaining_known). `body` is bytes.
    `schema_refusal` is the route's own answer to an unknown key (the
    `__proto__` case). `shift` is a QuotaShift carried across a replay.
    `request_headers` (optional) let a body-framing refusal of an
    unauthenticated request be expected as the device policy's (auth-first).
    """
    if isinstance(body, str):
        body = body.encode("utf-8", "surrogatepass")
    if is_prototype_poisoning(request_body, body):
        # The cost is the route's; the caller reads it from the answer.
        return 400, dict(rewrite_headers(headers, len(schema_refusal)), **{"x-rate-limit-cost": None}), \
            schema_refusal, False
    if is_removed_body_media(content_type) or fastify_code(body):
        # form-bodies + fastify-errors: refused by the body parser after the
        # device policy, charged (the cost is read from the answer).
        if headers.get("x-rate-limit-cost"):
            shift.uncharged(bearer, headers["x-rate-limit-cost"])
        if not fastify_code(body):
            body = UNSUPPORTED_MEDIA_TYPE_FASTIFY
        new_status, new_headers, new_body, charged = fastify_expected(status, headers, body, request_headers)
        if charged:
            new_headers = dict(new_headers, **{"x-rate-limit-cost": None})
        return new_status, new_headers, new_body, not charged
    status, headers, body = zod_expected(status, headers, body)
    if "x-rate-limit-remaining" in headers:
        headers = dict(headers, **{"x-rate-limit-remaining": shift.remaining(bearer, headers["x-rate-limit-remaining"])})
    return status, headers, body, True


def settle_charged(headers, answer_headers, bearer, shift):
    """Completes a `remap` result whose cost is unknown (the `__proto__` case,
    marked by an x-rate-limit-cost of None): this server must have charged a
    positive cost and reported what remains. The answer's own values are
    expected (the oracle recorded none) and moved into `shift`; a missing or
    non-positive cost is expected as a value no answer carries, so the
    comparison fails instead of accepting an uncharged refusal."""
    if headers.get("x-rate-limit-cost", "") is not None:
        return headers
    cost = answer_headers.get("x-rate-limit-cost")
    remaining = answer_headers.get("x-rate-limit-remaining")
    if not (cost and cost.isdigit() and int(cost) > 0 and remaining is not None):
        return dict(headers, **{"x-rate-limit-cost": "<a positive cost after the quota>",
                                "x-rate-limit-remaining": "<the remaining quota>"})
    shift.charged(bearer, cost)
    return dict(headers, **{"x-rate-limit-cost": cost, "x-rate-limit-remaining": remaining})


def check_removed_request(content_type, request_body, oracle_body, status, headers, payload,
                          schema_refusal=BAD_REQUEST, request_headers=None):
    """None when the recorded request is unaffected by those removals; otherwise the
    list of problems with this server's answer to a text/plain or form body
    or a body-framing refusal (the body parser's refusal after the device
    policy, charged; or the policy's own refusal without a credential), or to
    a `__proto__` key (the route's schema refusal after the quota)."""
    if is_prototype_poisoning(request_body, oracle_body):
        problems = []
        if status != 400 or payload != schema_refusal or "x-rate-limit-cost" not in headers:
            problems.append(f"__proto__ key: {status} {payload[:200]!r} is not the schema refusal after the quota")
        return problems
    if is_removed_body_media(content_type) or fastify_code(oracle_body):
        problems = []
        recorded = oracle_body if fastify_code(oracle_body) else UNSUPPORTED_MEDIA_TYPE_FASTIFY
        expected = fastify_expected(None, {}, recorded, request_headers)
        if status != expected[0] or payload != expected[2]:
            problems.append(f"body framing: {status} {payload[:200]!r} is not {expected[0]} {expected[2]!r}")
        if expected[3] and "x-rate-limit-cost" not in headers:
            problems.append("body framing: refused before the quota")
        return problems
    return None


UNSUPPORTED_MEDIA_TYPE_FASTIFY = (b'{"statusCode":415,"code":"FST_ERR_CTP_INVALID_MEDIA_TYPE",'
                                  b'"error":"Unsupported Media Type","message":"Unsupported Media Type"}')


def fastify_case(case, text_key="responseText", request_headers=None):
    """A recorded case dict whose answer is a Fastify/Node envelope, as this
    server answers it (fastify_expected); any other case is returned as is.
    The quota headers of a charged refusal cannot be known from the record:
    `x-rate-limit-cost` is set to None for the caller to settle."""
    if fastify_code(case.get(text_key)) is None:
        return case
    status, headers, body, charged = fastify_expected(case["status"], case.get("headers") or {}, case[text_key],
                                                      request_headers)
    if charged:
        headers = dict(headers, **{"x-rate-limit-cost": None})
    return dict(case, status=status, headers=headers, **{text_key: body.decode()})


def malformed_path_target(case):
    """opaque-params: True when the recorded request path holds a value the
    router now refuses with its 404 before any handler: an empty segment, a
    decoded control character or DEL, or a segment over 512 decoded bytes.
    `case` names its target as `targetHex`, `target` or `url`."""
    from urllib.parse import unquote_to_bytes
    if "targetHex" in case:
        raw = bytes.fromhex(case["targetHex"])
    else:
        raw = (case.get("target") or case.get("url") or "").encode("utf-8", "surrogateescape")
    segments = raw.split(b"?", 1)[0].split(b"/")[1:]
    for segment in segments:
        decoded = unquote_to_bytes(segment)
        if not decoded or len(decoded) > 512 or any(b < 0x20 or b == 0x7F for b in decoded):
            return True
    return False


ROUTER_NOT_FOUND = b'{"error":"NOT_FOUND"}'


def router_refused(status, headers, payload, observed):
    """The router's 404 for a malformed path value: no hook, no quota, no
    provider request."""
    head = payload == b"" and headers.get("content-length") == str(len(ROUTER_NOT_FOUND))  # a HEAD
    return status == 404 and (payload == ROUTER_NOT_FOUND or head) and "x-rate-limit-cost" not in headers and \
        not observed


def surrogate_text(text):
    """lmdb-msgpack: a record or answer holding a lone surrogate, normalized
    for comparison with the oracle's msgpack copy. NFT and tx records are
    stored as written now, so a cached answer carries the provider's lone
    surrogate (a \\udXXX JSON escape, or one U+FFFD in protobuf) where the
    oracle answered three U+FFFD (one in a string of 64 units or more). Both
    forms become one U+FFFD; the media keys derived from such a URL (sha256
    of it) become "<media key>". Use only for cases named as holding a lone
    surrogate."""
    import re
    if isinstance(text, (bytes, bytearray)):
        text = bytes(text).decode("utf-8", "surrogatepass")
    pair = r"\\ud[89ab][0-9a-f]{2}\\ud[c-f][0-9a-f]{2}"
    text = re.sub(r"(%s)|\\ud[89a-f][0-9a-f]{2}" % pair, lambda m: m.group(1) or "�", text,
                  flags=re.IGNORECASE)
    text = re.sub("[\ud800-\udfff]", "�", text)
    text = re.sub("�+", "�", text)
    text = re.sub(r"/v1/media/[0-9a-f]{32}", "/v1/media/<media key>", text)
    return re.sub(r'"(key|logoMediaKey|imageMediaKey)": ?"[0-9a-f]{32}"', r'"\1":"<media key>"', text)


def byte_order_ties(rows, score_of, key="id"):
    """locale-compare: a recorded ranking whose equal-score ties TypeScript
    broke with localeCompare (ICU root collation), re-broken in byte order of
    the UTF-8 id. `rows` keep their positional fields (rank, bucket,
    interval); only the ids move. `score_of(id)` is the ranking score; the
    stable sort leaves rows with different scores where the oracle put them."""
    ids = [row[key] for row in rows]
    order = sorted(ids, key=lambda i: (-score_of(i), i.encode("utf-8")))
    return [dict(row, **{key: i}) for row, i in zip(rows, order)]
