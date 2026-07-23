# Tracy Query completeness contract

`tracy-query` distinguishes four independent forms of completeness:

1. **Domain completeness** means a persisted Tracy domain has at least one
   stable DTO and query route.
2. **Field completeness** means every persisted semantic field in that domain
   is addressable without discarding its original value or relationship.
3. **MCP completeness** means every public Query method is reachable through
   the MCP transport.
4. **Producer completeness** means the profiled application states which
   instrumentation was enabled, disabled, or unsupported when the capture was
   recorded.

The original `coverage-v1.json` manifest records domain completeness. It does
not prove field or MCP completeness. `coverage-fields-v1.json` is the
field-level ledger and is audited against `Worker::Write`, the corresponding
load paths, `TracyEvent.hpp`, Worker getters, and Profiler consumers.

The saved-trace field and MCP gates are currently `complete`. Producer
completeness remains a separate gate and is not implied by either status.

## What "all fields" means

A field is mandatory when it is persisted in a supported `.tracy` version and
has observable performance, identity, relationship, source, symbol, or
diagnostic meaning.

The following are not protocol data:

- mutexes, condition variables, atomics, and thread objects;
- native pointers and allocator/container layout;
- lookup caches, reverse maps, staging buffers, and ready flags;
- GUI navigation, selection, playback, and drawing state;
- live-connection state that is not saved into the capture.

Every exclusion remains listed in the field ledger with a reason. A field
cannot disappear merely because it is inconvenient or expensive to serialize.

## Lossless and normalized representations

Query entities may expose both:

- `raw`: the persisted code, identifier, index, byte sequence, or bit pattern;
- `normalized`: a stable name, opaque ref, resolved relationship, or
  AI-friendly interpretation.

Enums retain their numeric code alongside a stable name. Native identifiers
retain their exact integer representation alongside an opaque trace-scoped
reference. Invalid UTF-8 has a repaired display form, a warning, and an
addressable raw byte representation when the original bytes are available.

Large source files, symbol code, and frame images remain addressable through
bounded resources and chunk/range reads. Response budgets limit one response;
they must not make the underlying capture permanently inaccessible.

## Version and absence semantics

Supported saved captures are Tracy 0.9.0 through 0.13.1. A field introduced by
a newer supported format returns an explicit `available=false` result with a
version reason when an older capture is queried. It must not be fabricated or
represented as an ambiguous empty value.

## Completion gate

Field completeness may be marked `complete` only when:

- the field ledger contains no unexplained `unmapped` persisted field;
- every mandatory field has a DTO/Query path and a deterministic test;
- raw-to-normalized conversion preserves the original value;
- list pagination can exhaust the entity set without duplicates or omissions;
- binary/text resources can be retrieved completely through bounded chunks;
- old-version absence and unavailable Producer capabilities are explicit.

MCP completeness has a separate gate: every public method in the Query method
registry must be callable through MCP, either by a workflow tool or by the
generic read-only Query tool.
