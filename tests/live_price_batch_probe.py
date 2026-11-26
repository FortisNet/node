#!/usr/bin/env python3
"""POST /v1/prices/history helpers for the opt-in guest live price probe.

Pure request encoding, a bounded protobuf decoder for the shared
fortisnet.intelligence.v1 schema, the batch window plan and the batch reply
classifier. No network or Docker; covered offline by
live_price_batch_probe_test.py, which also checks the schema table below
against proto/schema/fortisnet/intelligence/v1/prices.proto and
the decoder against the frozen TypeScript protobuf replies.
"""

import json
import struct

import live_price_canary as probe

DAY = 86400
BATCH_PATH = "/v1/prices/history"
BATCH_ID = "bitcoin"
BATCH_INTERVAL = "1d"
JSON_TYPE = "application/json"
PROTOBUF_TYPE = "application/protobuf"
# (route name, request and response media type).
BATCH_ROUTES = (("batch-json", JSON_TYPE), ("batch-pb", PROTOBUF_TYPE))

# --- schema -------------------------------------------------------------------
#
# message -> {field number: (JSON name, type, label)}. Types are scalar names
# or message names; label is "" (implicit presence), "optional", "repeated" or
# "oneof". Only the messages a batch reply or request carries are listed.

SCHEMA = {
    "CacheInfo": {
        1: ("hit", "bool", ""), 2: ("timestamp", "string", "optional"),
        3: ("partial", "bool", "")},
    "PricePoint": {1: ("unixSeconds", "int64", ""), 2: ("value", "double", "")},
    "PriceResolutionAvailability": {
        1: ("interval", "string", ""), 2: ("from", "int64", ""), 3: ("to", "int64", ""),
        4: ("days", "double", "")},
    "PriceAvailability": {
        1: ("from", "int64", "optional"), 2: ("to", "int64", "optional"),
        3: ("days", "double", ""),
        4: ("resolutions", "PriceResolutionAvailability", "repeated")},
    "PriceCoverage": {1: ("cachedPoints", "uint32", ""), 2: ("fetchedPoints", "uint32", "")},
    "PriceWindow": {
        1: ("from", "int64", "optional"), 2: ("to", "int64", "optional"),
        3: ("days", "double", "")},
    "PriceHistory": {
        1: ("id", "string", ""), 2: ("interval", "string", ""),
        3: ("points", "PricePoint", "repeated"), 4: ("volumes", "PricePoint", "repeated"),
        5: ("cache", "CacheInfo", ""), 6: ("coverage", "PriceCoverage", ""),
        7: ("window", "PriceWindow", ""), 8: ("availability", "PriceAvailability", "")},
    "PriceHistoryBatchRequest": {
        1: ("ids", "string", "repeated"), 2: ("from", "int64", ""), 3: ("to", "int64", ""),
        4: ("interval", "string", "optional")},
    "PriceHistoryFailure": {
        1: ("error", "string", ""), 2: ("availability", "PriceAvailability", "")},
    "PriceHistoryBatchEntry": {
        1: ("id", "string", ""), 2: ("history", "PriceHistory", "oneof"),
        3: ("failure", "PriceHistoryFailure", "oneof")},
    "PriceHistoryBatchResponse": {
        1: ("from", "int64", ""), 2: ("to", "int64", ""), 3: ("interval", "string", ""),
        4: ("series", "PriceHistoryBatchEntry", "repeated"), 5: ("cache", "CacheInfo", "")},
}
SCALAR_WIRE = {"bool": 0, "int64": 0, "uint32": 0, "string": 2, "double": 1}
SCALAR_DEFAULT = {"bool": False, "int64": 0, "uint32": 0, "string": "", "double": 0.0}

# Decoder bounds: above every native reply (1 MiB response buffer, nesting
# depth 4, 50 IDs x 3,700 candles x 2 point lists).
MAX_PROTO_BYTES = probe.LOCAL_READ_CAP
MAX_PROTO_DEPTH = 8
MAX_PROTO_FIELDS = 400_000


class ProtoError(ValueError):
    pass


# --- protobuf encoding ----------------------------------------------------------

def _varint(value: int) -> bytes:
    value &= (1 << 64) - 1  # negative int64 is ten bytes of two's complement
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def _tag(field: int, wire: int) -> bytes:
    return _varint(field << 3 | wire)


def encode_batch_request(ids, start, end, interval=None) -> bytes:
    """protobuf-es toBinary(PriceHistoryBatchRequest): field order, implicit
    zero int64 omitted, the optional interval written whenever it is set."""
    out = bytearray()
    for item in ids:
        data = item.encode("utf-8")
        out += _tag(1, 2) + _varint(len(data)) + data
    if start:
        out += _tag(2, 0) + _varint(start)
    if end:
        out += _tag(3, 0) + _varint(end)
    if interval is not None:
        data = interval.encode("utf-8")
        out += _tag(4, 2) + _varint(len(data)) + data
    return bytes(out)


