"""Windows process identity and process-tree resource metrics using ctypes."""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import sys
import time
from typing import Any


if sys.platform == "win32":
    import ctypes
    from ctypes import wintypes

    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _psapi = ctypes.WinDLL("psapi", use_last_error=True)

    _PROCESS_QUERY_INFORMATION = 0x0400
    _PROCESS_VM_READ = 0x0010
    _PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    _TH32CS_SNAPPROCESS = 0x00000002
    _INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
    _MAX_PATH = 260

    class _PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [
            ("dwSize", wintypes.DWORD),
            ("cntUsage", wintypes.DWORD),
            ("th32ProcessID", wintypes.DWORD),
            ("th32DefaultHeapID", ctypes.c_size_t),
            ("th32ModuleID", wintypes.DWORD),
            ("cntThreads", wintypes.DWORD),
            ("th32ParentProcessID", wintypes.DWORD),
            ("pcPriClassBase", wintypes.LONG),
            ("dwFlags", wintypes.DWORD),
            ("szExeFile", wintypes.WCHAR * _MAX_PATH),
        ]

    class _PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD),
            ("PageFaultCount", wintypes.DWORD),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
            ("PrivateUsage", ctypes.c_size_t),
        ]

    class _IO_COUNTERS(ctypes.Structure):
        _fields_ = [
            ("ReadOperationCount", ctypes.c_ulonglong),
            ("WriteOperationCount", ctypes.c_ulonglong),
            ("OtherOperationCount", ctypes.c_ulonglong),
            ("ReadTransferCount", ctypes.c_ulonglong),
            ("WriteTransferCount", ctypes.c_ulonglong),
            ("OtherTransferCount", ctypes.c_ulonglong),
        ]

    _kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    _kernel32.OpenProcess.restype = wintypes.HANDLE
    _kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    _kernel32.CloseHandle.restype = wintypes.BOOL
    _kernel32.QueryFullProcessImageNameW.argtypes = [
        wintypes.HANDLE,
        wintypes.DWORD,
        wintypes.LPWSTR,
        ctypes.POINTER(wintypes.DWORD),
    ]
    _kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
    _kernel32.GetProcessTimes.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME),
    ]
    _kernel32.GetProcessTimes.restype = wintypes.BOOL
    _kernel32.GetProcessIoCounters.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(_IO_COUNTERS),
    ]
    _kernel32.GetProcessIoCounters.restype = wintypes.BOOL
    _kernel32.GetProcessHandleCount.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(wintypes.DWORD),
    ]
    _kernel32.GetProcessHandleCount.restype = wintypes.BOOL
    _kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    _kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    _kernel32.Process32FirstW.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(_PROCESSENTRY32W),
    ]
    _kernel32.Process32FirstW.restype = wintypes.BOOL
    _kernel32.Process32NextW.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(_PROCESSENTRY32W),
    ]
    _kernel32.Process32NextW.restype = wintypes.BOOL
    _psapi.GetProcessMemoryInfo.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(_PROCESS_MEMORY_COUNTERS_EX),
        wintypes.DWORD,
    ]
    _psapi.GetProcessMemoryInfo.restype = wintypes.BOOL


class ProcessMetricError(OSError):
    pass


@dataclass(frozen=True)
class ProcessIdentity:
    pid: int
    creation_time_100ns: int
    image_path: str


@dataclass(frozen=True)
class ProcessTreeSample:
    timestamp_monotonic_ns: int
    pids: tuple[int, ...]
    working_set_bytes: int
    peak_working_set_bytes: int
    private_bytes: int
    peak_private_bytes: int
    kernel_time_100ns: int
    user_time_100ns: int
    read_operations: int
    write_operations: int
    read_bytes: int
    write_bytes: int
    thread_count: int
    handle_count: int
    inaccessible_pids: tuple[int, ...]

    def as_dict(self) -> dict[str, Any]:
        return {
            "timestamp_monotonic_ns": str(self.timestamp_monotonic_ns),
            "pids": list(self.pids),
            "working_set_bytes": str(self.working_set_bytes),
            "peak_working_set_bytes": str(self.peak_working_set_bytes),
            "private_bytes": str(self.private_bytes),
            "peak_private_bytes": str(self.peak_private_bytes),
            "kernel_time_100ns": str(self.kernel_time_100ns),
            "user_time_100ns": str(self.user_time_100ns),
            "read_operations": str(self.read_operations),
            "write_operations": str(self.write_operations),
            "read_bytes": str(self.read_bytes),
            "write_bytes": str(self.write_bytes),
            "thread_count": self.thread_count,
            "handle_count": self.handle_count,
            "inaccessible_pids": list(self.inaccessible_pids),
        }


def _require_windows() -> None:
    if sys.platform != "win32":
        raise ProcessMetricError("Windows process metrics are only available on win32")


def _filetime_value(value: "wintypes.FILETIME") -> int:
    return (int(value.dwHighDateTime) << 32) | int(value.dwLowDateTime)


