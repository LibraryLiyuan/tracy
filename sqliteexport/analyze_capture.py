#!/usr/bin/env python3
"""Produce a capture-specific JSON inventory from tracy-sqliteexport schema v1."""

from __future__ import annotations

import argparse
import json
import sqlite3
from pathlib import Path


def rows(cursor: sqlite3.Cursor) -> list[dict]:
    return [dict(row) for row in cursor]


def json_safe(value):
    if isinstance(value, (bytes, bytearray, memoryview)):
        return {"encoding": "hex", "value": bytes(value).hex()}
    if isinstance(value, dict):
        return {key: json_safe(item) for key, item in value.items()}
    if isinstance(value, list):
        return [json_safe(item) for item in value]
    return value


def scalar(conn: sqlite3.Connection, sql: str, params: tuple = ()):
    row = conn.execute(sql, params).fetchone()
    if row is None:
        raise RuntimeError(f"query returned no row: {sql}")
    return row[0]


def tracy_thread_color(thread_hex: str) -> str:
    """Return Tracy's packed 0xAARRGGBB value from GetHsvColor(tid, 0)."""
    thread = int(thread_hex, 16)
    hue = (thread * 11400714819323198485) & 0xFF
    saturation = 108
    value = 170
    region = hue // 43
    remainder = (hue - region * 43) * 6
    p = (value * (255 - saturation)) >> 8
    q = (value * (255 - ((saturation * remainder) >> 8))) >> 8
    t = (value * (255 - ((saturation * (255 - remainder)) >> 8))) >> 8
    if region == 0:
        red, green, blue = value, t, p
    elif region == 1:
        red, green, blue = q, value, p
    elif region == 2:
        red, green, blue = p, value, t
    elif region == 3:
        red, green, blue = p, q, value
    elif region == 4:
        red, green, blue = t, p, value
    else:
        red, green, blue = value, p, q
    return f"0xFF{red:02X}{green:02X}{blue:02X}"