def encode_batch_json(ids, start, end, interval) -> bytes:
    return json.dumps({"ids": list(ids), "from": start, "to": end, "interval": interval},
                      separators=(",", ":")).encode()


# --- bounded protobuf decoding ------------------------------------------------

class _Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.fields = 0

    def varint(self, pos, end):
        value = shift = 0
        for index in range(10):
            if pos >= end:
                raise ProtoError("truncated varint")
            byte = self.data[pos]
            pos += 1
            if index == 9 and byte > 1:
                raise ProtoError("varint over 64 bits")
            value |= (byte & 0x7F) << shift
            if not byte & 0x80:
                return value, pos
            shift += 7
        raise ProtoError("varint over ten bytes")


def _scalar(kind, wire, reader, pos, end):
    expected = SCALAR_WIRE[kind]
    if wire != expected:
        raise ProtoError(f"{kind} field with wire type {wire}")
    if kind == "double":
        if end - pos < 8:
            raise ProtoError("truncated double")
        return struct.unpack_from("<d", reader.data, pos)[0], pos + 8
    if kind == "string":
        length, pos = reader.varint(pos, end)
        if length > end - pos:
            raise ProtoError("string past its message")
        try:
            return reader.data[pos:pos + length].decode("utf-8"), pos + length
        except UnicodeDecodeError:
            raise ProtoError("string is not UTF-8") from None
    value, pos = reader.varint(pos, end)
    if kind == "bool":
        return value != 0, pos
    if kind == "uint32":
        return value & 0xFFFFFFFF, pos
    return (value - (1 << 64) if value >> 63 else value), pos  # int64


def _skip(wire, reader, pos, end):
    if wire == 0:
        return reader.varint(pos, end)[1]
    if wire == 1:
        size = 8
    elif wire == 5:
        size = 4
    elif wire == 2:
        size, pos = reader.varint(pos, end)
    else:
        raise ProtoError(f"wire type {wire} (groups are not used)")
    if size > end - pos:
        raise ProtoError("unknown field past its message")
    return pos + size


def _message(name, reader, pos, end, depth, unknown):
    if depth > MAX_PROTO_DEPTH:
        raise ProtoError("nesting too deep")
    fields = SCHEMA[name]
    result = {}
    while pos < end:
        reader.fields += 1
        if reader.fields > MAX_PROTO_FIELDS:
            raise ProtoError("too many fields")
        key, pos = reader.varint(pos, end)
        number, wire = key >> 3, key & 7
        if number == 0:
            raise ProtoError("field number 0")
        spec = fields.get(number)
        if spec is None:
            unknown[0] += 1
            pos = _skip(wire, reader, pos, end)
            continue
        field, kind, label = spec
        if kind in SCHEMA:
            if wire != 2:
                raise ProtoError(f"message field {field} with wire type {wire}")
            length, pos = reader.varint(pos, end)
            if length > end - pos:
                raise ProtoError(f"{field} past its message")
            value = _message(kind, reader, pos, pos + length, depth + 1, unknown)
            pos += length
            if label == "repeated":
                result.setdefault(field, []).append(value)
                continue
            if label == "oneof":
                for other, other_kind, other_label in fields.values():
                    if other_label == "oneof" and other != field:
                        result.pop(other, None)
            if isinstance(result.get(field), dict):
                _merge(result[field], value)  # a repeated singular message merges
            else:
                result[field] = value
            continue
        value, pos = _scalar(kind, wire, reader, pos, end)
        if label == "repeated":
            result.setdefault(field, []).append(value)
        else:
            result[field] = value  # last one wins
    if pos != end:
        raise ProtoError("field past its message")
    return result


def _merge(target, source):
    for field, value in source.items():
        if isinstance(value, list):
            target.setdefault(field, []).extend(value)
        elif isinstance(value, dict) and isinstance(target.get(field), dict):
            _merge(target[field], value)
        else:
            target[field] = value


def decode_message(name, data: bytes):
    """(fields present on the wire, unknown field count). Raises ProtoError.

    Absent fields are absent from the dict; get_field() supplies proto3
    defaults. A known field with the wrong wire type is refused rather than
    reinterpreted: the native encoder never writes one.
    """
    if len(data) > MAX_PROTO_BYTES:
        raise ProtoError(f"{len(data)} B over the {MAX_PROTO_BYTES} B decode bound")
    unknown = [0]
    return _message(name, _Reader(bytes(data)), 0, len(data), 0, unknown), unknown[0]


