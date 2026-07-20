#!/usr/bin/env python3
"""Independent verifier for tracy-sqliteexport schema version 1."""

from __future__ import annotations

import argparse
import hashlib
import json
import sqlite3
import sys
import time
from pathlib import Path


REQUIRED_AUDITS = {
    "raw_capture_chunks",
    "capture_metadata",
    "cpu_topology",
    "crash_event",
    "string_data",
    "string_map",
    "thread_names",
    "external_names",
    "thread_compression",
    "frame_sets",
    "frames",
    "source_locations_static",
    "source_location_expand",
    "source_locations_dynamic",
    "source_location_zone_counts",
    "zone_extras",
    "locks",
    "lock_threads",
    "lock_events",
    "threads",
    "messages",
    "thread_messages",
    "cpu_zones",
    "thread_samples",
    "gpu_contexts",
    "gpu_context_note_names",
    "gpu_context_threads",
    "gpu_zones",
    "gpu_notes",
    "plots",
    "plot_samples",
    "memory_pools",
    "memory_events",
    "memory_active",
    "memory_frees",
    "callstacks",
    "callstack_items",
    "callstack_frame_definitions",
    "callstack_frame_entries",
    "app_info",
    "frame_image_dictionary",
    "frame_images",
    "context_switch_threads",
    "context_switches",
    "cpu_context_switches",
    "tid_pid",
    "cpu_thread_stats",
    "symbols",
    "symbol_locations",
    "symbol_code",
    "code_symbol_map",
    "hardware_samples",
    "source_cache",
    "derived_cpu_usage",
    "derived_zone_statistics",
    "derived_zone_statistics_threads",
    "derived_symbol_statistics",
    "derived_symbol_stat_parents",
    "derived_symbol_samples",
    "derived_child_samples",
    "derived_instruction_pointers",
    "fiber_thread_map",
}

MAJOR_TABLES = (
    "threads",
    "cpu_zones",
    "thread_samples",
    "messages",
    "gpu_contexts",
    "gpu_zones",
    "gpu_notes",
    "plots",
    "plot_samples",
    "memory_pools",
    "memory_events",
    "callstacks",
    "callstack_items",
    "frame_images",
    "context_switches",
    "cpu_context_switches",
    "symbols",
    "hardware_samples",
)


class VerificationError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def scalar(conn: sqlite3.Connection, sql: str, params: tuple = ()):
    row = conn.execute(sql, params).fetchone()
    require(row is not None, f"query returned no row: {sql}")
    return row[0]


