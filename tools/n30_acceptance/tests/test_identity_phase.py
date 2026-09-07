import tempfile
import unittest
from pathlib import Path

from tools.n30_acceptance.identity_phase import (
    GitPorcelainIdentity,
    parse_git_porcelain_v2,
    parse_session_manifest_header,
)


class GitIdentityParsingTests(unittest.TestCase):
    def test_parses_branch_head_upstream_and_tracked_cleanliness(self):
        parsed = parse_git_porcelain_v2(
            "# branch.oid 0123456789abcdef\n"
            "# branch.head feature/JNTracy-N30-LongTraceSessionStore\n"
            "# branch.upstream origin/feature/JNTracy-N30-LongTraceSessionStore\n"
            "# branch.ab +2 -0\n"
            "? build-untracked/\n"
        )
        self.assertEqual(
            parsed,
            GitPorcelainIdentity(
                head="0123456789abcdef",
                branch="feature/JNTracy-N30-LongTraceSessionStore",
                upstream="origin/feature/JNTracy-N30-LongTraceSessionStore",
                ahead=2,
                behind=0,
                tracked_dirty=(),
                untracked=("build-untracked/",),
            ),
        )

    def test_tracked_changes_are_not_confused_with_untracked_outputs(self):
        parsed = parse_git_porcelain_v2(
            "# branch.oid abc\n# branch.head dev\n"
            "1 .M N... 100644 100644 100644 abc abc source.cpp\n"
            "u UU N... 100644 100644 100644 100644 a b c conflict.cpp\n"
            "? cache/\n"
        )
        self.assertEqual(parsed.tracked_dirty, ("source.cpp", "conflict.cpp"))
        self.assertEqual(parsed.untracked, ("cache/",))


class SessionManifestHeaderTests(unittest.TestCase):
    def test_parses_only_authoritative_header_and_stops_before_shards(self):
        header = parse_session_manifest_header(
            'magic 13878316235968074\n'
            'store_schema 1\n'
            'generation "n30-123"\n'
            'state 11\n'
            'source_sha256 "abc"\n'
            'mandatory_derived_complete 1\n'
            'audit_complete 1\n'
            'reason "source_gap:7;clock:2"\n'
            'shard_count 402\n'
            'shard 0 "control" 0 1\n'
        )
        self.assertEqual(header["generation"], "n30-123")
        self.assertEqual(header["state"], 11)
        self.assertEqual(header["shard_count"], 402)
        self.assertNotIn("shard", header)

    def test_rejects_duplicate_or_malformed_header_keys(self):
        with self.assertRaises(ValueError):
            parse_session_manifest_header("store_schema 1\nstore_schema 2\n")
        with self.assertRaises(ValueError):
            parse_session_manifest_header("generation\n")


if __name__ == "__main__":
    unittest.main()
