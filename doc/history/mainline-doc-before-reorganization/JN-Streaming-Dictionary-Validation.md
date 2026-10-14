> 历史归档：mainline f49190f2541c 中的历史资料。当前版本与操作入口见 [文档首页](../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# Streaming dictionary validation (P6 checkpoint)

Converter validation no longer reloads the complete global string table or callstack payload table into a second Worker.

For global strings, the reader consumes each value through a fixed 64 KiB buffer. The validation sink checks the exact record count, first-seen order, deterministic `ID + 1` token, byte length and the replay-derived SHA-256 stream. Client string, thread name and external name tables are streamed separately; every referenced file token must be within the emitted string table and external names must provide two valid tokens. The validation Worker leaves `stringData`, `stringMap`, `strings`, `threadNames` and `externalNames` empty.

For callstacks, the reader retains only the ID 0 sentinel and uses a fixed 4,096-frame buffer. The sink checks sequential 1-based IDs, nonzero 16-bit frame counts, block offsets, full frame bytes and the replay-derived SHA-256 stream. No historical `VarArray` body is reconstructed during Validate.

Ordinary Query/Profiler loading remains unchanged and still materializes these dictionaries for interactive consumers. The explicit streaming path is selected only by a sink that advertises the capability; unsupported callers keep the normal behavior.

Native tests reopen non-empty string and callstack fixtures twice: once through the ordinary full reader for field comparison, and once through the validation sink to assert that dictionary containers remain empty. The 64,891-byte end-to-end fixture completed Scan, Replay, Finalize, Write, Validate, Publish and Cleanup under the existing 3J limit with the streaming paths enabled. Callstack frame/symbol blobs and remaining historical holders still require separate work before the integrated 30-minute acceptance.
