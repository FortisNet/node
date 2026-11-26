#!/usr/bin/env python3
"""Report C++ allocation boundaries, ignoring comments and embedded page strings.

This is a source inventory, not a proof of correct lifetime management. Vendor,
generated, C-only and test fixture code are outside the production C++ scope.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
NON_CODE = re.compile(
    r'//[^\n]*|/\*.*?\*/|R"(?P<tag>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=tag)"'
    r'|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.DOTALL)
CALL = re.compile(r'\b(?:malloc|calloc|realloc|free|strdup|strndup|alloca|aligned_alloc|posix_memalign)\s*\(')
KEYWORD = re.compile(r'\b(?:new|delete)\b')


def code_only(text):
    code = NON_CODE.sub(lambda match: re.sub(r'[^\n]', ' ', match.group()), text)
    return re.sub(r'(?m)^[ \t]*#[ \t]*include[^\n]*', lambda match: ' ' * len(match.group()), code)


def main():
    violations = []
    boundaries = []
    for path in sorted((ROOT / 'src').rglob('*')):
        if path.suffix not in {'.cpp', '.h', '.hpp', '.cc'}:
            continue
        code = code_only(path.read_text())
        relative = path.relative_to(ROOT).as_posix()
        for call in CALL.finditer(code):
            violations.append(f'{relative}:{code.count(chr(10), 0, call.start()) + 1}: {call.group().strip()}')
        for keyword in KEYWORD.finditer(code):
            if keyword.group() == 'delete' and re.search(r'=\s*$', code[:keyword.start()]):
                continue  # a deleted special member, not deallocation
            boundaries.append(f'{relative}:{code.count(chr(10), 0, keyword.start()) + 1}: {keyword.group()}')
    print(f'Forbidden C allocation calls in production C++: {len(violations)}')
    for violation in violations:
        print(violation)
    print('Explicit new/delete expressions (including placement construction):')
    for boundary in boundaries:
        print(boundary)
    return bool(violations)


if __name__ == '__main__':
    raise SystemExit(main())
