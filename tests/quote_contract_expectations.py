"""Retarget captured provider URLs to the native quote-observation contract.

Captured fixture files and their integrity hashes remain unchanged. Only the
required token-price observation query field is added; payloads and assertions
are preserved. Native simple/price behavior depends on snapshot configuration
and is intentionally outside this conversion.
"""
from urllib.parse import parse_qs, urlsplit, urlunsplit


def observation_url(url):
    parts = urlsplit(url)
    if not parts.path.startswith(('/api/v3/simple/token_price/', '/simple/token_price/')) or \
            'include_last_updated_at' in parse_qs(parts.query, keep_blank_values=True):
        return url
    query = parts.query + ('&' if parts.query else '') + 'include_last_updated_at=true'
    return urlunsplit(parts._replace(query=query))


def retarget(value):
    if isinstance(value, dict):
        for key, item in value.items():
            if key in ('body', 'json', 'response', 'responseBody'):
                continue
            if key in ('url', 'path') and isinstance(item, str):
                value[key] = observation_url(item)
            elif isinstance(item, (dict, list)):
                retarget(item)
    elif isinstance(value, list):
        for item in value:
            retarget(item)
    return value
