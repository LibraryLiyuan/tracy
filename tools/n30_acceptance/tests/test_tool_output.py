import unittest

from tools.n30_acceptance.tool_output import ToolOutputDecoder


class ToolOutputDecoderTests(unittest.TestCase):
    def test_utf8_split_at_multibyte_boundary_is_not_corrupted(self):
        decoder = ToolOutputDecoder()
        raw = "[COMPARE] 中文通过\n".encode("utf-8")
        text = decoder.feed(raw[:11]) + decoder.feed(raw[11:]) + decoder.finish()
        self.assertEqual(text, raw.decode("utf-8"))

    def test_msbuild_chinese_uses_explicit_windows_fallback(self):
        decoder = ToolOutputDecoder(fallback_encoding="cp936")
        raw = "正在生成代码\r\n已完成代码的生成\r\n".encode("cp936")
        self.assertEqual(decoder.feed(raw) + decoder.finish(), raw.decode("cp936"))

    def test_final_line_without_newline_is_not_lost(self):
        decoder = ToolOutputDecoder()
        self.assertEqual(decoder.feed(b"last message"), "")
        self.assertEqual(decoder.finish(), "last message")

    def test_invalid_bytes_are_visible_not_silently_removed(self):
        decoder = ToolOutputDecoder(fallback_encoding="ascii")
        self.assertEqual(decoder.feed(b"bad\xff\n"), "bad\\xff\n")


if __name__ == "__main__":
    unittest.main()
