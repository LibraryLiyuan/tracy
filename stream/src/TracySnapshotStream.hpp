#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>

namespace tracy::stream
{
struct SnapshotStreamOptions
{
    uint8_t streams = 4;
    uint64_t maxFileBytes = 64ull << 30;
    uint64_t maxDecodedBytes = UINT64_MAX;
    std::function<bool()> cancelled;
};
struct SnapshotStreamStats
{
    uint64_t decodedBytes = 0, fileBytes = 0;
    std::array<uint8_t, 32> sha256{};
};
 // Standard Tracy compression envelope. Memory is bounded by stream count,
 // independent of file length. Callers own structure validation and publication.
class SnapshotStreamWriter
{
public:
    SnapshotStreamWriter( const std::filesystem::path& newTemporaryFile, const SnapshotStreamOptions& options = {} );
    ~SnapshotStreamWriter();
    void Write( std::span<const uint8_t> bytes );
    SnapshotStreamStats Finish();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
class SnapshotStreamReader
{
public:
    explicit SnapshotStreamReader( const std::filesystem::path& file, const SnapshotStreamOptions& options = {} );
    ~SnapshotStreamReader();
     // Returns zero only at a block boundary at EOF. Corrupt/truncated envelopes
     // throw; they never masquerade as successful short reads.
    size_t Read( std::span<uint8_t> destination );
    void ReadExact( std::span<uint8_t> destination );
    SnapshotStreamStats Finish();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
