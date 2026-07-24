# `.tracy-stream` journal format v1

Status: implementation contract on `feature-StructuredData`.

## Goals and non-goals

The journal is an append-only, crash-recoverable transcript of one Tracy TCP
session. It is designed for continuous disk writing, a bounded-memory recorder,
offline replay/conversion, and a live reader that only consumes committed
records.

Version 1 deliberately does not claim seamless reconnect or exactly-once
delivery. One TCP connection is one session. A reconnect creates a new session
identifier and a new journal. TCP backpressure is the v1 flow-control mechanism.
The sequence field and bidirectional records reserve room for a later
application-level ACK/retransmit protocol.

This file is not the normal `.tracy` snapshot format. A completed journal must
be replayed or converted before existing snapshot-only tools consume it.

## Encoding

All integers are unsigned little-endian. Persisted structures are serialized
field-by-field; native C/C++ structure layout is never written. Unknown flag
bits must be ignored. Unknown record types may be skipped after their integrity
checks pass.

### File header (64 bytes)

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | ASCII magic `TRCSTRM1` |
| 8 | 2 | major version (`1`) |
| 10 | 2 | header size (`64`) |
| 12 | 4 | file flags |
| 16 | 4 | Tracy wire protocol version |
| 20 | 4 | reserved, zero |
| 24 | 16 | opaque session identifier |
| 40 | 8 | creation time, Unix nanoseconds |
| 48 | 4 | CRC32C of all 64 bytes with this field zeroed |
| 52 | 12 | reserved, zero |

An invalid file header has no recoverable journal prefix.

### Record header (48 bytes)

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | ASCII magic `TSR1` |
| 4 | 2 | header size (`48`) |
| 6 | 2 | record type |
| 8 | 4 | record flags |
| 12 | 4 | reserved, zero |
| 16 | 8 | sequence, beginning at 1 and increasing by 1 |
| 24 | 8 | monotonic nanoseconds relative to recorder start |
| 32 | 8 | payload size |
| 40 | 4 | payload CRC32C |
| 44 | 4 | header CRC32C with this field zeroed |

### Payload

The payload has exactly `payload size` bytes. Version 1 writers cap a payload at
64 MiB by default. Readers must impose a configured cap before allocation or
streaming.

### Commit trailer (32 bytes)

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | ASCII magic `TCM1` |
| 4 | 2 | trailer size (`32`) |
| 6 | 2 | trailer version (`1`) |
| 8 | 8 | repeated sequence |
| 16 | 8 | total record size, including header and trailer |
| 24 | 4 | CRC32C of record header, payload, and trailer bytes 0–23 |
| 28 | 4 | trailer CRC32C with this field zeroed |

A record becomes committed only when the complete, valid trailer follows its
header and payload. A reader scans from byte 64 and stops at the first
truncated, corrupt, oversized, or out-of-sequence record. The bytes before that
record are the valid recovery prefix. Readers never resynchronize past an
invalid record because doing so could expose causally incomplete protocol data.

## Record types

| Value | Name | Payload |
| ---: | --- | --- |
| 1 | `SessionBegin` | Versioned recorder/session metadata |
| 2 | `ClientToServer` | Exact TCP bytes received from the Tracy client |
| 3 | `ServerToClient` | Exact TCP bytes sent by the recorder, including server queries |
| 4 | `Checkpoint` | Versioned checkpoint metadata; may mark a durable boundary |
| 5 | `SessionEnd` | Versioned terminal reason and counters |
| 6 | `Diagnostic` | Versioned non-replay or diagnostic data |

Wire records preserve exact bytes. Flags distinguish handshake bytes,
compressed frames, and server-query packets. This matters because Tracy is a
bidirectional protocol: the server requests source locations, strings, call
stacks, frame images, and other definitions while capture is in progress.

Flag bit 5 (`RecordFlagLocalControl`) marks recorder-local state stored as
`Diagnostic`; it is not a TCP packet. Its `BeginDrain` payload is independently
versioned without changing the journal file-format version:

| Payload | Meaning |
| --- | --- |
| one byte `1` | Legacy server-only drain. |
| one byte `2` | Send native `ServerQueryDisconnect`, then use the original full symbol-expansion behavior. |
| one byte `3` | Send native disconnect and defer optional instruction-level symbol expansion. |
| five bytes: `4`, then LE `uint32` | Current behavior; version 3 semantics plus the recorded server-query window. |