def file_sha256(path: Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        while block := stream.read(16 * 1024 * 1024):
            digest.update(block)
            size += len(block)
    return size, digest.hexdigest()


def verify_hashed_blobs(
    conn: sqlite3.Connection,
    table: str,
    key_column: str,
    length_column: str,
    hash_column: str,
    data_column: str,
) -> int:
    count = 0
    query = (
        f'SELECT "{key_column}", "{length_column}", "{hash_column}", "{data_column}" '
        f'FROM "{table}"'
    )
    for key, declared_length, declared_hash, data in conn.execute(query):
        data = bytes(data)
        require(
            declared_length == len(data),
            f"{table}[{key!r}] byte length mismatch",
        )
        require(
            hashlib.sha256(data).hexdigest() == declared_hash,
            f"{table}[{key!r}] SHA-256 mismatch",
        )
        count += 1
    return count


def verify_no_rows(conn: sqlite3.Connection, sql: str, message: str) -> None:
    row = conn.execute(sql).fetchone()
    require(row is None, f"{message}: {row!r}")


def verify(database: Path, trace: Path | None) -> dict:
    started = time.time()
    print("[verify] opening database read-only", flush=True)
    uri = database.resolve().as_uri() + "?mode=ro"
    conn = sqlite3.connect(uri, uri=True)
    conn.execute("PRAGMA query_only=ON")

    meta = dict(conn.execute("SELECT key, value FROM meta"))
    required_meta = {
        "exporter_schema_version": "1",
        "tracy_version": "0.13.1",
        "export_complete": "1",
        "raw_capture_reconstruction_validated": "1",
        "input_unchanged_through_export": "1",
        "foreign_key_check": "ok",
        "integrity_check": "ok",
    }
    for key, expected in required_meta.items():
        require(meta.get(key) == expected, f"meta[{key!r}] is not {expected!r}")

    print("[verify] checking audit manifest and table counts", flush=True)
    audits = {
        row[0]: {
            "table": row[1],
            "source": row[2],
            "exported": row[3],
            "database": row[4],
            "status": row[5],
            "details": row[6],
        }
        for row in conn.execute(
            "SELECT section, table_name, source_count, exported_count, "
            "database_count, status, details FROM export_audit"
        )
    }
    missing = REQUIRED_AUDITS - audits.keys()
    require(not missing, f"missing required audit sections: {sorted(missing)}")
    for section, audit in audits.items():
        require(audit["status"] == "PASS", f"audit {section} is not PASS")
        require(
            audit["source"] == audit["exported"] == audit["database"],
            f"audit {section} count mismatch",
        )
        actual = scalar(conn, f'SELECT count(*) FROM "{audit["table"]}"')
        require(actual == audit["database"], f"audit {section} stale database count")

    print("[verify] reconstructing archived .tracy bytes", flush=True)
    reconstructed = hashlib.sha256()
    expected_index = 0
    expected_offset = 0
    for index, offset, byte_count, digest, data in conn.execute(
        "SELECT chunk_index, byte_offset, byte_count, sha256, data "
        "FROM capture_chunks ORDER BY chunk_index"
    ):
        data = bytes(data)
        require(index == expected_index, "capture chunk index sequence mismatch")
        require(offset == expected_offset, "capture chunk byte offset mismatch")
        require(byte_count == len(data), "capture chunk byte count mismatch")
        require(hashlib.sha256(data).hexdigest() == digest, "capture chunk SHA-256 mismatch")
        reconstructed.update(data)
        expected_index += 1
        expected_offset += len(data)
    require(expected_offset == int(meta["input_size"]), "reconstructed input size mismatch")
    require(reconstructed.hexdigest() == meta["input_sha256"], "reconstructed input SHA-256 mismatch")

    if trace is not None:
        print("[verify] hashing external source trace", flush=True)
        trace_size, trace_hash = file_sha256(trace)
        require(trace_size == expected_offset, "external trace size mismatch")
        require(trace_hash == meta["input_sha256"], "external trace SHA-256 mismatch")

    print("[verify] checking embedded blob hashes", flush=True)
    verify_hashed_blobs(
        conn,
        "frame_image_dictionary",
        "singleton",
        "byte_count",
        "sha256",
        "data",
    )
    verify_hashed_blobs(
        conn,
        "frame_images",
        "frame_image_index",
        "packed_byte_count",
        "packed_sha256",
        "packed_data",
    )
    verify_hashed_blobs(
        conn,
        "frame_images",
        "frame_image_index",
        "unpacked_byte_count",
        "unpacked_sha256",
        "unpacked_data",
    )
    verify_hashed_blobs(
        conn,
        "symbol_code",
        "symbol_address_hex",
        "byte_count",
        "sha256",
        "data",
    )
    verify_hashed_blobs(
        conn,
        "source_cache",
        "source_path_text",
        "byte_count",
        "sha256",
        "data",
    )

    print("[verify] checking semantic cross-table invariants", flush=True)
    require(
        scalar(conn, "SELECT coalesce(sum(zone_count), 0) FROM threads")
        == scalar(conn, "SELECT count(*) FROM cpu_zones"),
        "thread zone totals do not match cpu_zones",
    )
    require(
        scalar(conn, "SELECT coalesce(sum(event_count), 0) FROM gpu_contexts")
        == scalar(conn, "SELECT count(*) FROM gpu_zones"),
        "GPU context event totals do not match gpu_zones",
    )
    require(
        scalar(conn, "SELECT coalesce(sum(frame_count), 0) FROM callstacks")
        == scalar(conn, "SELECT count(*) FROM callstack_items"),
        "callstack frame totals do not match callstack_items",
    )
    require(
        scalar(conn, "SELECT coalesce(sum(event_count), 0) FROM context_switch_threads")
        == scalar(conn, "SELECT count(*) FROM context_switches"),
        "context-switch thread totals do not match context_switches",
    )
    require(
        scalar(
            conn,
            "SELECT count(*) FROM string_data "
            "WHERE byte_count <> length(value_blob)",
        )
        == 0,
        "string_data contains a byte length mismatch",
    )
    verify_no_rows(
        conn,
        "SELECT f.frame_set_id, f.frame_index, f.frame_image_index "
        "FROM frames f LEFT JOIN frame_images i "
        "ON i.frame_image_index=f.frame_image_index "
        "WHERE f.frame_image_index >= 0 AND i.frame_image_index IS NULL LIMIT 1",
        "frame references a missing frame image",
    )
    verify_no_rows(
        conn,
        "SELECT z.zone_id, z.source_location_id FROM cpu_zones z "
        "LEFT JOIN source_location_expand s ON s.source_location_id=z.source_location_id "
        "LEFT JOIN source_locations_dynamic d ON d.source_location_id=z.source_location_id "
        "WHERE s.source_location_id IS NULL AND d.source_location_id IS NULL LIMIT 1",
        "CPU zone references a missing source location id",
    )
    verify_no_rows(
        conn,
        "SELECT z.gpu_zone_id, z.source_location_id FROM gpu_zones z "
        "LEFT JOIN source_location_expand s ON s.source_location_id=z.source_location_id "
        "LEFT JOIN source_locations_dynamic d ON d.source_location_id=z.source_location_id "
        "WHERE s.source_location_id IS NULL AND d.source_location_id IS NULL LIMIT 1",
        "GPU zone references a missing source location id",
    )
    verify_no_rows(
        conn,
        "SELECT z.zone_id, z.extra_id FROM cpu_zones z "
        "LEFT JOIN zone_extras e ON e.extra_id=z.extra_id "
        "WHERE e.extra_id IS NULL LIMIT 1",
        "CPU zone references a missing zone extra",
    )

    u64_pairs = {
        "capture_metadata": (
            ("frame_offset_u64", "frame_offset_hex"),
            ("pid_u64", "pid_hex"),
            ("capture_time_u64", "capture_time_hex"),
            ("executable_time_u64", "executable_time_hex"),
        ),
        "threads": (("thread_u64", "thread_hex"),),
        "messages": (
            ("original_pointer_u64", "original_pointer_hex"),
            ("thread_u64", "thread_hex"),
        ),
        "gpu_contexts": (
            ("owning_thread_u64", "owning_thread_hex"),
            ("overflow_u64", "overflow_hex"),
        ),
        "memory_events": (
            ("pointer_u64", "pointer_hex"),
            ("size_u64", "size_hex"),
            ("allocation_thread_u64", "allocation_thread_hex"),
            ("free_thread_u64", "free_thread_hex"),
        ),
        "symbols": (("symbol_address_u64", "symbol_address_hex"),),
    }
    for table, pairs in u64_pairs.items():
        for blob_column, hex_column in pairs:
            bad = scalar(
                conn,
                f'SELECT count(*) FROM "{table}" '
                f'WHERE length("{blob_column}") <> 8 OR length("{hex_column}") <> 16',
            )
            require(bad == 0, f"{table}.{blob_column}/{hex_column} exact-u64 shape mismatch")

    print("[verify] running SQLite foreign-key and integrity checks", flush=True)
    verify_no_rows(conn, "PRAGMA foreign_key_check", "foreign-key violation")
    integrity_rows = [row[0] for row in conn.execute("PRAGMA integrity_check")]
    require(integrity_rows == ["ok"], f"integrity_check failed: {integrity_rows[:10]}")

    counts = {table: scalar(conn, f'SELECT count(*) FROM "{table}"') for table in MAJOR_TABLES}
    report = {
        "status": "PASS",
        "database": str(database.resolve()),
        "database_size": database.stat().st_size,
        "trace": str(trace.resolve()) if trace else None,
        "trace_size": expected_offset,
        "trace_sha256": meta["input_sha256"],
        "schema_version": meta["exporter_schema_version"],
        "tracy_version": meta["tracy_version"],
        "audit_sections": len(audits),
        "unresolved_static_source_location_keys": scalar(
            conn, "SELECT count(*) FROM source_location_expand WHERE resolved=0"
        ),
        "counts": counts,
        "elapsed_seconds": round(time.time() - started, 3),
    }
    conn.close()
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("database", type=Path)
    parser.add_argument("--trace", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()

    try:
        report = verify(args.database, args.trace)
        rendered = json.dumps(report, indent=2, ensure_ascii=False)
        if args.report:
            args.report.write_text(rendered + "\n", encoding="utf-8")
        print(rendered)
        return 0
    except (OSError, sqlite3.Error, VerificationError) as error:
        print(f"VERIFY ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
