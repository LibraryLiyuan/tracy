"""Backend-neutral semantic projection for Full Worker versus Session Query."""

from __future__ import annotations

import hashlib
import re
from typing import Any, Mapping

from .io_utils import canonical_json_bytes


_TRACE_REF = re.compile(r"\btracy:v1:[0-9A-Fa-f]{16}:")
_TOP_LEVEL_TRANSPORT = frozenset({"budget", "trace"})

# These projections are visible in the report. Domain quality and warnings
# are never normalized away. Paging continuity is checked independently.
SEMANTIC_PROJECTIONS = (
    "Query top-level budget: measured separately per command/probe",
    "Query top-level trace envelope: backend identity checked separately",
    "Opaque next_cursor: logical pagination checked separately",
    "tracy:v1 reference fingerprint: normalize only trace prefix, preserve node kind/id",
)
_CURSOR_KEYS = frozenset({"next_cursor"})


def _normalize(value: Any, *, top_level: bool = False) -> Any:
    if isinstance(value, Mapping):
        result = {}
        for key, child in value.items():
            if top_level and key in _TOP_LEVEL_TRANSPORT:
                continue
            if key in _CURSOR_KEYS:
                continue
            result[str(key)] = _normalize(child)
        return result
    if isinstance(value, list):
        return [_normalize(child) for child in value]
    if isinstance(value, tuple):
        return [_normalize(child) for child in value]
    if isinstance(value, str):
        return _TRACE_REF.sub("tracy:v1:<trace>:", value)
    return value


def stable_response_projection(response: Mapping[str, Any]) -> dict[str, Any]:
    return _normalize(response, top_level=True)


def stable_response_hash(response: Mapping[str, Any]) -> str:
    return hashlib.sha256(
        canonical_json_bytes(stable_response_projection(response))
    ).hexdigest()
