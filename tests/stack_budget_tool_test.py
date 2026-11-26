#!/usr/bin/env python3
"""Keep release stack measurements distinct from test executables and guesses."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import stack_budget


class StackBudgetTest(unittest.TestCase):
    def test_measurements_and_raw_callgraph(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / 'build'
            build.mkdir()
            (build / 'fortis_node.ltrans0.ltrans.su').write_text('src/a.cpp:1:1:Small\t16\tstatic\nsrc/a.cpp:2:1:Large\t128\tdynamic,bounded\n')
            (build / 'test.ltrans0.ltrans.su').write_text('test.cpp:1:1:Fixture\t999999\tstatic\n')
            (build / 'fortis_node.ltrans0.ltrans.ci').write_text('graph: {}\n')
            output = root / 'report'
            result = stack_budget.collect(build, output)
            self.assertEqual(result['maximum_frame_bytes'], 128)
            self.assertEqual(len(result['frames']), 2)
            self.assertEqual(json.loads((output / 'report.json').read_text()), result)
            self.assertEqual((output / 'fortis_node.ltrans0.ltrans.ci').read_text(), 'graph: {}\n')

    def test_missing_or_malformed_records_refused(self):
        for contents in (None, 'not a compiler stack report\n'):
            with self.subTest(contents=contents), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                build = root / 'build'
                build.mkdir()
                if contents is not None:
                    (build / 'fortis_node.ltrans0.ltrans.su').write_text(contents)
                with self.assertRaises(ValueError):
                    stack_budget.collect(build, root / 'output')

    def test_large_or_unbounded_frames_fail_closed(self):
        for frame in ({'function': 'src/a.cpp:1:1:Unexpected', 'bytes': 98305, 'kind': 'static'},
                      {'function': 'src/a.cpp:1:1:Unbounded', 'bytes': 32, 'kind': 'dynamic'},
                      {'function': 'src/prices/spot_owner_batches.cpp:1:1:Other', 'bytes': 100000, 'kind': 'static'},
                      {'function': 'src/prices/spot_owner_batches.cpp:1:1:CompleteBatch', 'bytes': 393217, 'kind': 'static'}):
            with self.subTest(frame=frame), self.assertRaises(ValueError):
                stack_budget.validate_frames([frame])
        stack_budget.validate_frames([{'function': 'src/prices/spot_owner_batches.cpp:1:1:CompleteBatch',
                                       'bytes': 350672, 'kind': 'dynamic,bounded'}])

    def test_explicit_stack_inventory_refuses_unknown_expression(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'src'
            (source / 'base').mkdir(parents=True)
            (source / 'base/threading.h').write_text('pthread_attr_setstacksize(&attr, stack_bytes);\n')
            worker = source / 'worker.cpp'
            worker.write_text('constexpr size_t kStack = 1024 * 1024;\npthread_attr_setstacksize(&attr, kStack);\nworker.Start(Run, this, 256 * 1024);\n')
            self.assertEqual([row['bytes'] for row in stack_budget.thread_stacks(source)], [1048576, 262144])
            worker.write_text('pthread_attr_setstacksize(&attr, UnknownRuntimeBudget());\n')
            with self.assertRaises(ValueError):
                stack_budget.thread_stacks(source)


if __name__ == '__main__':
    unittest.main()
