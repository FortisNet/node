#!/usr/bin/env python3
"""Negative controls for enforced scoped formatting, naming and doc generation."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('contracts', ROOT / 'scripts/cpp_contracts.py')
contracts = importlib.util.module_from_spec(spec)
spec.loader.exec_module(contracts)


class ContractToolsTest(unittest.TestCase):
    def test_format_and_naming_reject_drift(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'bad.h'
            source.write_text('class bad_type{public:void bad_method(){}};\n')
            formatting = subprocess.run(['clang-format-18', '--style=file:' + str(ROOT / '.clang-format'), '--dry-run', '--Werror', str(source)], capture_output=True)
            self.assertNotEqual(formatting.returncode, 0)
            naming = subprocess.run(['clang-tidy-18', str(source), '-config=' + json.dumps(contracts.naming_config()), '--', '-x', 'c++-header', '-std=c++23'], capture_output=True)
            self.assertNotEqual(naming.returncode, 0)
            self.assertIn(b'readability-identifier-naming', naming.stdout + naming.stderr)

    def test_documentation_rejects_unresolved_contract_reference(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'bad.h').write_text('/** See @ref no_such_contract_type. */\nclass Bad {};\n')
            config = root / 'Doxyfile'
            config.write_text(f'INPUT={root / "bad.h"}\nOUTPUT_DIRECTORY={root / "docs"}\nGENERATE_LATEX=NO\nQUIET=YES\nWARN_AS_ERROR=YES\nEXTRACT_ALL=YES\n')
            result = subprocess.run(['doxygen', str(config)], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'no_such_contract_type', result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