def get_field(message, name, field):
    """The decoded value or the proto3 default (an absent message is None)."""
    if field in message:
        return message[field]
    for spec_field, kind, label in SCHEMA[name].values():
        if spec_field == field:
            if label == "repeated":
                return []
            if kind in SCHEMA or label in ("optional", "oneof"):
                return None
            return SCALAR_DEFAULT[kind]
    raise KeyError(field)


# --- plan ---------------------------------------------------------------------

def batch_windows(now_s, history_days, offset_days, batch_days):
    """The daily windows the probe sends, newest first, as (route, from, to).

    The GET history window is [anchor - history_days, anchor). Each batch
    window sits one empty day further back, so no two windows touch: native
    coverage merges touching ranges, and a shared bucket could be served from
    the store. Every window therefore needs its own provider read unless an
    earlier run already filled it. The gap planner treats `to` as half-open;
    the store read also returns a bucket stamped exactly `to`, which is that
    empty day, so a reply may carry one point no window requested a fetch for.
    """
    anchor = now_s // DAY * DAY - offset_days * DAY
    windows = [("history", anchor - history_days * DAY, anchor)]
    end = anchor - history_days * DAY
    for route, _ in BATCH_ROUTES:
        end -= DAY
        windows.append((route, end - batch_days * DAY, end))
        end -= batch_days * DAY
    return windows


def span_days(history_days, batch_days) -> int:
    """Days covered by one run's windows and gaps: the offset step that gives
    a re-run entirely fresh windows on the same volume."""
    return history_days + len(BATCH_ROUTES) * (batch_days + 1) + 1


def batch_request(route, start, end):
    """(local path, headers without auth, body, provider control path)."""
    media = dict(BATCH_ROUTES)[route]
    if media == PROTOBUF_TYPE:
        body = encode_batch_request([BATCH_ID], start, end, BATCH_INTERVAL)
    else:
        body = encode_batch_json([BATCH_ID], start, end, BATCH_INTERVAL)
    provider = (f"/api/v3/coins/{BATCH_ID}/market_chart/range?vs_currency=usd"
                f"&from={start}&to={end}")
    return BATCH_PATH, {"Content-Type": media, "Accept": media}, body, provider


# --- classification -------------------------------------------------------------

def _points_problem(points, start, end, width, require_positive):
    previous = None
    for point in points:
        if not (isinstance(point, (list, tuple)) and len(point) == 2 and
                isinstance(point[0], int) and not isinstance(point[0], bool) and
                probe.is_number(point[1])):
            return "malformed point"
        stamp, value = point
        # The store read is inclusive at both ends (TypeScript range():
        # ts >= from && ts <= to), so a bucket at `to` is in the window.
        if not start <= stamp <= end or stamp % width:
            return f"point at {stamp} outside the window or off the {width} s grid"
        if previous is not None and stamp <= previous:
            return "points not strictly ascending"
        if require_positive and value <= 0:
            return "non-positive price"
        previous = stamp
    return None


def _json_entry(payload):
    """(envelope, entry, failure error) from a JSON batch reply."""
    series = payload.get("series")
    entry = series.get(BATCH_ID) if isinstance(series, dict) else None
    if not isinstance(entry, dict):
        return None
    envelope = (payload.get("from"), payload.get("to"), payload.get("interval"))
    if "error" in entry:
        return envelope, None, entry.get("error")
    cache = entry.get("cache")
    coverage = entry.get("coverage")
    history = {
        "id": entry.get("id"), "interval": entry.get("interval"),
        "points": entry.get("points"), "volumes": entry.get("volumes"),
        "hit": cache.get("hit") if isinstance(cache, dict) else None,
        "fetched": coverage.get("fetchedPoints") if isinstance(coverage, dict) else None,
        "cached": coverage.get("cachedPoints") if isinstance(coverage, dict) else None,
    }
    return envelope, history, None


