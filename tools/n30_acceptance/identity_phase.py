"""Strict parsers used by the P0 identity gate."""

from __future__ import annotations

from dataclasses import dataclass
import re
import shlex


_INTEGER = re.compile(r"-?(?:0|[1-9][0-9]*)\Z")


@dataclass(frozen=True)
class GitPorcelainIdentity:
    head: str
    branch: str
    upstream: str
    ahead: int
    behind: int
    tracked_dirty: tuple[str, ...]
    untracked: tuple[str, ...]


def parse_git_porcelain_v2(payload: str) -> GitPorcelainIdentity:
    headers: dict[str, str] = {}
    tracked: list[str] = []
    untracked: list[str] = []
    for line in payload.splitlines():
        if line.startswith("# "):
            key, separator, value = line[2:].partition(" ")
            if not separator or key in headers:
                raise ValueError(f"malformed or duplicate git status header: {line!r}")
            headers[key] = value
        elif line.startswith("1 "):
            fields = line.split(" ", 8)
            if len(fields) != 9:
                raise ValueError(f"malformed ordinary change record: {line!r}")
            tracked.append(fields[8])
        elif line.startswith("2 "):
            fields = line.split(" ", 9)
            if len(fields) != 10:
                raise ValueError(f"malformed rename record: {line!r}")
            tracked.append(fields[9].split("\t", 1)[0])
        elif line.startswith("u "):
            fields = line.split(" ", 10)
            if len(fields) != 11:
                raise ValueError(f"malformed unmerged record: {line!r}")
            tracked.append(fields[10])
        elif line.startswith("? "):
            untracked.append(line[2:])
        elif line.startswith("! ") or not line:
            continue
        else:
            raise ValueError(f"unknown git porcelain-v2 record: {line!r}")

    if "branch.oid" not in headers or "branch.head" not in headers:
        raise ValueError("git status did not provide branch.oid and branch.head")
    ahead = 0
    behind = 0
    if "branch.ab" in headers:
        fields = headers["branch.ab"].split()
        if len(fields) != 2 or not fields[0].startswith("+") or not fields[1].startswith("-"):
            raise ValueError("malformed branch.ab header")
        ahead = int(fields[0][1:], 10)
        behind = int(fields[1][1:], 10)
    return GitPorcelainIdentity(
        head=headers["branch.oid"],
        branch=headers["branch.head"],
        upstream=headers.get("branch.upstream", ""),
        ahead=ahead,
        behind=behind,
        tracked_dirty=tuple(tracked),
        untracked=tuple(untracked),
    )


def parse_session_manifest_header(payload: str) -> dict[str, object]:
    result: dict[str, object] = {}
    for line_number, line in enumerate(payload.splitlines(), 1):
        if not line:
            continue
        fields = shlex.split(line, posix=True)
        if not fields:
            continue
        if fields[0] == "shard":
            break
        if len(fields) != 2:
            raise ValueError(f"malformed manifest header at line {line_number}: {line!r}")
        key, raw_value = fields
        if key in result:
            raise ValueError(f"duplicate manifest header key: {key}")
        result[key] = int(raw_value, 10) if _INTEGER.fullmatch(raw_value) else raw_value
    return result
