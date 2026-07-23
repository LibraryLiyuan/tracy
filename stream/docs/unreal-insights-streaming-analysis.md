# Unreal Insights streaming recorder and store source analysis

## Audited baseline

The local source tree audited for this design is:

```text
C:\CodeProjects\UnrealEngineProjects\UnrealEngine
UE 5.7.2
branch: release
commit: dbd4567bb144
```

The tracked worktree was clean when inspected. No Unreal Engine files were
changed.

This is a source-level audit, not an inference from the Unreal Insights UI. The
important distinction is that Unreal Trace is a one-way, self-describing event
stream, while Tracy 0.13.1 is bidirectional and asks the client for definitions
and frame images while recording.

## End-to-end path

```text
instrumented UE thread
  -> thread-local FWriteBuffer
  -> bounded block pool
  -> Trace worker drains and packetizes
  -> synchronous socket/file write
  -> UnrealTraceServer FRecorderRelay
  -> two 256 KiB asynchronous relay buffers
  -> append-only .utrace
  -> Store file reader
  -> TraceRelay (temporary EOF polling while session is live)
  -> TraceAnalysis stream buffer and incremental parser
```

### 1. Producer-side buffers and backpressure

Relevant sources:

```text
Engine\Source\Runtime\TraceLog\Private\Trace\TlsBuffer.cpp
Engine\Source\Runtime\TraceLog\Private\Trace\BlockPool.cpp
Engine\Source\Runtime\TraceLog\Private\Trace\Writer.cpp
Engine\Source\Runtime\TraceLog\Public\Trace\Config.h
Engine\Source\Runtime\TraceLog\Public\Trace\Trace.h
```

Each instrumented thread writes into a thread-local `FWriteBuffer`.
`Writer_DrainBuffers()` collects committed bytes and returns retired blocks to
the pool. The default pool limit is 79 MiB
(`UE_TRACE_BLOCK_POOL_MAXSIZE`, `Config.h:120-122`).

When the free list is empty and the pool has reached that limit,
`Writer_AllocateBlockFromPool()` does not discard the event. It loops, yields or
throttles the emitting thread, and waits for the trace worker to return a block
(`BlockPool.cpp:116-215`). This is producer-side backpressure. It can perturb
the instrumented application under a sufficiently slow sink, but it preserves
the event instead of silently dropping it.

The worker uses a 1 MiB send buffer when `TRACE_PRIVATE_BUFFER_SEND` is enabled.
`Writer_FlushSendBuffer()` calls the configured `IoWrite` synchronously and
closes the data handle on failure (`Writer.cpp:332-395`). On Windows,
`IoWrite()` is one blocking `WriteFile` and requires the complete byte count
(`Detail\Windows\WindowsTrace.cpp:234-245`).

### 2. TraceServer recorder and slow-disk behavior

Relevant sources:

```text
Engine\Source\Programs\UnrealTraceServer\src\Recorder.cpp
Engine\Source\Programs\UnrealTraceServer\src\AsioFile.cpp
Engine\Source\Programs\UnrealTraceServer\src\AsioIoable.cpp
Engine\Source\Programs\UnrealTraceServer\src\Store.cpp
```

`FRecorderRelay` validates the TRC2 preamble, creates the destination
`.utrace`, and relays the exact stream. It allocates exactly two 256 KiB buffers
(`Recorder.cpp:32-44, 64-81`).

The core state machine is at `Recorder.cpp:258-309`:

1. one buffer is submitted to the file;
2. the other buffer is submitted for a socket read;
3. no next pair is issued until both the fill and drain complete;
4. the buffers swap roles.

This permits one read and one write to overlap, but bounds relay storage to
512 KiB plus transport/OS buffers. If the file write stalls, the completion
barrier prevents further socket reads. TCP receive-window pressure then reaches
the producer, whose own block-pool limit eventually stalls event emitters.

The file is opened with `CREATE_ALWAYS`, `FILE_SHARE_READ`, and overlapped I/O.
Writes use `asio::async_write_at` and advance a monotonically increasing file
offset only on completion (`AsioFile.cpp:56-80, 114-131`). Any asynchronous
read/write error closes both sides of the relay.

### 3. LIVE store and incremental analysis

Relevant sources:

```text
Engine\Source\Programs\UnrealTraceServer\src\Store.cpp
Engine\Source\Programs\UnrealTraceServer\src\StoreCborServer.cpp
Engine\Source\Programs\UnrealTraceServer\src\TraceRelay.cpp
Engine\Source\Developer\TraceAnalysis\Private\DataStream.cpp
Engine\Source\Developer\TraceAnalysis\Private\Analysis\Processor.cpp
Engine\Source\Developer\TraceAnalysis\Private\Analysis\StreamReader.cpp
```