def _open_process(pid: int):
    access = _PROCESS_QUERY_INFORMATION | _PROCESS_VM_READ
    handle = _kernel32.OpenProcess(access, False, pid)
    if not handle:
        handle = _kernel32.OpenProcess(_PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        error = ctypes.get_last_error()
        raise ProcessMetricError(error, f"OpenProcess({pid}) failed")
    return handle


def capture_process_identity(pid: int) -> ProcessIdentity:
    _require_windows()
    handle = _open_process(pid)
    try:
        creation = wintypes.FILETIME()
        exit_time = wintypes.FILETIME()
        kernel = wintypes.FILETIME()
        user = wintypes.FILETIME()
        if not _kernel32.GetProcessTimes(
            handle,
            ctypes.byref(creation),
            ctypes.byref(exit_time),
            ctypes.byref(kernel),
            ctypes.byref(user),
        ):
            error = ctypes.get_last_error()
            raise ProcessMetricError(error, f"GetProcessTimes({pid}) failed")
        capacity = wintypes.DWORD(32768)
        image = ctypes.create_unicode_buffer(capacity.value)
        if not _kernel32.QueryFullProcessImageNameW(
            handle, 0, image, ctypes.byref(capacity)
        ):
            error = ctypes.get_last_error()
            raise ProcessMetricError(error, f"QueryFullProcessImageNameW({pid}) failed")
        return ProcessIdentity(pid, _filetime_value(creation), image.value)
    finally:
        _kernel32.CloseHandle(handle)


def _process_entries() -> dict[int, tuple[int, int]]:
    snapshot = _kernel32.CreateToolhelp32Snapshot(_TH32CS_SNAPPROCESS, 0)
    if snapshot == _INVALID_HANDLE_VALUE:
        error = ctypes.get_last_error()
        raise ProcessMetricError(error, "CreateToolhelp32Snapshot failed")
    entries: dict[int, tuple[int, int]] = {}
    try:
        entry = _PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        if _kernel32.Process32FirstW(snapshot, ctypes.byref(entry)):
            while True:
                entries[int(entry.th32ProcessID)] = (
                    int(entry.th32ParentProcessID),
                    int(entry.cntThreads),
                )
                entry.dwSize = ctypes.sizeof(entry)
                if not _kernel32.Process32NextW(snapshot, ctypes.byref(entry)):
                    break
        return entries
    finally:
        _kernel32.CloseHandle(snapshot)


def process_tree_pids(root_pid: int) -> tuple[int, ...]:
    _require_windows()
    entries = _process_entries()
    selected = {root_pid}
    changed = True
    while changed:
        changed = False
        for pid, (parent, _) in entries.items():
            if pid not in selected and parent in selected:
                selected.add(pid)
                changed = True
    return tuple(sorted(selected))


def _sample_process(pid: int) -> dict[str, int]:
    handle = _open_process(pid)
    try:
        memory = _PROCESS_MEMORY_COUNTERS_EX()
        memory.cb = ctypes.sizeof(memory)
        if not _psapi.GetProcessMemoryInfo(
            handle, ctypes.byref(memory), memory.cb
        ):
            error = ctypes.get_last_error()
            raise ProcessMetricError(error, f"GetProcessMemoryInfo({pid}) failed")
        creation = wintypes.FILETIME()
        exit_time = wintypes.FILETIME()
        kernel = wintypes.FILETIME()
        user = wintypes.FILETIME()
        if not _kernel32.GetProcessTimes(
            handle,
            ctypes.byref(creation),
            ctypes.byref(exit_time),
            ctypes.byref(kernel),
            ctypes.byref(user),
        ):
            error = ctypes.get_last_error()
            raise ProcessMetricError(error, f"GetProcessTimes({pid}) failed")
        io = _IO_COUNTERS()
        if not _kernel32.GetProcessIoCounters(handle, ctypes.byref(io)):
            error = ctypes.get_last_error()
            raise ProcessMetricError(error, f"GetProcessIoCounters({pid}) failed")
        handles = wintypes.DWORD()
        if not _kernel32.GetProcessHandleCount(handle, ctypes.byref(handles)):
            handles.value = 0
        return {
            "working_set": int(memory.WorkingSetSize),
            "peak_working_set": int(memory.PeakWorkingSetSize),
            "private": int(memory.PrivateUsage),
            "peak_private": int(memory.PeakPagefileUsage),
            "kernel": _filetime_value(kernel),
            "user": _filetime_value(user),
            "read_operations": int(io.ReadOperationCount),
            "write_operations": int(io.WriteOperationCount),
            "read_bytes": int(io.ReadTransferCount),
            "write_bytes": int(io.WriteTransferCount),
            "handles": int(handles.value),
        }
    finally:
        _kernel32.CloseHandle(handle)


def sample_process_tree(root_pid: int) -> ProcessTreeSample:
    _require_windows()
    entries = _process_entries()
    pids = process_tree_pids(root_pid)
    totals = {
        "working_set": 0,
        "peak_working_set": 0,
        "private": 0,
        "peak_private": 0,
        "kernel": 0,
        "user": 0,
        "read_operations": 0,
        "write_operations": 0,
        "read_bytes": 0,
        "write_bytes": 0,
        "handles": 0,
    }
    inaccessible: list[int] = []
    for pid in pids:
        try:
            values = _sample_process(pid)
        except ProcessMetricError:
            inaccessible.append(pid)
            continue
        for key, value in values.items():
            totals[key] += value
    return ProcessTreeSample(
        timestamp_monotonic_ns=time.monotonic_ns(),
        pids=pids,
        working_set_bytes=totals["working_set"],
        peak_working_set_bytes=totals["peak_working_set"],
        private_bytes=totals["private"],
        peak_private_bytes=totals["peak_private"],
        kernel_time_100ns=totals["kernel"],
        user_time_100ns=totals["user"],
        read_operations=totals["read_operations"],
        write_operations=totals["write_operations"],
        read_bytes=totals["read_bytes"],
        write_bytes=totals["write_bytes"],
        thread_count=sum(entries.get(pid, (0, 0))[1] for pid in pids),
        handle_count=totals["handles"],
        inaccessible_pids=tuple(inaccessible),
    )


def same_process(expected: ProcessIdentity) -> bool:
    try:
        actual = capture_process_identity(expected.pid)
    except ProcessMetricError:
        return False
    return (
        actual.creation_time_100ns == expected.creation_time_100ns
        and os.path.normcase(os.path.abspath(actual.image_path))
        == os.path.normcase(os.path.abspath(expected.image_path))
    )
