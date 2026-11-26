"""Find the HTTP routes a C++ source file binds with a fixed name.

Used by generate_api_docs.py to check that every route the server registers
is described in the API contract. Only bindings whose path is a string
literal, or a constexpr string constant defined in the same file, are found;
routes built at run time are covered by their own route and contract tests.
"""

import re

# constexpr char kRoute[] = "/v1/example";   (also "const char*" forms)
_STRING_CONSTANT = re.compile(
    r'constexpr\s+(?:const\s+)?char\s+([A-Za-z0-9_]+)\s*(?:\[\s*\]|\*)\s*=\s*"([^"\n]+)"')

# router.Add("GET", "/v1/example", ...), router.Add<&Service::Get>("GET", kRoute, ...)
_ROUTER_ADD = re.compile(
    r'router\.Add(?:<[^;{}]*?>)?\(\s*"(GET|POST|PUT|PATCH|DELETE)"\s*,\s*'
    r'(?:"([^"\n]+)"|([A-Za-z0-9_]+))')


def route_bindings(source):
    """Return [(method, path), ...] for the router.Add calls in source, in
    the order they appear. Calls whose path is neither a literal nor a known
    string constant are left out."""
    constants = dict(_STRING_CONSTANT.findall(source))
    bindings = []
    for method, literal, constant in _ROUTER_ADD.findall(source):
        route = literal or constants.get(constant)
        if route is not None:
            bindings.append((method, route))
    return bindings
