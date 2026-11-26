#!/usr/bin/env python3
"""Build the API reference that the node serves at /doc and /openapi.yaml.

Inputs
  contracts/openapi.base.yaml  the reviewed API contract (OpenAPI 3)
  openapi.native.yaml          an overlay: paths and schemas this C++ server
                               adds, and the ones it does not implement

Output
  src/operations/api_docs_generated.cpp, which holds two C++ string
  constants compiled into the server: a self-contained, read-only HTML
  reference, and the merged contract as YAML.

Before writing anything the merged contract is checked against the code:
every /v1/ route that a source file under src/ binds on the router must be
described in it (see route_bindings.py).

Usage
  python3 scripts/generate_api_docs.py --write   regenerate the file
  python3 scripts/generate_api_docs.py --check   fail if the file is stale

Only the Python standard library is needed. The page embeds schemas as JSON
and the served YAML is public, so both are kept byte-stable: api_docs_yaml.py
reads and writes the YAML, and the helpers below format values the way the
page has always shown them (JavaScript's JSON and string conventions).
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import api_docs_yaml  # noqa: E402
from route_bindings import route_bindings  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE_CONTRACT = 'contracts/openapi.base.yaml'
NATIVE_OVERLAY = 'openapi.native.yaml'
OUTPUT = 'src/operations/api_docs_generated.cpp'

HTTP_METHODS = ('get', 'post', 'put', 'patch', 'delete', 'head')
HTML_DELIMITER = 'FORTIS_HTML'
YAML_DELIMITER = 'FORTIS_YAML'


class GenerationError(Exception):
    """The contract, the overlay or the source tree cannot be documented."""


# --------------------------------------------------------------------------
# Value formatting
# --------------------------------------------------------------------------

def text_of(value):
    """The text of a scalar taken from the contract; absent values are empty."""
    if value is None:
        return ''
    if isinstance(value, str):
        return value
    if isinstance(value, bool):
        return 'true' if value else 'false'
    if isinstance(value, (int, float)):
        return api_docs_yaml.js_number_text(value)
    raise GenerationError(f'expected text in the contract, found {type(value).__name__}: {value!r}')


_HTML_ESCAPES = {'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}


def escape(value):
    return re.sub(r'[&<>"\']', lambda m: _HTML_ESCAPES[m.group()], text_of(value))


def json_text(value, indent=''):
    """Format a value as JSON with two-space indentation, exactly as
    JavaScript's JSON.stringify(value, null, 2): non-ASCII text is kept as
    is, and a whole-number float is written without a fraction."""
    if value is None:
        return 'null'
    if isinstance(value, bool):
        return 'true' if value else 'false'
    if isinstance(value, str):
        return api_docs_yaml.json_quote(value)
    if isinstance(value, (int, float)):
        return api_docs_yaml.js_number_text(value)
    inner = indent + '  '
    if isinstance(value, list):
        if not value:
            return '[]'
        items = [inner + json_text(item, inner) for item in value]
        return '[\n' + ',\n'.join(items) + '\n' + indent + ']'
    if isinstance(value, dict):
        if not value:
            return '{}'
        items = [f'{inner}{api_docs_yaml.json_quote(key)}: {json_text(item, inner)}'
                 for key, item in value.items()]
        return '{\n' + ',\n'.join(items) + '\n' + indent + '}'
    raise GenerationError(f'cannot format {type(value).__name__} as JSON')


def pretty(value):
    return escape(json_text(value))


def is_present(value):
    """True unless the value is absent, false, zero or an empty string. An
    empty mapping or list counts as present."""
    return isinstance(value, (dict, list)) or bool(value)


def mapping(value, what):
    if not isinstance(value, dict):
        raise GenerationError(f'{what} must be a mapping')
    return value


def optional_list(value, what):
    if value is not None and not isinstance(value, list):
        raise GenerationError(f'{what} must be a list')
    return value or []


def route_key(method, route):
    """One spelling for a route in the contract ("/v1/x/{id}") and in the
    C++ router ("/v1/x/:id")."""
    return f'{method.upper()} ' + re.sub(r':([A-Za-z0-9_]+)', r'{\1}', route)


def operations_of(document):
    """Yield (route, method, operation) for every operation in the contract.
    Other keys of a path item (shared parameters, summaries) are skipped."""
    for route, path_item in mapping(document['paths'], 'paths').items():
        for method, operation in mapping(path_item, f'paths.{route}').items():
            if method in HTTP_METHODS:
                yield route, method, mapping(operation, f'paths.{route}.{method}')


# --------------------------------------------------------------------------
# Contract
# --------------------------------------------------------------------------

def read_text(relative_path):
    with open(os.path.join(ROOT, relative_path), encoding='utf-8', newline='') as handle:
        return handle.read()


def child_mapping(root, *keys):
    node = root
    for key in keys:
        entry = node.find(key) if isinstance(node, api_docs_yaml.Mapping) else None
        if entry is None or not isinstance(entry.value, api_docs_yaml.Mapping):
            raise GenerationError(f'{BASE_CONTRACT}: expected a mapping at {".".join(keys)}')
        node = entry.value
    return node


def remove_entries(target, names, what):
    for name in optional_list(names, f'{NATIVE_OVERLAY}: {what}'):
        if not isinstance(name, str):
            raise GenerationError(f'{NATIVE_OVERLAY}: {what} must list names')
        # A name the contract no longer has is not an error.
        target.entries = [entry for entry in target.entries if entry.key.value != name]


def put_entries(target, additions, what):
    for name, value in mapping(additions if additions is not None else {},
                               f'{NATIVE_OVERLAY}: {what}').items():
        if not isinstance(value, dict):
            raise GenerationError(f'{NATIVE_OVERLAY}: {what}.{name} must be a mapping')
        node = api_docs_yaml.from_js(value)
        existing = target.find(name)
        if existing is not None:
            existing.value = node  # replaced in place; the key keeps its position
        else:
            target.entries.append(api_docs_yaml.Entry(api_docs_yaml.Scalar(name), node))


def merged_contract():
    """Apply the native overlay to the base contract and return the result
    both as data and as YAML text.

    The base keeps its layout (it is parsed into nodes that remember how each
    value was written). Overlay operations are added as plain data, so the
    native-only parts stay out of the shared base contract while /openapi.yaml
    and /doc describe the surface this server really has.
    """
    merged = api_docs_yaml.parse_document(read_text(BASE_CONTRACT), BASE_CONTRACT)
    overlay = api_docs_yaml.to_js(api_docs_yaml.parse_document(
        read_text(NATIVE_OVERLAY), NATIVE_OVERLAY, allow_comments=True))

    if overlay.get('excludedPaths'):
        remove_entries(child_mapping(merged, 'paths'), overlay['excludedPaths'], 'excludedPaths')
    if overlay.get('excludedSecuritySchemes'):
        remove_entries(child_mapping(merged, 'components', 'securitySchemes'),
                       overlay['excludedSecuritySchemes'], 'excludedSecuritySchemes')
    if overlay.get('excludedSchemas'):
        remove_entries(child_mapping(merged, 'components', 'schemas'),
                       overlay['excludedSchemas'], 'excludedSchemas')
    if overlay.get('paths'):
        put_entries(child_mapping(merged, 'paths'), overlay['paths'], 'paths')
    overlay_components = overlay.get('components')
    overlay_schemas = None if overlay_components is None else mapping(
        overlay_components, f'{NATIVE_OVERLAY}: components').get('schemas')
    if overlay_schemas:
        put_entries(child_mapping(merged, 'components', 'schemas'), overlay_schemas,
                    'components.schemas')

    document = api_docs_yaml.to_js(merged)
    version = document.get('openapi')
    if not isinstance(version, str) or not version.startswith('3.') or not is_present(document.get('paths')):
        raise GenerationError('Expected an OpenAPI 3 document')
    mapping(document.get('info'), 'info')
    return document, api_docs_yaml.emit_document(merged)


def validate_native_coverage(document):
    """Every /v1/ route bound in src/ must be in the contract. Routes under
    /v1/test-operations/ exist only in test builds and are exempt."""
    documented = {route_key(method, route) for route, method, _ in operations_of(document)}
    missing = []
    for directory, subdirectories, files in os.walk(os.path.join(ROOT, 'src')):
        subdirectories.sort()
        for name in sorted(files):
            if not name.endswith('.cpp'):
                continue
            path = os.path.join(directory, name)
            with open(path, encoding='utf-8', errors='replace') as handle:
                source = handle.read()
            for method, route in route_bindings(source):
                if (route.startswith('/v1/') and not route.startswith('/v1/test-operations/')
                        and route_key(method, route) not in documented):
                    missing.append(f'{method} {route} ({os.path.relpath(path, ROOT)})')
    if missing:
        raise GenerationError('Native routes missing from OpenAPI:\n' + '\n'.join(missing))


# --------------------------------------------------------------------------
# HTML reference
# --------------------------------------------------------------------------

STYLE = (
    ':root{color-scheme:dark;font:14px/1.6 system-ui,sans-serif;background:#101116;color:#e8e8ee}'
    '*{box-sizing:border-box}body{margin:0;display:grid;grid-template-columns:230px minmax(0,1fr)}'
    'aside{position:sticky;top:0;height:100vh;overflow:auto;padding:1.5rem;background:#171920;'
    'border-right:1px solid #30343d}aside a{display:block;color:#b7c2d9;text-decoration:none;'
    'padding:.3rem 0}aside a:hover{color:#fff}main{max-width:1100px;width:100%;padding:2rem 3rem}'
    'h1{font-size:2rem}h2{margin-top:2.5rem;border-bottom:1px solid #343943;padding-bottom:.3rem}'
    'h2 small{color:#8793a7;font-size:.75rem}h3{display:flex;align-items:center;gap:1rem;'
    'flex-wrap:wrap}article,.schema{background:#1a1d25;border:1px solid #343943;'
    'border-radius:10px;padding:1rem 1.4rem;margin:1rem 0}article p{white-space:normal;'
    'color:#bdc4cf}code,pre{font-family:ui-monospace,SFMono-Regular,monospace}'
    'code{overflow-wrap:anywhere}pre{white-space:pre-wrap;overflow-wrap:anywhere;'
    'background:#11131a;border-radius:6px;padding:1rem;max-height:28rem;overflow:auto}'
    'details{margin:.75rem 0}summary{cursor:pointer;color:#c5aaff;font-weight:600}'
    '.method{border-radius:4px;padding:.2rem .5rem;color:#081011;background:#69d8a0;'
    'font-size:.7rem}.post{background:#e5b86b}.delete{background:#fa9292}'
    '.put,.patch{background:#80baf1}.meta{font-size:.8rem;color:#9da6b5!important}'
    '.summary{font-weight:700;color:#f0f1f6!important}input{width:100%;padding:.7rem;'
    'background:#20242d;color:#fff;border:1px solid #444c58;border-radius:6px}a{color:#bc9fff}'
    '@media(max-width:700px){body{display:block}aside{position:relative;height:auto}'
    'main{padding:1rem}} ')

# Filters the operations and schemas by the text typed into the search box.
SEARCH_SCRIPT = (
    "document.getElementById('search').addEventListener('input',function(){"
    "var q=this.value.trim().toLowerCase();"
    "document.querySelectorAll('[data-search]').forEach(function(el){"
    "el.hidden=!!q&&!el.dataset.search.includes(q)})})")


def tag_anchor(tag):
    return 'tag-' + escape(re.sub(r'[^A-Za-z0-9_]+', '-', tag))


def security_text(security):
    """Name the credentials an operation accepts: alternatives are separated
    by "; ", schemes that must be combined by ", "."""
    alternatives = optional_list(security, 'security')
    if not alternatives:
        return 'public'
    return '; '.join(', '.join(mapping(alternative, 'security requirement'))
                     for alternative in alternatives)


def render_operation(document, tag, route, method, operation):
    operation_id = escape(operation['operationId'] if operation.get('operationId') is not None
                          else f'{method}-{route}')
    summary = text_of(operation.get('summary'))
    details = []
    if optional_list(operation.get('parameters'), f'{method} {route}: parameters'):
        details.append('<details><summary>Parameters</summary>'
                       f'<pre>{pretty(operation["parameters"])}</pre></details>')
    if is_present(operation.get('requestBody')):
        details.append('<details><summary>Request body</summary>'
                       f'<pre>{pretty(operation["requestBody"])}</pre></details>')
    if is_present(operation.get('responses')):
        details.append('<details><summary>Responses</summary>'
                       f'<pre>{pretty(operation["responses"])}</pre></details>')
    security = operation['security'] if operation.get('security') is not None \
        else document.get('security')
    search = escape(f'{tag} {method} {route} {summary}'.lower())
    description = escape(operation.get('description')).replace('\n', '<br>')
    return (
        f'<article id="{operation_id}" data-search="{search}">\n'
        f'<h3><span class="method {method}">{escape(method.upper())}</span>'
        f'<code>{escape(route)}</code></h3>\n'
        f'<p class="summary">{escape(summary)}</p><p>{description}</p>\n'
        f'<p class="meta">Operation: <code>{operation_id}</code> · Security: '
        f'{escape(security_text(security))}</p>{"".join(details)}</article>')


def render(document):
    """Lay the contract out as one HTML page: operations grouped by their
    first tag in the order tags first appear, then every schema."""
    by_tag = {}
    for route, method, operation in operations_of(document):
        tags = optional_list(operation.get('tags'), f'{method} {route}: tags')
        tag = tags[0] if tags and tags[0] is not None else 'Other'
        if not isinstance(tag, str):
            raise GenerationError(f'{method} {route}: tag must be text')
        by_tag.setdefault(tag, []).append((route, method, operation))

    groups = '\n'.join(
        f'<section id="{tag_anchor(tag)}"><h2>{escape(tag)} <small>{len(operations)}</small></h2>'
        + '\n'.join(render_operation(document, tag, *operation) for operation in operations)
        + '</section>'
        for tag, operations in by_tag.items())

    components = document.get('components')
    schemas = (mapping(components, 'components').get('schemas') if components is not None
               else None)
    schemas = mapping(schemas if schemas is not None else {}, 'components.schemas')
    schema_cards = '\n'.join(
        f'<details class="schema" data-search="{escape(name.lower())}">'
        f'<summary>{escape(name)}</summary><pre>{pretty(schema)}</pre></details>'
        for name, schema in schemas.items())

    navigation = ''.join(f'<a href="#{tag_anchor(tag)}">{escape(tag)}</a>' for tag in by_tag)
    operation_count = sum(len(operations) for operations in by_tag.values())
    info = document['info']
    title = escape(info.get('title'))
    description = escape(info.get('description')).replace('\n', '<br>')
    return (
        '<!doctype html><html lang="en"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width, initial-scale=1">'
        f'<title>{title} — API Reference</title>\n'
        f'<style>{STYLE}</style></head><body>\n'
        f'<aside><strong>FortisNet API</strong><p>{operation_count} operations · '
        f'{len(schemas)} schemas</p><input id="search" type="search" '
        'placeholder="Filter operations" aria-label="Filter operations">'
        '<a href="/">← Home</a><a href="/openapi.yaml" download>Download OpenAPI YAML</a><hr>'
        f'{navigation}<a href="#schemas">Schemas</a></aside>\n'
        f'<main><h1>{title}</h1><p>Version {escape(info.get("version"))} · OpenAPI '
        f'{escape(document["openapi"])} · Read-only reference</p><p>{description}</p>{groups}'
        f'<section id="schemas"><h2>Schemas</h2>{schema_cards}</section></main>\n'
        f'<script>{SEARCH_SCRIPT}</script></body></html>')


# --------------------------------------------------------------------------
# C++ output
# --------------------------------------------------------------------------

def raw_string_literal(value, delimiter):
    """Wrap text in a C++ raw string literal, R"DELIM(...)DELIM", which needs
    no escaping as long as the text never contains the closing sequence."""
    closing = f'){delimiter}"'
    if closing in value:
        raise GenerationError(f'the generated text contains {closing!r} and cannot be embedded')
    return f'R"{delimiter}({value}{closing}'


def generate():
    """Return the full text of the generated C++ file and the number of
    operations it documents."""
    document, yaml_text = merged_contract()
    validate_native_coverage(document)
    html = render(document)
    # The contract is about 200 KiB, so these literals intentionally exceed
    # the minimum literal length the C++ standard guarantees. The build
    # disables that warning for this generated translation unit only.
    generated = (
        f'// Generated by scripts/generate_api_docs.py from {BASE_CONTRACT}\n'
        f'// plus {NATIVE_OVERLAY}. Do not edit; run make generate-api-docs.\n'
        '#include "api_docs_generated.h"\n'
        'namespace fortis::operations {\n'
        '\n'
        f'const char kApiDocsHtml[] = {raw_string_literal(html, HTML_DELIMITER)};\n'
        'const size_t kApiDocsHtmlLength = sizeof(kApiDocsHtml) - 1;\n'
        f'const char kApiDocsYaml[] = {raw_string_literal(yaml_text, YAML_DELIMITER)};\n'
        'const size_t kApiDocsYamlLength = sizeof(kApiDocsYaml) - 1;\n'
        '\n'
        '}  // namespace fortis::operations\n')
    return generated, sum(1 for _ in operations_of(document))


def main(arguments):
    if len(arguments) != 1 or arguments[0] not in ('--write', '--check'):
        print('usage: python3 scripts/generate_api_docs.py --write|--check', file=sys.stderr)
        return 2
    try:
        generated, operation_count = generate()
    except (GenerationError, api_docs_yaml.YamlSubsetError) as error:
        print(f'generate_api_docs: {error}', file=sys.stderr)
        return 1
    output_path = os.path.join(ROOT, OUTPUT)
    if arguments[0] == '--write':
        with open(output_path, 'w', encoding='utf-8', newline='') as handle:
            handle.write(generated)
        print(f'generated {OUTPUT} ({operation_count} operations)')
        return 0
    try:
        current = read_text(OUTPUT)
    except (OSError, UnicodeDecodeError):
        current = ''
    if current != generated:
        print('API docs are stale; run make generate-api-docs', file=sys.stderr)
        return 1
    print('API docs are current')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