def analyze(database: Path) -> dict:
    uri = database.resolve().as_uri() + "?mode=ro"
    conn = sqlite3.connect(uri, uri=True)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA query_only=ON")

    meta = dict(conn.execute("SELECT key, value FROM meta"))
    capture = dict(conn.execute("SELECT * FROM capture_metadata").fetchone())
    capture_pid = capture["pid_hex"]

    profiled_threads = rows(
        conn.execute(
            """
            SELECT t.thread_index, t.thread_hex, t.thread_name, t.zone_count,
                   t.kernel_sample_count, t.is_fiber, t.group_hint,
                   e.process_name AS os_process_name,
                   e.thread_name AS os_thread_name,
                   coalesce(c.event_count, 0) AS context_switch_count,
                   coalesce(c.running_time_ns, 0) AS context_running_time_ns,
                   coalesce(s.running_regions, 0) AS cpu_running_regions,
                   coalesce(s.running_time_ns, 0) AS cpu_running_time_ns,
                   coalesce(s.migrations, 0) AS cpu_migrations,
                   (SELECT count(*) FROM thread_samples x
                    WHERE x.thread_index=t.thread_index) AS sample_count,
                   (SELECT count(*) FROM thread_messages x
                    WHERE x.thread_index=t.thread_index) AS message_count
            FROM threads t
            LEFT JOIN external_names e ON e.external_id_hex=t.thread_hex
            LEFT JOIN context_switch_threads c ON c.thread_hex=t.thread_hex
            LEFT JOIN cpu_thread_stats s ON s.tid_hex=t.thread_hex
            ORDER BY t.thread_index
            """
        )
    )
    for thread in profiled_threads:
        thread["tracy_dynamic_color_packed"] = tracy_thread_color(thread["thread_hex"])

    scheduler_threads = rows(
        conn.execute(
            """
            WITH activity AS (
                SELECT thread_hex, count(*) AS running_regions,
                       sum(CASE WHEN end_valid<>0 AND end_ns>=start_ns
                                THEN end_ns-start_ns ELSE 0 END) AS running_time_ns,
                       count(DISTINCT cpu) AS cpu_count
                FROM cpu_context_switches
                GROUP BY thread_hex
            )
            SELECT a.thread_hex,
                   CASE
                     WHEN t.thread_hex IS NOT NULL THEN 'profiled_local'
                     WHEN p.pid_hex=?1 THEN 'same_process_untracked'
                     ELSE 'external'
                   END AS tracy_class,
                   t.thread_name AS profiled_thread_name,
                   e.thread_name AS os_thread_name,
                   coalesce(nullif(t.thread_name, ''), nullif(e.thread_name, ''),
                            '???') AS thread_name,
                   coalesce(e.process_name, '') AS process_name,
                   p.pid_hex,
                   a.running_regions, a.running_time_ns, a.cpu_count
            FROM activity a
            LEFT JOIN threads t ON t.thread_hex=a.thread_hex
            LEFT JOIN tid_pid p ON p.tid_hex=a.thread_hex
            LEFT JOIN external_names e ON e.external_id_hex=a.thread_hex
            ORDER BY a.running_time_ns DESC, a.running_regions DESC
            """,
            (capture_pid,),
        )
    )

    scheduler_class_summary = rows(
        conn.execute(
            """
            WITH activity AS (
                SELECT thread_hex, count(*) AS running_regions,
                       sum(CASE WHEN end_valid<>0 AND end_ns>=start_ns
                                THEN end_ns-start_ns ELSE 0 END) AS running_time_ns
                FROM cpu_context_switches
                GROUP BY thread_hex
            ), classified AS (
                SELECT a.*,
                       CASE
                         WHEN t.thread_hex IS NOT NULL THEN 'profiled_local'
                         WHEN p.pid_hex=?1 THEN 'same_process_untracked'
                         ELSE 'external'
                       END AS tracy_class
                FROM activity a
                LEFT JOIN threads t ON t.thread_hex=a.thread_hex
                LEFT JOIN tid_pid p ON p.tid_hex=a.thread_hex
            )
            SELECT tracy_class, count(*) AS thread_count,
                   sum(running_regions) AS running_regions,
                   sum(running_time_ns) AS running_time_ns
            FROM classified
            GROUP BY tracy_class
            ORDER BY tracy_class
            """,
            (capture_pid,),
        )
    )

    memory_pools = rows(
        conn.execute(
            """
            SELECT p.memory_pool_id, p.name_text, p.map_key_hex, p.stored_name_hex,
                   p.high_hex, p.low_hex, p.usage_hex, p.reconstruct, p.plot_id,
                   count(e.event_index) AS event_count,
                   sum(CASE WHEN e.is_active<>0 THEN 1 ELSE 0 END) AS active_event_count,
                   sum(CASE WHEN e.is_active=0 THEN 1 ELSE 0 END) AS freed_event_count
            FROM memory_pools p
            LEFT JOIN memory_events e ON e.memory_pool_id=p.memory_pool_id
            GROUP BY p.memory_pool_id
            ORDER BY p.memory_pool_id
            """
        )
    )
    for pool in memory_pools:
        allocated_bytes = 0
        active_bytes = 0
        for size_hex, is_active in conn.execute(
            "SELECT size_hex, is_active FROM memory_events WHERE memory_pool_id=?",
            (pool["memory_pool_id"],),
        ):
            size = int(size_hex, 16)
            allocated_bytes += size
            if is_active:
                active_bytes += size
        pool["cumulative_allocated_bytes"] = allocated_bytes
        pool["active_bytes_from_events"] = active_bytes
        pool["current_usage_u64"] = int(pool["usage_hex"], 16)
        pool["address_high_u64"] = int(pool["high_hex"], 16)
        pool["address_low_u64"] = int(pool["low_hex"], 16)

    plots = rows(
        conn.execute(
            """
            SELECT p.plot_id, p.name_text, p.plot_type, p.value_format,
                   p.show_steps, p.fill, p.color, p.origin,
                   p.min_value, p.max_value, p.sum_value,
                   count(s.sample_index) AS sample_count,
                   min(s.time_ns) AS first_time_ns,
                   max(s.time_ns) AS last_time_ns,
                   (SELECT x.value FROM plot_samples x
                    WHERE x.plot_id=p.plot_id ORDER BY x.sample_index LIMIT 1)
                       AS first_value,
                   (SELECT x.value FROM plot_samples x
                    WHERE x.plot_id=p.plot_id ORDER BY x.sample_index DESC LIMIT 1)
                       AS last_value
            FROM plots p
            LEFT JOIN plot_samples s ON s.plot_id=p.plot_id
            GROUP BY p.plot_id
            ORDER BY p.plot_id
            """
        )
    )

    gpu_contexts = rows(
        conn.execute(
            """
            WITH thread_counts AS (
                SELECT gpu_context_id, count(*) AS context_thread_count
                FROM gpu_context_threads GROUP BY gpu_context_id
            ), zone_counts AS (
                SELECT gpu_context_id,
                       sum(CASE WHEN gpu_end_valid<>0 THEN 1 ELSE 0 END)
                           AS complete_zone_count,
                       sum(CASE WHEN gpu_end_valid=0 THEN 1 ELSE 0 END)
                           AS incomplete_zone_count,
                       min(gpu_start_ns) AS first_gpu_zone_ns,
                       max(CASE WHEN gpu_end_valid<>0 THEN gpu_end_ns END)
                           AS last_gpu_zone_ns
                FROM gpu_zones GROUP BY gpu_context_id
            )
            SELECT c.gpu_context_id, c.name_text, c.context_type,
                   c.owning_thread_hex, c.event_count, c.period, c.has_period,
                   c.has_calibration, c.time_diff_ns, c.calibrated_gpu_time_ns,
                   c.calibrated_cpu_time_ns, c.last_gpu_time_ns,
                   coalesce(t.context_thread_count, 0) AS context_thread_count,
                   coalesce(z.complete_zone_count, 0) AS complete_zone_count,
                   coalesce(z.incomplete_zone_count, 0) AS incomplete_zone_count,
                   z.first_gpu_zone_ns, z.last_gpu_zone_ns
            FROM gpu_contexts c
            LEFT JOIN thread_counts t ON t.gpu_context_id=c.gpu_context_id
            LEFT JOIN zone_counts z ON z.gpu_context_id=c.gpu_context_id
            ORDER BY c.gpu_context_id
            """
        )
    )

    gpu_zone_names = rows(
        conn.execute(
            """
            WITH source_names AS (
                SELECT x.source_location_id,
                       coalesce(nullif(s.name_text, ''), nullif(s.function_text, ''),
                                '[unnamed]') AS zone_name,
                       s.file_text, s.line
                FROM source_location_expand x
                LEFT JOIN source_locations_static s
                  ON s.static_key_hex=x.static_key_hex AND x.resolved<>0
                UNION ALL
                SELECT d.source_location_id,
                       coalesce(nullif(d.name_text, ''), nullif(d.function_text, ''),
                                '[unnamed]') AS zone_name,
                       d.file_text, d.line
                FROM source_locations_dynamic d
            )
            SELECT z.gpu_context_id, c.name_text AS context_name,
                   coalesce(n.zone_name,
                            printf('[source_location_id=%d]', z.source_location_id))
                       AS zone_name,
                   coalesce(n.file_text, '') AS file_text,
                   coalesce(n.line, 0) AS line,
                   count(*) AS event_count,
                   sum(CASE WHEN z.gpu_end_valid<>0 AND z.gpu_end_ns>=z.gpu_start_ns
                            THEN z.gpu_end_ns-z.gpu_start_ns ELSE 0 END)
                       AS total_gpu_time_ns,
                   min(CASE WHEN z.gpu_end_valid<>0 AND z.gpu_end_ns>=z.gpu_start_ns
                            THEN z.gpu_end_ns-z.gpu_start_ns END)
                       AS min_gpu_time_ns,
                   max(CASE WHEN z.gpu_end_valid<>0 AND z.gpu_end_ns>=z.gpu_start_ns
                            THEN z.gpu_end_ns-z.gpu_start_ns END)
                       AS max_gpu_time_ns
            FROM gpu_zones z
            JOIN gpu_contexts c ON c.gpu_context_id=z.gpu_context_id
            LEFT JOIN source_names n ON n.source_location_id=z.source_location_id
            GROUP BY z.gpu_context_id, z.source_location_id
            ORDER BY z.gpu_context_id, event_count DESC, total_gpu_time_ns DESC
            """
        )
    )

    frame_images = rows(
        conn.execute(
            """
            SELECT width, height, flip, count(*) AS image_count,
                   sum(packed_byte_count) AS packed_bytes,
                   sum(unpacked_byte_count) AS unpacked_bytes,
                   min(frame_reference) AS first_frame_reference,
                   max(frame_reference) AS last_frame_reference
            FROM frame_images
            GROUP BY width, height, flip
            ORDER BY image_count DESC
            """
        )
    )

    cpu_topology = rows(
        conn.execute(
            """
            SELECT package_id, die_id, core_id, sibling_ordinal, hardware_thread
            FROM cpu_topology
            ORDER BY package_id, die_id, core_id, sibling_ordinal
            """
        )
    )

    report = {
        "database": str(database.resolve()),
        "database_size": database.stat().st_size,
        "meta": meta,
        "capture": capture,
        "app_info": rows(
            conn.execute(
                "SELECT app_info_index, value_text FROM app_info ORDER BY app_info_index"
            )
        ),
        "table_counts": {
            table: scalar(conn, f'SELECT count(*) FROM "{table}"')
            for table in (
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
        },
        "profiled_threads": profiled_threads,
        "scheduler_thread_class_summary": scheduler_class_summary,
        "scheduler_threads": scheduler_threads,
        "cpu": {
            "cpu_data_count": int(meta["cpu_data_count"]),
            "topology_entries": len(cpu_topology),
            "topology": cpu_topology,
            "derived_usage_points": scalar(conn, "SELECT count(*) FROM derived_cpu_usage"),
        },
        "memory_pools": memory_pools,
        "plots": plots,
        "gpu_contexts": gpu_contexts,
        "gpu_zone_names": gpu_zone_names,
        "frame_images": frame_images,
        "frame_sets": rows(
            conn.execute(
                """
                SELECT s.frame_set_id, s.name_text, s.continuous, s.is_base,
                       count(f.frame_index) AS frame_count, s.min_ns, s.max_ns,
                       s.total_ns
                FROM frame_sets s
                LEFT JOIN frames f ON f.frame_set_id=s.frame_set_id
                GROUP BY s.frame_set_id
                ORDER BY s.frame_set_id
                """
            )
        ),
    }
    conn.close()
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("database", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = analyze(args.database)
    rendered = json.dumps(json_safe(report), ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(rendered + "\n", encoding="utf-8")
    print(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
