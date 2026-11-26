#!/usr/bin/env python3
"""Checks scripts/route_bindings.py against the ways routes are bound."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'scripts'))

from route_bindings import route_bindings  # noqa: E402

assert route_bindings('router.Add("GET", "/v1/example", handler);') == [('GET', '/v1/example')]
assert route_bindings('router.Add<&Service::Post>(\n "POST", "/v1/example", service);') == [
    ('POST', '/v1/example')]
assert route_bindings(
    'constexpr char kRoute[] = "/v1/prices/missing-identifiers";\n'
    'router.Add<&Service::Post>("POST", kRoute, service);') == [
        ('POST', '/v1/prices/missing-identifiers')]
assert route_bindings('router.Add("GET", dynamic_path, handler);') == []
print('route binding extraction: literal, typed, constant and dynamic cases passed')
