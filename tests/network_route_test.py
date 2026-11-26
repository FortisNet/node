#!/usr/bin/env python3
"""Native GET /v1/networks contract against its frozen TypeScript oracle."""
import json, os, socket, subprocess, sys, tempfile, time
from pathlib import Path

FIXTURE = Path(__file__).resolve().parents[1] / "tests/corpus/networks_route.json"
TOKEN = "fintel_dev_" + "N" * 43
ID = "01ARZ3NDEKTSV4RRFFQ69G5FAV"

def read_varint(data, offset):
    value = 0
    shift = 0
    while True:
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7f) << shift
        if byte < 0x80:
            return value, offset
        shift += 7
        if shift > 63:
            raise ValueError("oversized protobuf varint")

def fields(data):
    offset = 0
    while offset < len(data):
        key, offset = read_varint(data, offset)
        number, wire = key >> 3, key & 7
        if wire == 0:
            value, offset = read_varint(data, offset)
        elif wire == 2:
            length, offset = read_varint(data, offset)
            value = data[offset:offset + length]
            offset += length
        else:
            raise ValueError(f"unsupported protobuf wire type {wire}")
        yield number, value

def decode_map_entry(data):
    result = {}
    for number, value in fields(data):
        if number in (1, 2):
            result["key" if number == 1 else "value"] = value.decode()
    return result

def decode_native(data):
    result = {}
    for number, value in fields(data):
        if number == 1: result["symbol"] = value.decode()
        elif number == 2: result["decimals"] = value
        elif number == 3: result["coingeckoId"] = value.decode()
    return result

def decode_network(data):
    result = {"testnet": False, "rpcUrls": [], "capabilities": {}}
    for number, value in fields(data):
        if number == 1: result["id"] = value.decode()
        elif number == 2: result["family"] = value.decode()
        elif number == 3: result["evmChainId"] = value
        elif number == 4: result["testnet"] = bool(value)
        elif number == 5: result["requiredConfirmations"] = value
        elif number == 6: result["rpcUrls"].append(value.decode())
        elif number == 7: result.setdefault("graphqlUrls", []).append(value.decode())
        elif number == 8: result["native"] = decode_native(value)
        elif number == 9:
            entry = decode_map_entry(value)
            result["capabilities"][entry["key"]] = entry["value"]
    return result

def decode_networks(data):
    result = {"networks": []}
    prices = {"spot": False, "history": False, "sources": {}}
    for number, value in fields(data):
        if number == 1: result["networks"].append(decode_network(value))
        elif number == 2:
            for field, item in fields(value):
                if field == 1: prices["spot"] = bool(item)
                elif field == 2: prices["history"] = bool(item)
                elif field == 3:
                    entry = decode_map_entry(item)
                    prices["sources"][entry["key"]] = entry["value"]
    if prices:
        result["prices"] = prices
    return result

def native_expectation(fixture_body):
    expected = json.loads(json.dumps(fixture_body))
    for network in expected["networks"]:
        network["capabilities"] = {name: "none" for name in network["capabilities"]}
    expected["prices"]["spot"] = False
    expected["prices"]["history"] = False
    return expected

def assert_live_catalog(actual, fixture_body):
    """Check stable catalog fields while allowing composed live route planes.

    The normal server now advertises the routes it actually assembled at
    startup. Their capability values depend on configuration and therefore
    cannot use the old all-disabled test-binary expectation.
    """
    expected = native_expectation(fixture_body)
    assert set(actual) == {"networks", "prices"}
    assert len(actual["networks"]) == len(expected["networks"])
    for got, want in zip(actual["networks"], expected["networks"]):
        assert set(got["capabilities"]) == set(want["capabilities"])
        assert set(got["capabilities"].values()) <= {"none", "ok"}
        got_static = {key: value for key, value in got.items() if key != "capabilities"}
        want_static = {key: value for key, value in want.items() if key != "capabilities"}
        assert got_static == want_static
    assert actual["prices"]["sources"] == expected["prices"]["sources"]
    assert isinstance(actual["prices"]["spot"], bool)
    assert isinstance(actual["prices"]["history"], bool)

def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0)); return s.getsockname()[1]

def request(port, method="GET", accept=None, auth=True, origin=None):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
        h=[f"{method} /v1/networks HTTP/1.1", "Host: localhost", "Connection: close"]
        if accept: h.append("Accept: " + accept)
        if auth: h.append("Authorization: Bearer " + TOKEN)
        if origin: h.append("Origin: " + origin)
        s.sendall(("\r\n".join(h)+"\r\n\r\n").encode())
        data=bytearray()
        while True:
            b=s.recv(65536)
            if not b: break
            data.extend(b)
    head,_,body=bytes(data).partition(b"\r\n\r\n")
    lines=head.decode("latin1").split("\r\n")
    headers={k.lower():v.strip() for line in lines[1:] if ":" in line for k,v in [line.split(":",1)]}
    if not lines or len(lines[0].split()) < 2: raise RuntimeError(f"empty response: {data!r}")
    return int(lines[0].split()[1]),headers,body