For version 4 the query window must be in `[1, 8192]`. It is the exact
`m_serverQuerySpaceBase` selected for the live socket. Replay restores this
value before processing frames because a replay socket can have a different
send-buffer size; deriving a new window could reorder priority and ordinary
queries and break byte-for-byte validation.

At the current v4 boundary the recorder sends the ordinary recorded
`ServerQueryDisconnect`. An on-demand Client stops accepting new normal
producer events while continuing to serialize its pre-disconnect backlog
(finite at the boundary, but not protected by a Client hard limit) and
asynchronous definition responses. `ProtocolOnly` keeps callstack
function, file, and line data, but does not recursively request optional symbol
code and disassembly addresses. After the remaining required definitions are
resolved, the recorder sends the ordinary recorded `ServerQueryTerminate`.

Replay excludes the diagnostic payload from both wire directions, restores
the matching drain and symbol-expansion behavior at the same compressed-frame
boundary, and validates the regenerated query stream and final terminate
packet byte for byte. Readers accept legacy payload versions 1 through 3 for
existing journals; current writers emit version 4.

### Versioned metadata payloads

`SessionBegin` starts with a 24-byte little-endian payload header. Payload
version 1 is the legacy layout; current writers emit version 2:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 2 | payload version (`1` legacy, `2` current) |
| 2 | 2 | fixed header size (`24`) |
| 4 | 4 | Tracy protocol version |
| 8 | 2 | TCP port |
| 10 | 2 | reserved |
| 12 | 4 | UTF-8 address byte count |
| 16 | 4 | v2 session flags; reserved and zero in v1 |
| 20 | 4 | reserved |
| 24 | variable | address bytes, without a terminator |

Session flag bit 0 (`SessionBeginFlagDeferredSymbolExpansion`) means the live
recorder used ProtocolOnly's deferred optional symbol-expansion mode from the
start of the session. Replay must select that mode and the recorder drain
completion predicate before processing any client frames. This makes an
in-progress revision deterministic even though its v4 `BeginDrain` record does
not exist yet. Unknown v2 flag bits are rejected. A payload-level version bump
does not change the journal file format, which remains version 1.0.

`Checkpoint` has a 24-byte payload: version and size at offsets 0 and 2,
reserved bytes 4–7, cumulative client bytes at offset 8, and cumulative server
bytes at offset 16.

`SessionEnd` has a 32-byte payload: version and size at offsets 0 and 2, a
`ProtocolCloseReason` value at offset 4, cumulative client and server bytes at
offsets 8 and 16, and eight reserved bytes. Version 1 reason values distinguish
clean capture completion, local shutdown, peer disconnect, handshake failures,
memory limit, instrumentation failure, recorder failure, and transport error.

## Visibility and durability

The writer reports three monotonically increasing offsets:

- **Committed**: complete records have been passed to the file sink.
- **Published**: buffered I/O has been flushed, so another process can observe
  the committed prefix.
- **Durable**: the published prefix has crossed an OS durability barrier
  (`FlushFileBuffers` on Windows, `fsync` on POSIX).

Live readers may temporarily see a partial tail and must wait for more bytes.
Recovery after termination accepts the longest valid prefix present on disk.
Truncating an invalid tail is an explicit operation, never an implicit read.
The repair path verifies the observed file size again through the writable
handle and durably flushes the truncation before reporting success.

## Reliability semantics

- A disk error or zero-progress short write poisons the writer and ends capture
  with an explicit failure.
- Ordinary short writes are retried until the requested buffer is complete.
- Backpressure is bounded: the recorder stops reading the socket while disk
  buffers are unavailable, allowing TCP flow control to reach the producer.
- A clean end appends `SessionEnd` and performs a durable flush.
- A missing `SessionEnd` is an incomplete but recoverable session, not a corrupt
  one.
- Strict local replay imposes a 10-second deadline while waiting for each
  recorded server chunk. A missing response or divergent completion state
  fails explicitly instead of leaving conversion or query loading blocked
  forever.
- Immutable live views fingerprint their exact committed prefix and revalidate
  it before and after reads; a partial tail does not advance the revision.
- Version 1 makes no guarantee for data that the producer had not yet delivered
  over TCP when either side failed.
