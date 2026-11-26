# Reference tools

The node promises to behave exactly like JavaScript in a few places: case
conversion, JSON and protobuf number formatting, Unicode normalization for
redaction, and URL parsing. The tests replay answers that a JavaScript
engine gave once; those answers are data files in this repository.

The tools here are what recorded them. They are the only files in the
repository that need Node.js, and nothing in the build or the gate runs
them. Use them when the reference itself must move, for example to a newer
Unicode version, and commit the regenerated data.

| Tool | Writes | When |
| --- | --- | --- |
| `generate_js_case_tables.mjs --write` | `src/compat/js_case_tables.cpp` | A newer Unicode version |
| `record_js_case.mjs` | `tests/corpus/js_case_expectations.json` | After regenerating the case tables |
| `record_js_json.mjs` | `tests/corpus/js_json_expectations.json` | To extend the JSON and protobuf cases. Needs the npm package `@bufbuild/protobuf` 2.x |
| `record_redaction_unicode.mjs` | `contracts/redaction-unicode-data.json` | A newer Unicode version; then `python3 scripts/generate_redaction_tables.py --write` |
| `generate_whatwg_url_corpus.mjs` | `tests/corpus/whatwg_url_corpus.json` | To extend the URL cases |

Two checks need a JavaScript engine by nature and are run by hand:

| Check | What it does |
| --- | --- |
| `incorrect_coin_ids_page_test.mjs` | Runs the script of the node's error page with stand-in page elements and checks how it sorts records. The gate runs a weaker, static check of the same page (`tests/incorrect_coin_ids_page_test.py`) |
| `dashboard_browser_test.mjs` | Drives a real Chromium against the dashboard for layout and accessibility. Needs `build/fortis_node_operations_test` and a Chrome or Chromium binary |

Each tool was last run with Node.js 25.9.0 (Unicode 17.0). A data file
records which engine produced it.