The store registers a new trace immediately and reports its current file size.
A reader opens the file with shared read/write access. `FTraceRelay` forwards
the growing file to an analysis client over a local TCP connection.

Reaching the current file end is not treated as final while the corresponding
recorder session still exists. `TraceRelay.cpp:89-108` waits 200 ms and retries.
Once the recorder session disappears, EOF closes the relay.

The analysis client blocks on the relay socket, retrying a one-second `select`
timeout (`DataStream.cpp:107-145`). `FStreamBuffer` preserves bytes that do not
yet satisfy a parser demand, consolidates them with the next read, and grows
only when an individual parser demand requires it (`StreamReader.cpp:78-130`,
`StreamReader.h:116-129`). This is how LIVE analysis tolerates a transport
packet split across file writes.

The LIVE safety boundary is therefore protocol parseability plus the current
file end. UnrealTraceServer does not add a separate transaction envelope around
each `.utrace` packet.

## What Unreal does not guarantee

The audited implementation does not provide the following as default
end-to-end guarantees:

- no application ACK from TraceServer to the producer;
- no retransmission protocol or reconnect continuation;
- no producer WAL for exactly-once delivery;
- no record CRC or commit trailer added by TraceServer;
- no `FlushFileBuffers`, `fsync`, write-through open, or periodic durable
  checkpoint in the recorder path;
- no recovery scanner that distinguishes a committed prefix from arbitrary
  bytes after a crash.

`UE_TRACE_PACKET_VERIFICATION` can append a packet serial and detect a gap, but
it defaults to `0` (`Trace\Config.h:142-143`). Detection is not acknowledgment
or recovery. Sync packets help the analyzer distinguish startup holes from a
need for more data (`Writer.cpp:635-655`), but they also do not retransmit.

An ordinary OS/process failure can therefore leave an incomplete `.utrace`
tail. The incremental parser naturally waits for missing bytes, but the file
format does not carry an explicit durable-commit boundary.

## Important Events Cache and tail tracing

Relevant sources:

```text
Engine\Source\Runtime\TraceLog\Private\Trace\Important\Cache.cpp
Engine\Source\Runtime\TraceLog\Private\Trace\Tail.cpp
Engine\Source\Runtime\TraceLog\Private\Trace\Writer.cpp
```

On connection, the producer sends event descriptions, cached Important events,
connection callback state, the tail buffer, and sync packets
(`Writer.cpp:713-746`).

The Important Events Cache preserves state needed by a late subscriber. It is
not a complete history WAL. The tail buffer defaults to 4 MiB and is explicitly
a packet ring: it removes the oldest packets until a new packet fits
(`Tail.cpp:168-216`). A single packet larger than the ring resets the retained
history before that packet is sent (`Tail.cpp:225-241`).

Consequently, late connect means “definitions/important state plus recent
tail,” not “all events since process start.”

## Decisions transferred to Tracy

| Concern | Unreal behavior | Tracy streaming v1 decision |
| --- | --- | --- |
| Slow disk | two fixed relay buffers stop further socket reads | synchronous write-ahead journal initially; later two bounded async buffers |
| Producer pressure | bounded pool stalls emitting threads | rely on TCP pressure for v1; never silently discard |
| Wire direction | one-way self-describing stream | persist both Tracy directions in causal order |
| LIVE visibility | current file size plus incremental parser | expose only CRC-validated records with complete commit trailers |
| Crash tail | incomplete parser demand/EOF | scan longest valid prefix; explicit repair only |
| Durability | no explicit recorder barrier | durable header, periodic checkpoints, durable SessionEnd |
| Sequence | optional debug packet serial | mandatory journal record sequence |
| ACK/retransmit | absent | explicitly outside v1; reserve session/sequence fields |
| Late connect | Important cache plus bounded tail | not claimed as complete history |

The main Tracy-specific constraint is server queries. A raw one-way
socket-to-file relay cannot reproduce a Tracy session because strings, source
locations, call stacks, symbols, and frame images may be requested by the
server. The compatibility recorder therefore keeps the existing `Worker` as
the protocol authority and records every exact incoming and outgoing byte.
Offline replay validates the outgoing query stream before producing a normal
`.tracy` snapshot.

## Consequences for later milestones

The current compatibility recorder is correct as a transcript but still uses
`Worker` and therefore still retains the analysis timeline. Removing
duration-dependent memory requires a Tracy-specific protocol definition
resolver, not just copying Unreal's relay.

The next recorder milestone should:

1. split definition/query state from timeline allocation;
2. retain only bounded pending-query and decompression state;
3. add a two-buffer asynchronous sink with a strict configured queue cap;
4. keep write-ahead ordering before a query is sent or an incoming frame is
   processed;
5. add slow-disk and disk-full injection plus a long-duration RSS plateau test.

ACK/WAL/reconnect should remain a separate protocol version because it requires
producer changes and cannot be implemented honestly only on the server.
