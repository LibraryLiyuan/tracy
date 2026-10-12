"""Structured gap details must survive every report path without weakening validation."""
import copy
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

from tests.test_query_native_report_contract import query_native_analysis, load_contract, SKILL_ROOT


class StructuredMinimumEvidenceTests(unittest.TestCase):
    def render_paths(self, values):
        with tempfile.TemporaryDirectory() as temp:
            _, _, _, analysis, _ = query_native_analysis(Path(temp))
            gap = {'reason': 'RepresentativeOrCompletionEvidenceIncomplete',
                   'minimum_additional_evidence': values}
            analysis['evidence_gaps'] = [gap]
            original = copy.deepcopy(analysis)
            contract = load_contract()
            sys.path.insert(0, str(SKILL_ROOT / 'scripts'))
            spec = importlib.util.spec_from_file_location('structured_builder', SKILL_ROOT / 'scripts/build_report.py')
            builder = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(builder)
            outputs = {'global': lambda: '\n'.join(contract.render_evidence_gaps(analysis)),
                       'main': lambda: builder.render_main_report(analysis, {}, {}, {}, False),
                       '09': lambda: contract.render_specialty_reports(analysis, {})[
                           '09-Investigation-Backlog-and-Evidence-Gaps.md']}
            for name, render in outputs.items():
                with self.subTest(path=name):
                    try:
                        text = render()
                    except TypeError as exc:
                        self.fail(f'{name} rejects valid structured gap details: {exc}')
                    yield name, text
                    self.assertEqual(analysis, original, 'Rendering must not modify analysis facts')
            analysis['evidence_gaps'] = []
            analysis['bottleneck_signatures'][0]['evidence_gap'] = gap
            original = copy.deepcopy(analysis)
            with self.subTest(path='card'):
                try:
                    text = builder.render_main_report(analysis, {}, {}, {}, False)
                except TypeError as exc:
                    self.fail(f'card rejects valid structured gap details: {exc}')
                yield 'card', text
                self.assertEqual(analysis, original)

    def test_real_frame_scope_object_and_following_text_remain_visible(self):
        # Catches the original join(dict) crash and lossy extraction of only need.
        values = [{'frames': ['4787'], 'need': '不得补造 Zone 或空树。',
                   'frame_scope': 'tracy:v1:da5495e1bc5d8596:frame-set:3'},
                  'native Present 对应的真实 typed completion/继续端。']
        expected = '` {"frame_scope": "tracy:v1:da5495e1bc5d8596:frame-set:3", "frames": ["4787"], "need": "不得补造 Zone 或空树。"} `'
        for _, text in self.render_paths(values):
            self.assertIn(expected, text)
            self.assertIn(values[1], text)

    def test_nested_json_types_and_large_integer_are_not_coerced(self):
        # Catches Python repr, scalar stringification and integer precision loss.
        for _, text in self.render_paths([{'z': [False, None, 9007199254740993], 'a': {'x': 1}}]):
            self.assertIn('` {"a": {"x": 1}, "z": [false, null, 9007199254740993]} `', text)

    def test_structured_markup_is_literal_and_backticks_cannot_close_span(self):
        # Catches an undersized fence and double escaping JSON inside code.
        values = [{'need': '<img src=x> [x](javascript:x) ` `` ``` | C:\\x\n下一行'}]
        expected = '```` {"need": "<img src=x> [x](javascript:x) ` `` ``` | C:\\\\x\\n下一行"} ````'
        for _, text in self.render_paths(values):
            self.assertIn(expected, text)

    def test_object_key_insertion_order_does_not_change_output(self):
        first = dict(self.render_paths([{'z': 2, 'a': 1}]))
        second = dict(self.render_paths([{'a': 1, 'z': 2}]))
        self.assertEqual(first, second)

    def test_legacy_strings_keep_separators_and_markdown_escaping(self):
        for name, text in self.render_paths(['真实 frame', 'C:\\src | next\nline']):
            separator = ', ' if name == 'card' else '；'
            self.assertIn('真实 frame' + separator + 'C:\\\\src \\| next line', text)


if __name__ == '__main__':
    unittest.main()
