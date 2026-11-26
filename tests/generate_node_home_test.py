#!/usr/bin/env python3
"""Exercise bounded future catalogs and HTML escaping without touching inputs."""
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('node_home_generator', Path(__file__).resolve().parents[1] / 'scripts/generate_node_home.py')
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class HomeGeneratorTest(unittest.TestCase):
    def render(self, mutate=lambda value: None):
        with tempfile.TemporaryDirectory(prefix='fortis-home-generator-') as temporary:
            root = Path(temporary)
            (root / 'contracts').mkdir()
            (root / 'assets').mkdir()
            catalog = json.loads((generator.ROOT / 'contracts/node-future-features.json').read_text())
            mutate(catalog)
            (root / 'contracts/node-future-features.json').write_text(json.dumps(catalog))
            (root / 'assets/node-home.html').write_text('@FUTURE_NOTICE@ @FUTURE_FEATURES@ @SERVICES@')
            for asset in generator.ASSETS:
                (root / 'assets' / asset).write_bytes(b'asset')
            with patch.object(generator, 'ROOT', root):
                return list(generator.outputs().values())[0]

    def test_escaped_copy(self):
        def malicious(catalog):
            catalog['features'][0]['name'] = '<script>bad</script>'
            catalog['notice'] = '<img src=x onerror=bad>'
        rendered = self.render(malicious)
        # JSON stays verbatim; the HTML fragment must escape display content.
        html = rendered.split('const char kNodeFutureFeaturesJson')[0]
        self.assertIn('&lt;script&gt;bad&lt;/script&gt;', html)
        self.assertNotIn('<script>', html)
        self.assertIn('&lt;img src=x onerror=bad&gt;', html)

    def test_invalid_catalogs(self):
        mutations = [lambda c: c.update(schemaVersion=True), lambda c: c.update(features='wrong'), lambda c: c.update(schemaVersion=2), lambda c: c.update(status='enabled'),
                     lambda c: c.update(features=[]), lambda c: c.update(features=c['features'] * 4),
                     lambda c: c['features'].append(c['features'][0].copy()),
                     lambda c: c['features'][0].update(id='../vpn'),
                     lambda c: c['features'][0].update(name='a' * 241),
                     lambda c: c['features'][0].update(extra=True),
                     lambda c: c.update(notice='')]
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                with self.assertRaises(ValueError):
                    self.render(mutate)


if __name__ == '__main__':
    unittest.main()