def start(binary,root,env_extra=None,enabled=True):
    kv=root/"kv";kv.mkdir(parents=True,exist_ok=True);port=free_port();env=os.environ.copy();env["FORTIS_KV_DIR"]=str(kv)
    env.pop("FORTIS_ENABLE_TEST_NETWORK_ROUTE",None)
    if enabled:env["FORTIS_ENABLE_TEST_NETWORK_ROUTE"]="1"
    if env_extra:env.update(env_extra)
    p=subprocess.Popen([binary,str(port)],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    for _ in range(100):
        if p.poll() is not None: raise RuntimeError(p.stderr.read().decode())
        try:
            with socket.create_connection(("127.0.0.1",port),timeout=.1): return p,port
        except OSError:time.sleep(.03)
    raise RuntimeError("server failed to start")

def main(binary):
    fixture=json.loads(FIXTURE.read_text()); expected=fixture["json"]
    native_expected = native_expectation(expected["body"])
    with tempfile.TemporaryDirectory(prefix="networks-route-") as td:
        root=Path(td)
        p,port=start(binary,root/"off",enabled=False)
        try: assert request(port,auth=False)[0]==401
        finally:p.terminate();p.wait(timeout=5)
        # Seed a real durable token through the fixture build, then reopen the
        # same KV with the normal production binary. This proves the route is
        # registered outside the test macro and reads ordinary device auth.
        promotion_root = root/"promotion"
        p,port=start(binary,promotion_root)
        try: assert request(port)[0]==200
        finally:p.terminate();p.wait(timeout=5)
        normal_binary = str(Path(binary).with_name("fortis_node"))
        p,port=start(normal_binary,promotion_root,enabled=False)
        try:
            status,_,body=request(port)
            assert status==200
            assert_live_catalog(json.loads(body), expected["body"])
            status,headers,body=request(port,accept="application/protobuf")
            assert status==200 and headers["content-type"]=="application/protobuf"
            assert_live_catalog(decode_networks(body), expected["body"])
        finally:p.terminate();p.wait(timeout=5)
        p,port=start(binary,root/"on")
        try:
            status,he,be=request(port,auth=False)
            assert status==401 and json.loads(be)=={"error":"UNAUTHORIZED"}
            # TypeScript refuses in the preHandler, before the handler adds Vary.
            assert he["content-type"]=="application/json; charset=utf-8" and "vary" not in he
            status,he,be=request(port,origin="https://example.com")
            assert status==403 and json.loads(be)=={"error":"BROWSER_ORIGIN_FORBIDDEN"}
            assert "vary" not in he and he["cache-control"]=="no-store"
            status,h,b=request(port)
            assert status==200 and h["content-type"]==expected["headers"]["content-type"]
            assert h["content-length"]==str(len(b))
            assert h["vary"]=="Accept" and h["cache-control"]=="no-store"
            assert h["x-rate-limit-cost"]=="1" and h["x-rate-limit-remaining"]==expected["headers"]["x-rate-limit-remaining"]
            for key in ("x-content-type-options", "referrer-policy", "permissions-policy", "cache-control"):
                assert h[key] == expected["headers"][key]
            assert json.loads(b)==native_expected
            json_body_length = len(b)
            assert all(set(network["capabilities"].values()) == {"none"}
                       for network in json.loads(b)["networks"])
            ph=fixture["protobuf"]; status,h,b=request(port,accept="application/protobuf")
            assert status==200 and h["content-type"]=="application/protobuf"
            assert decode_networks(b)==native_expected
            assert h["content-length"]==str(len(b))
            assert h["x-rate-limit-remaining"]==ph["headers"]["x-rate-limit-remaining"]
            for key in ("x-content-type-options", "referrer-policy", "permissions-policy", "cache-control"):
                assert h[key] == ph["headers"][key]
            assert h["content-length"]==str(len(b)) and h["vary"]=="Accept"
            status,hh,body=request(port,method="HEAD")
            assert status==200 and not body and hh["content-length"]==str(json_body_length)
            status,hh,body=request(port,method="HEAD",accept="application/protobuf")
            assert status==200 and not body and hh["content-length"]==str(len(b))
            for _ in range(236):
                status,_,_=request(port,method="HEAD")
                assert status==200
            status,hq,bq=request(port)
            assert status==429 and json.loads(bq)=={"error":"RATE_LIMITED"}
            assert hq["retry-after"]=="60" and hq["x-rate-limit-cost"]=="1"
            assert hq["x-rate-limit-remaining"]=="0"
        finally:p.terminate();p.wait(timeout=5)
        p,port=start(binary,root/"keyed",env_extra={
            "COINGECKO_API_KEY":"fixture-key", "JUPITER_API_KEY":"fixture-key",
            "ALCHEMY_API_KEY":"private-alchemy-key",
            "DASH_BLOCKBOOK_URLS":"https://private-dash.example/api/v2?token=secret"})
        try:
            status,_,body=request(port)
            assert status==200
            result=json.loads(body)
            assert result["prices"]=={
                "spot":False, "history":False, "sources":{
                    "coingecko":"demo", "jupiter":"keyed", "vestige":"keyless"}}
            assert next(row for row in result["networks"] if row["id"]=="robinhood")["rpcUrls"]==[
                "https://rpc.mainnet.chain.robinhood.com"]
            assert next(row for row in result["networks"] if row["id"]=="dash")["rpcUrls"]==[
                "https://dash.atomicwallet.io/api/v2"]
            assert b"private-alchemy-key" not in body and b"private-dash.example" not in body
        finally:p.terminate();p.wait(timeout=5)
    print("native networks route contract passed")

if __name__=="__main__":main(sys.argv[1])