def _proto_entry(message):
    """The same view of a decoded PriceHistoryBatchResponse; proto3 defaults
    stand in for absent implicit fields, but an absent submessage stays None."""
    response = "PriceHistoryBatchResponse"
    entry = next((item for item in get_field(message, response, "series")
                  if get_field(item, "PriceHistoryBatchEntry", "id") == BATCH_ID), None)
    if entry is None:
        return None
    envelope = tuple(get_field(message, response, name) for name in ("from", "to", "interval"))
    failure = entry.get("failure")
    if failure is not None:
        return envelope, None, get_field(failure, "PriceHistoryFailure", "error")
    history = entry.get("history")
    if history is None:
        return envelope, None, "entry carries neither history nor failure"

    def points(name):
        return [[get_field(p, "PricePoint", "unixSeconds"), get_field(p, "PricePoint", "value")]
                for p in get_field(history, "PriceHistory", name)]

    cache = history.get("cache")
    coverage = history.get("coverage")
    return envelope, {
        "id": get_field(history, "PriceHistory", "id"),
        "interval": get_field(history, "PriceHistory", "interval"),
        "points": points("points"), "volumes": points("volumes"),
        "hit": None if cache is None else get_field(cache, "CacheInfo", "hit"),
        "fetched": None if coverage is None else get_field(coverage, "PriceCoverage",
                                                           "fetchedPoints"),
        "cached": None if coverage is None else get_field(coverage, "PriceCoverage",
                                                          "cachedPoints"),
    }, None


def classify_batch(media, status, headers, body, start, end):
    """(category, detail, facts) for one native batch reply.

    Categories follow classify_native(): `compatible`, `volume_cached`,
    `native_no_data` (a control may attribute it), the local_* server
    refusals, `route_absent`, `route_error` and `ambiguous`. `facts` holds
    point counts for the report; the detail never quotes the body beyond the
    server's own short error code.
    """
    facts = {}
    if status in (413, 415, 503):
        payload = probe.load_json(body) if body else None
        code = (payload.get("code") or payload.get("error")) if isinstance(payload, dict) else None
        code = probe.bounded(code, 40) if isinstance(code, str) else "non-JSON"
        if status == 503:
            return ("route_error", f"503 {code}: native slot, size, handoff or shutdown "
                    "refusal, not provider evidence", facts)
        return "local_validation", f"{status} {code}: the native server refused the body", facts
    if status != 200:
        category, detail = probe.classify_native("history", status, headers, body)
        return category, detail, facts
    content_type = headers.get("content-type", "").split(";")[0].strip().lower()
    if content_type != media:
        return ("route_error", f"200 with content-type {probe.bounded(content_type, 40)}, "
                f"expected {media}", facts)
    if media == PROTOBUF_TYPE:
        try:
            message, unknown = decode_message("PriceHistoryBatchResponse", body)
        except ProtoError as error:
            return "route_error", f"200 body is not a PriceHistoryBatchResponse ({error})", facts
        facts["unknown_fields"] = unknown
        view = _proto_entry(message)
    else:
        payload = probe.load_json(body)
        if not isinstance(payload, dict):
            return "route_error", "200 body is not a JSON object", facts
        view = _json_entry(payload)
    if view is None:
        return "route_error", f"200 without a series entry for {BATCH_ID}", facts
    envelope, history, failure = view
    if envelope != (start, end, BATCH_INTERVAL):
        return "route_error", "200 whose from/to/interval do not echo the request", facts
    if history is None:
        error = probe.bounded(failure, 32) if isinstance(failure, str) else "?"
        if error == "UNRECOGNIZED_ID":
            return "local_validation", f"entry {error}: the native parser refused {BATCH_ID}", facts
        return ("route_error", f"entry failure {error}: native store, shutdown or encoding, "
                "not provider evidence", facts)
    if history["id"] != BATCH_ID or history["interval"] != BATCH_INTERVAL:
        return "route_error", "history entry id or interval does not match the request", facts
    points, volumes = history["points"], history["volumes"]
    if not isinstance(points, list) or not isinstance(volumes, list):
        return "route_error", "history entry without point lists", facts
    problem = (_points_problem(points, start, end, DAY, True)
               or _points_problem(volumes, start, end, DAY, False))
    if problem:
        return "route_error", f"history points failed validation: {problem}", facts
    hit, fetched, cached = history["hit"], history["fetched"], history["cached"]
    facts.update(points=len(points), fetched=fetched, cached=cached, hit=hit)
    counts = (f"points={len(points)} fetchedPoints={probe.bounded(fetched, 12)} "
              f"cachedPoints={probe.bounded(cached, 12)} cache.hit={hit}")
    if not isinstance(hit, bool) or not (isinstance(fetched, int) and isinstance(cached, int)):
        return "ambiguous", f"cache or coverage missing: {counts}", facts
    if points and hit is False and fetched > 0:
        return probe.COMPATIBLE, f"live provider read: {counts}", facts
    if points and hit is True:
        return ("volume_cached", f"served from the store without a provider read: {counts}",
                facts)
    if not points:
        why = ("backfill failed and was swallowed" if hit else
               "the provider read completed but left no point in the window")
        return "native_no_data", f"empty window ({why}): {counts}", facts
    return "ambiguous", counts, facts
