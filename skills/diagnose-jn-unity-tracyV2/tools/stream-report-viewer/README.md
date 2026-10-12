# Native stream report viewer

Display defaults to device-pixel 1:1, correcting CSS dimensions for `devicePixelRatio` instead of stretching the bitmap to 100% width. The selector also offers integer 2x (pixel-preserving enlargement) and fit-width (explicitly resampled overview). It remains available in fullscreen. Refreshing an already-open report attaches to its current native navigation/range; the reset button explicitly returns to the report anchor. `node test_display.js` checks DPR/viewport geometry and inverse input coordinates. These controls do not increase native resolution or claim high-DPI native rerasterization.

`stream-report-2.1` renders the web view into a fixed 1600×1000 offscreen framebuffer. It is independent of the hidden desktop window's maximize, DPI, size and iconified state; the 8 MiB transport cannot silently overflow when the desktop framebuffer becomes larger. A successful navigation is not considered a visible page until a matching-navigation frame reaches the browser. Missing frames and superseded navigations are reported explicitly. Frontend logic is in `viewer.js`, inserted into the HTML by the local server.

Report links open an interactive native Tracy canvas in the browser, plus a preview gallery. The hidden native Profiler reads the existing `.tracy-stream` EventStore directly; this does not convert the trace or use Wasm. Inputs target only this renderer's ImGui context, never the OS desktop.

1. `prepare.py --analysis DIR --spec view-spec.json --profiler VERIFIED_EXE --output NEW_REGISTRY.json`
2. `start.py --config NEW_REGISTRY.json --state-dir NEW_RUNTIME_DIR`
3. `export.py --state-dir NEW_RUNTIME_DIR`

Use the actual URL in `server-state.json` for Markdown `/open/<id>` links. `start.py` reuses a matching live server or restarts on the recorded port. One registry owns one hidden renderer; active tabs share its current view. Close unused viewer tabs when switching problems. The server is loopback only, with registered view IDs, Host/Origin checks and a per-instance input token. There is no arbitrary-file or command endpoint.

Wheel zooms, right-drag pans horizontally and scrolls tracks vertically, middle-drag zooms a range, clicking selects events or expands lanes. `stream-report-2` sends inputs during movement and independently delivers the latest native frame at up to about 30 Hz. Motion is coalesced; button edges remain ordered. Frame transfer uses a fixed 8 MiB Windows shared mapping with a consistency guard and lossless PNG encoding in memory. There is no live PNG file queue or wait for backend reads to settle. Toolbar controls also navigate and scroll. Keyboard/text entry is not currently forwarded. The runtime uses Python standard-library PNG encoding; the pixel regression test uses Pillow.

Native reports verify capture content, revision, exact frame boundaries, owner and event identity before exporting original framebuffer PNGs. Query zone refs use snapshot-order indices and are mapped with native EventStore cursors. GPU context refs are also translated from Query context order. `export-validation.json` is separate from analysis correctness. Review the images and record actual UI checks before claiming presentation completion.

`python -m unittest discover -s . -p 'test_*.py' -v` checks identity/receipt/frame consistency guards. `benchmark.py --url URL --output NEW_JSON` checks latency and 120 live drag inputs at 60 Hz, intermediate output frames, native queue depth and release acknowledgement. Record browser paint latency separately. Old `tracy-links-20261012` checks established functionality but missed the roughly 1-second interaction delay; do not reuse them as a performance pass. The realtime correction evidence is in `tracy-web-realtime-20261012`. No query statistics are calculated by these scripts.

Build the native support with the existing Tracy profiler CMake target. Runtime flags are `--stream FILE --report-channel DIR --report-web 1`. Normal desktop stream mode omits `--report-web 1`. Analysis-only use should consume a pinned, previously verified EXE rather than rebuilding.
