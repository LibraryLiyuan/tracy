#include "TracyAnalysisCacheTable.hpp"
#include "TracyAnalysisIoPath.hpp"
#include "TracyHash.hpp"
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace tracy::analysis
{
namespace
{
using nlohmann::json;
constexpr uint32_t Schema = 2;
constexpr uint64_t MaxBlockBytes = 64 * 1024 * 1024;
constexpr uint64_t MaxManifestBytes = 16 * 1024 * 1024;
struct IndexHeader
{
    char magic[8] = {'J','N','A','C','I','D','X','2'};
    uint32_t schema = Schema;
    uint32_t reserved = 0;
    uint64_t records = 0;
};
struct IndexEntry
{
    uint64_t block = 0;
    uint32_t offset = 0;
    uint32_t keyBytes = 0;
    uint32_t payloadBytes = 0;
    uint32_t reserved = 0;
};
struct BlockHeader
{
    char magic[8] = {'J','N','A','C','B','L','K','1'};
    uint64_t ordinal = 0;
    uint64_t first = 0;
    uint64_t records = 0;
    uint64_t bytes = 0;
};
struct BlockEncoding
{
    uint32_t version = 2, codec = 0;
    uint64_t storedBytes = 0;
};
static_assert(sizeof(BlockEncoding)==16);
struct BlockInfo
{
    uint64_t ordinal = 0, first = 0, records = 0, bytes = 0;
    std::string sha256;
    uint64_t storedBytes = 0;
    uint32_t codec = 0;
};
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(IndexHeader) == 24 && sizeof(IndexEntry) == 24 && sizeof(BlockHeader) == 40);

[[noreturn]] void Fail(const char* reason) { throw std::runtime_error(std::string("analysis_cache_") + reason); }
uint64_t ExactUnsigned(const json& value,const char* error)
{
    if(!value.is_number_unsigned() && !(value.is_number_integer() && value.get<int64_t>()>=0)) Fail(error);
    return value.get<uint64_t>();
}
uint32_t ExactU32(const json& value,const char* error)
{
    const auto result=ExactUnsigned(value,error);
    if(result>UINT32_MAX) Fail(error);
    return uint32_t(result);
}
void CheckCancel(const AnalysisCacheTableOptions& options)
{
    if(options.cancelled && options.cancelled()) Fail("cancelled");
}
bool Digest(std::string_view value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
void ValidateOptions(const AnalysisCacheTableOptions& options)
{
    if(options.blockBytes < 64 || options.blockBytes > MaxBlockBytes ||
        options.manifestBytes < 1024 || options.manifestBytes > MaxManifestBytes) Fail("invalid_budget");
}
std::string Hash(std::string_view value)
{
    Sha256Builder hash; hash.Update(value.data(), value.size()); return hash.FinalHex();
}
std::string BlockName(uint64_t ordinal)
{
    std::ostringstream value;
    value << std::hex << std::setw(16) << std::setfill('0') << ordinal << ".bin";
    return value.str();
}
void CheckContained(const std::filesystem::path& root, const std::filesystem::path& path)
{
    const auto relative = std::filesystem::weakly_canonical(path).lexically_relative(std::filesystem::weakly_canonical(root));
    if(relative.empty() || relative.is_absolute()) Fail("path_escape");
    for(const auto& part : relative) if(part == "..") Fail("path_escape");
}
void DurableRename(const std::filesystem::path& temporary, const std::filesystem::path& target)
{
    if(std::filesystem::exists(target)) Fail("already_exists");
#ifdef _WIN32
    const auto source = AnalysisIoPath(temporary);
    const auto destination = AnalysisIoPath(target);
    HANDLE file = CreateFileW(source.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(file == INVALID_HANDLE_VALUE) Fail("flush_open_failed");
    const bool flushed = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if(!flushed) Fail("flush_failed");
    if(!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) Fail("rename_failed");
#else
    int file = open(temporary.c_str(), O_RDONLY);
    if(file < 0) Fail("flush_open_failed");
    const auto flushed = fsync(file); close(file);
    if(flushed != 0) Fail("flush_failed");
    std::filesystem::rename(temporary, target);
    file = open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if(file < 0) Fail("directory_flush_open_failed");
    const auto directoryFlushed = fsync(file); close(file);
    if(directoryFlushed != 0) Fail("directory_flush_failed");
#endif
}
template<class T> void Write(std::ostream& out, const T& value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
    if(!out) Fail("write_failed");
}
template<class T> T Read(std::istream& input)
{
    T value;
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if(!input) Fail("truncated");
    return value;
}
std::string ReadText(const std::filesystem::path& path, uint64_t limit)
{
    const auto size = std::filesystem::file_size(path);
    if(size > limit) Fail("manifest_budget");
    std::ifstream input(AnalysisIoPath(path), std::ios::binary);
    std::string value(size_t(size), '\0');
    input.read(value.data(), std::streamsize(value.size()));
    if(!input) Fail("manifest_read_failed");
    return value;
}
void WriteText(const std::filesystem::path& path, std::string_view value)
{
    std::ofstream output(AnalysisIoPath(path), std::ios::binary | std::ios::trunc);
    output.write(value.data(), std::streamsize(value.size())); output.flush();
    if(!output) Fail("manifest_write_failed");
}
}

namespace {
std::string StorageExperiment()
{
#ifdef TRACY_CACHE_STORAGE_EXPERIMENTS
    char value[32]{};
#ifdef _WIN32
    const auto size=GetEnvironmentVariableA("JNTRACY_CACHE_EXPERIMENT",value,sizeof(value));
    if(size>=sizeof(value)) Fail("experiment_mode");
#else
    const auto* environment=std::getenv("JNTRACY_CACHE_EXPERIMENT");
    if(environment) {
        if(std::strlen(environment)>=sizeof(value)) Fail("experiment_mode");
        std::strcpy(value,environment);
    }
#endif
    const std::string mode=value;
    if(!mode.empty() && mode!="cleanup" && mode!="compression" && mode!="combined") Fail("experiment_mode");
    return mode;
#else
    return {};
#endif
}
}
bool AnalysisCacheCompressionDefault() { return StorageExperiment()!="cleanup"; }
bool AnalysisCacheCleanupDefault() { return StorageExperiment()!="compression"; }

struct AnalysisCacheTableWriter::Impl
{
    std::filesystem::path root;
    std::string identity, kind, previous;
    AnalysisCacheTableOptions options;
    AnalysisWorkspaceReservation memory;
    std::ofstream index;
    std::vector<char> buffer;
    std::vector<BlockInfo> blocks;
    Sha256Builder content;
    uint64_t records = 0, blockRecords = 0, peak = 0;
    bool failed = false, committed = false;

    Impl(std::filesystem::path path, std::string id, std::string type, AnalysisCacheTableOptions config)
        : root(std::move(path)), identity(std::move(id)), kind(std::move(type)), options(std::move(config)),
          memory(options.workspace)
    {
        ValidateOptions(options);
        if(!Digest(identity) || kind.empty() || kind.size() > 128) Fail("invalid_identity_or_kind");
        CheckCancel(options);
        // Block capacity, the maximum retained previous key, and stream/hash
        // overhead share the caller's workspace, before any buffer is grown.
        memory.Resize(options.blockBytes+128*1024);
        std::filesystem::create_directories(root.parent_path());
        if(!std::filesystem::create_directory(root)) Fail("already_exists");
        std::filesystem::create_directory(root / "blocks");
        buffer.reserve(size_t(options.blockBytes));
        AnalysisDiskGrow(options.disk,sizeof(IndexHeader));
        index.open(AnalysisIoPath(root / "index.tmp"), std::ios::binary | std::ios::trunc);
        Write(index, IndexHeader{});
    }
    void Check()
    {
        if(failed) Fail("writer_failed");
        if(committed) Fail("writer_closed");
        CheckCancel(options);
    }
    void Flush()
    {
        if(buffer.empty()) return;
        Check();
        // Directory metadata is bounded too; very small block configurations
        // must not create an unbounded in-memory manifest.
        if(blocks.size() >= options.manifestBytes / 512) Fail("manifest_budget");
        memory.Add(1024); // block metadata, including vector capacity headroom
        BlockHeader header;
        header.ordinal = blocks.size(); header.first = records - blockRecords;
        header.records = blockRecords; header.bytes = buffer.size();
        const auto target = root / "blocks" / BlockName(header.ordinal);
        const auto temporary = std::filesystem::path(target.native() + std::filesystem::path(".tmp").native());
        CheckContained(root, target);
        header.magic[7]='2';
        BlockEncoding encoding;
        const char* stored=buffer.data();
        size_t storedBytes=buffer.size();
        // The raw block is already reserved. Charge output and an aligned static
        // codec arena before allocation; codec code cannot allocate off-budget.
        AnalysisWorkspaceReservation codecMemory(options.workspace);
        std::vector<char> compressed;
        std::vector<uint64_t> arena;
        if(options.compressBlocks) {
            const auto bound=ZSTD_compressBound(buffer.size());
            const auto arenaBytes=ZSTD_estimateCCtxSize(1);
            if(ZSTD_isError(bound) || ZSTD_isError(arenaBytes)) Fail("codec_budget");
            codecMemory.Resize(bound+arenaBytes+8);
            compressed.resize(bound); arena.resize((arenaBytes+7)/8);
            auto* context=ZSTD_initStaticCCtx(arena.data(),arena.size()*8);
            if(!context) Fail("codec_context");
            const auto size=ZSTD_compressCCtx(context,compressed.data(),compressed.size(),buffer.data(),buffer.size(),1);
            if(ZSTD_isError(size)) Fail("codec_compress");
            if(size<buffer.size()) { encoding.codec=1; stored=compressed.data(); storedBytes=size; }
        }
        CheckCancel(options);
        encoding.storedBytes=storedBytes;
        AnalysisDiskGrow(options.disk,sizeof(header)+sizeof(encoding)+storedBytes);
        std::ofstream output(AnalysisIoPath(temporary), std::ios::binary | std::ios::trunc);
        Write(output, header); Write(output,encoding);
        output.write(stored, std::streamsize(storedBytes)); output.flush();
        if(!output) Fail("block_write_failed");
        output.close();
        Sha256Builder hash;
        hash.Update(&header, sizeof(header)); hash.Update(&encoding,sizeof(encoding)); hash.Update(stored,storedBytes);
        const auto digest = hash.FinalHex();
        if(Sha256File(temporary) != digest) Fail("block_write_verification");
        DurableRename(temporary, target);
        blocks.push_back({header.ordinal, header.first, header.records, header.bytes, digest,storedBytes,encoding.codec});
        buffer.clear(); blockRecords = 0;
    }
};

AnalysisCacheTableWriter::AnalysisCacheTableWriter(std::filesystem::path root, std::string identity,
    std::string kind, AnalysisCacheTableOptions options)
    : m_impl(std::make_unique<Impl>(std::move(root), std::move(identity), std::move(kind), std::move(options))) {}
AnalysisCacheTableWriter::~AnalysisCacheTableWriter() = default;
uint64_t AnalysisCacheTableWriter::PeakBufferedBytes() const { return m_impl->peak; }

void AnalysisCacheTableWriter::Append(std::string_view key, std::string_view payload)
{
    auto& w = *m_impl;
    try
    {
        w.Check();
        if(key.empty() || (w.records && key <= w.previous)) Fail("key_order");
        const uint64_t size = 8 + uint64_t(key.size()) + uint64_t(payload.size());
        if(key.size() > 65536 || size > w.options.blockBytes) Fail("record_budget");
        if(w.buffer.size() + size > w.options.blockBytes) w.Flush();
        const IndexEntry position {uint64_t(w.blocks.size()), uint32_t(w.buffer.size()), uint32_t(key.size()), uint32_t(payload.size()), 0};
        const auto append = [&](const void* bytes, size_t count) {
            const auto first = static_cast<const char*>(bytes);
            if(count) w.buffer.insert(w.buffer.end(), first, first + count);
            w.content.Update(bytes, count);
        };
        append(&position.keyBytes, 4); append(&position.payloadBytes, 4);
        append(key.data(), key.size()); append(payload.data(), payload.size());
        AnalysisDiskGrow(w.options.disk,sizeof(position));
        Write(w.index, position);
        ++w.records; ++w.blockRecords; w.previous = key;
        w.peak = std::max<uint64_t>(w.peak, w.buffer.size());
    }
    catch(...) { w.failed = true; throw; }
}

AnalysisCacheTableDescriptor AnalysisCacheTableWriter::Commit()
{
    auto& w = *m_impl;
    try
    {
        w.Check(); w.Flush();
        IndexHeader header; header.records = w.records;
        w.index.seekp(0); Write(w.index, header); w.index.flush();
        if(!w.index) Fail("index_write_failed");
        w.index.close();
        const auto indexHash = Sha256File(w.root / "index.tmp");
        const auto indexBytes = std::filesystem::file_size(w.root / "index.tmp");
        if(w.records > (UINT64_MAX-sizeof(header))/sizeof(IndexEntry) ||
            indexBytes != sizeof(header)+w.records*sizeof(IndexEntry)) Fail("index_size");
        DurableRename(w.root / "index.tmp", w.root / "index.bin");
        const auto contentHash = w.content.FinalHex();
        AnalysisWorkspaceReservation manifestMemory(w.options.workspace,128*1024+w.blocks.size()*4096);
        json body = { {"schema", Schema}, {"identity", w.identity}, {"kind", w.kind},
            {"record_count", w.records}, {"block_bytes", w.options.blockBytes},
            {"content_sha256", contentHash}, {"index_bytes", indexBytes},
            {"index_sha256", indexHash}, {"blocks", json::array()} };
        for(const auto& block : w.blocks) body["blocks"].push_back({
            {"ordinal", block.ordinal}, {"first", block.first}, {"records", block.records},
            {"bytes", block.bytes}, {"sha256", block.sha256}, {"stored_bytes",block.storedBytes}, {"codec",block.codec} });
        const auto manifest = json({{"body", body}, {"sha256", Hash(body.dump())}}).dump();
        if(manifest.size() > w.options.manifestBytes) Fail("manifest_budget");
        w.Check();
        AnalysisDiskGrow(w.options.disk,manifest.size());
        WriteText(w.root / "manifest.tmp", manifest);
        DurableRename(w.root / "manifest.tmp", w.root / "manifest.json");
        w.committed = true;
        return {w.records, uint64_t(w.blocks.size()), contentHash};
    }
    catch(...) { w.failed = true; throw; }
}

struct AnalysisCacheTableReader::Impl
{
    std::filesystem::path root;
    AnalysisCacheTableOptions options;
    AnalysisWorkspaceReservation metadataMemory,blockMemory;
    AnalysisCacheTableDescriptor descriptor;
    std::vector<BlockInfo> blocks;
    std::ifstream index;
    std::vector<char> buffer;
    uint32_t schema = 0;
    uint64_t loaded = UINT64_MAX;
    AnalysisCacheReadMetrics metrics;
    Impl(std::filesystem::path path, const std::string& identity, const std::string& kind, AnalysisCacheTableOptions config)
        : root(std::move(path)), options(std::move(config)),
          metadataMemory(options.workspace),blockMemory(options.workspace)
    {
        ValidateOptions(options); CheckCancel(options);
        const auto manifestPath = root / "manifest.json";
        if(!std::filesystem::exists(manifestPath)) Fail("not_complete");
        CheckContained(root, manifestPath);
        const auto manifestBytes=std::filesystem::file_size(manifestPath);
        if(manifestBytes>options.manifestBytes) Fail("manifest_budget");
        metadataMemory.Resize(64*1024);
        AnalysisWorkspaceReservation parseMemory(options.workspace,64*1024+manifestBytes*16);
        const auto envelope = json::parse(ReadText(manifestPath, options.manifestBytes));
        const auto& body = envelope.at("body");
        if(Hash(body.dump()) != envelope.at("sha256").get<std::string>()) Fail("manifest_checksum");
        schema=ExactU32(body.at("schema"),"schema_mismatch");
        if(schema!=1 && schema!=Schema) Fail("schema_mismatch");
        if(body.at("identity").get<std::string>() != identity || !Digest(identity)) Fail("identity_mismatch");
        if(body.at("kind").get<std::string>() != kind) Fail("kind_mismatch");
        if(body.at("block_bytes").get<uint64_t>() > options.blockBytes) Fail("block_budget");
        descriptor.records = body.at("record_count").get<uint64_t>();
        descriptor.contentSha256 = body.at("content_sha256").get<std::string>();
        if(!Digest(descriptor.contentSha256)) Fail("content_identity");
        if(!body.at("blocks").is_array() || body.at("blocks").size() > options.manifestBytes/512) Fail("manifest_budget");
        uint64_t end = 0;
        for(const auto& value : body.at("blocks"))
        {
            BlockInfo block {value.at("ordinal").get<uint64_t>(), value.at("first").get<uint64_t>(),
                value.at("records").get<uint64_t>(), value.at("bytes").get<uint64_t>(), value.at("sha256").get<std::string>()};
            if(block.ordinal != blocks.size() || block.first != end || block.records == 0 ||
                block.records > descriptor.records-end || block.bytes > options.blockBytes ||
                block.records > block.bytes/9 || !Digest(block.sha256)) Fail("block_directory");
            block.storedBytes=schema==1 ? block.bytes : ExactUnsigned(value.at("stored_bytes"),"block_encoding");
            block.codec=schema==1 ? 0 : ExactU32(value.at("codec"),"block_encoding");
            if(block.codec>1 || block.storedBytes==0 || block.storedBytes>block.bytes ||
                (block.codec==0 && block.storedBytes!=block.bytes)) Fail("block_encoding");
            metadataMemory.Add(512);
            end += block.records; blocks.push_back(std::move(block));
        }
        if(end != descriptor.records) Fail("record_count");
        descriptor.blocks = blocks.size();
        const auto indexPath = root / "index.bin";
        CheckContained(root, indexPath);
        if(descriptor.records > (UINT64_MAX-sizeof(IndexHeader))/sizeof(IndexEntry)) Fail("index_size");
        const auto expectedBytes = sizeof(IndexHeader)+descriptor.records*sizeof(IndexEntry);
        if(body.at("index_bytes").get<uint64_t>() != expectedBytes || std::filesystem::file_size(indexPath) != expectedBytes) Fail("index_size");
        if(Sha256File(indexPath) != body.at("index_sha256").get<std::string>()) Fail("index_checksum");
        index.open(AnalysisIoPath(indexPath), std::ios::binary);
        const auto header = Read<IndexHeader>(index);
        IndexHeader expected; expected.magic[7]=char('0'+schema);
        if(std::memcmp(header.magic, expected.magic, 8) || header.schema != schema || header.reserved || header.records != descriptor.records) Fail("index_header");
    }
    IndexEntry Position(uint64_t ordinal)
    {
        CheckCancel(options);
        if(ordinal >= descriptor.records) Fail("ordinal_out_of_range");
        index.clear(); index.seekg(std::streamoff(sizeof(IndexHeader)+ordinal*sizeof(IndexEntry)));
        const auto entry = Read<IndexEntry>(index);
        if(entry.block >= blocks.size() || entry.reserved) Fail("index_entry");
        const auto& block = blocks[size_t(entry.block)];
        if(ordinal < block.first || ordinal-block.first >= block.records || entry.keyBytes == 0 || entry.keyBytes > 65536 ||
            uint64_t(entry.offset)+8+entry.keyBytes+entry.payloadBytes > block.bytes) Fail("index_entry");
        return entry;
    }
    void Load(uint64_t blockOrdinal)
    {
        if(loaded == blockOrdinal) return;
        CheckCancel(options); loaded = UINT64_MAX;
        const auto& block = blocks[size_t(blockOrdinal)];
        const auto path = root / "blocks" / BlockName(blockOrdinal);
        CheckContained(root, path);
        if(!std::filesystem::exists(path)) Fail("block_missing");
        const auto extensionBytes=schema==1 ? 0 : sizeof(BlockEncoding);
        if(std::filesystem::file_size(path) != sizeof(BlockHeader)+extensionBytes+block.storedBytes) Fail("block_size");
        std::ifstream input(AnalysisIoPath(path), std::ios::binary);
        const auto header = Read<BlockHeader>(input);
        BlockHeader expected; expected.magic[7]=char('0'+schema);
        if(std::memcmp(header.magic, expected.magic, 8) || header.ordinal != block.ordinal ||
            header.first != block.first || header.records != block.records || header.bytes != block.bytes) Fail("block_header");
        BlockEncoding encoding;
        if(schema==2) {
            encoding=Read<BlockEncoding>(input);
            if(encoding.version!=2 || encoding.codec!=block.codec || encoding.storedBytes!=block.storedBytes) Fail("block_encoding");
        }
        if(buffer.capacity() < block.bytes) {
            blockMemory.Resize(2*options.blockBytes);
            buffer.reserve(size_t(options.blockBytes));
        }
        buffer.resize(size_t(block.bytes));
        AnalysisWorkspaceReservation codecMemory(options.workspace);
        std::vector<char> compressed;
        std::vector<uint64_t> arena;
        char* stored=buffer.data();
        if(block.codec==1) {
            const auto arenaBytes=ZSTD_estimateDCtxSize();
            if(ZSTD_isError(arenaBytes)) Fail("codec_budget");
            codecMemory.Resize(block.storedBytes+arenaBytes+8);
            compressed.resize(size_t(block.storedBytes)); arena.resize((arenaBytes+7)/8);
            stored=compressed.data();
        }
        input.read(stored, std::streamsize(block.storedBytes));
        if(!input) Fail("block_truncated");
        Sha256Builder hash; hash.Update(&header, sizeof(header));
        if(schema==2) hash.Update(&encoding,sizeof(encoding));
        hash.Update(stored,size_t(block.storedBytes));
        if(hash.FinalHex() != block.sha256) Fail("block_checksum");
        if(block.codec==1) {
            if(ZSTD_getFrameContentSize(stored,size_t(block.storedBytes))!=block.bytes ||
                ZSTD_findFrameCompressedSize(stored,size_t(block.storedBytes))!=block.storedBytes) Fail("codec_frame_size");
            auto* context=ZSTD_initStaticDCtx(arena.data(),arena.size()*8);
            if(!context) Fail("codec_context");
            const auto size=ZSTD_decompressDCtx(context,buffer.data(),buffer.size(),stored,size_t(block.storedBytes));
            if(ZSTD_isError(size) || size!=block.bytes) Fail("codec_decode");
        }
        CheckCancel(options);
        ++metrics.blocksLoaded; metrics.payloadBytesRead += block.bytes;
        metrics.storedBytesRead += sizeof(header)+extensionBytes+block.storedBytes;
        metrics.decodedBytes += block.bytes;
        metrics.peakBlockBytes = std::max<uint64_t>(metrics.peakBlockBytes, buffer.size());
        loaded = blockOrdinal;
    }
    std::pair<std::string_view,std::string_view> View(uint64_t ordinal)
    {
        const auto position = Position(ordinal);
        Load(position.block);
        uint32_t keyBytes, payloadBytes;
        const auto* record = buffer.data()+position.offset;
        std::memcpy(&keyBytes, record, 4); std::memcpy(&payloadBytes, record+4, 4);
        if(keyBytes != position.keyBytes || payloadBytes != position.payloadBytes) Fail("record_index_mismatch");
        return {{record+8, keyBytes}, {record+8+keyBytes, payloadBytes}};
    }
};

AnalysisCacheTableReader::AnalysisCacheTableReader(std::filesystem::path root, std::string identity,
    std::string kind, AnalysisCacheTableOptions options)
    : m_impl(std::make_unique<Impl>(std::move(root), identity, kind, std::move(options))) {}
AnalysisCacheTableReader::~AnalysisCacheTableReader() = default;
uint64_t AnalysisCacheTableReader::RecordCount() const { return m_impl->descriptor.records; }
const AnalysisCacheTableDescriptor& AnalysisCacheTableReader::Descriptor() const { return m_impl->descriptor; }
AnalysisCacheReadMetrics AnalysisCacheTableReader::Metrics() const { return m_impl->metrics; }
void AnalysisCacheTableReader::ValidateAll()
{
    auto& s=*m_impl;
    AnalysisWorkspaceReservation previousMemory(s.options.workspace,65536);
    Sha256Builder hash; std::string previous; previous.reserve(65536);
    uint64_t block=UINT64_MAX, nextOffset=0;
    for(uint64_t i=0;i<RecordCount();++i) {
        const auto position=s.Position(i);
        if(position.block!=block) {
            if(block!=UINT64_MAX && nextOffset!=s.blocks[size_t(block)].bytes) Fail("index_coverage");
            block=position.block; nextOffset=0;
        }
        if(position.offset!=nextOffset) Fail("index_coverage");
        const auto [key,payload]=s.View(i);
        if(i && key<=previous) Fail("key_order");
        hash.Update(&position.keyBytes,4); hash.Update(&position.payloadBytes,4);
        hash.Update(key.data(),key.size()); hash.Update(payload.data(),payload.size());
        previous.assign(key); nextOffset+=8+key.size()+payload.size();
    }
    if(block!=UINT64_MAX && nextOffset!=s.blocks[size_t(block)].bytes) Fail("index_coverage");
    if(hash.FinalHex()!=s.descriptor.contentSha256) Fail("content_checksum");
}
AnalysisCacheRecord AnalysisCacheTableReader::GetAt(uint64_t ordinal)
{
    const auto [key,payload] = m_impl->View(ordinal);
    return {key,payload,m_impl->options.workspace};
}
std::optional<AnalysisCacheRecord> AnalysisCacheTableReader::Find(std::string_view key)
{
    const auto ordinal = LowerBound(key);
    if(ordinal == RecordCount()) return std::nullopt;
    const auto [found,payload] = m_impl->View(ordinal);
    if(found == key) return AnalysisCacheRecord{found,payload,m_impl->options.workspace};
    return std::nullopt;
}
uint64_t AnalysisCacheTableReader::LowerBound(std::string_view key)
{
    CheckCancel(m_impl->options);
    uint64_t first = 0, last = RecordCount();
    if(m_impl->loaded!=UINT64_MAX)
    {
        // Repeated ordered lookups (definition/context joins) should stay in
        // the one already verified block. Starting every binary search at the
        // table midpoint would evict and rehash it for almost every record.
        // No second block, key directory or per-lookup allocation is retained.
        const auto& block=m_impl->blocks[size_t(m_impl->loaded)];
        const auto begin=block.first,end=begin+block.records;
        const auto firstKey=m_impl->View(begin).first;
        const auto lastKey=m_impl->View(end-1).first;
        if(key<firstKey) last=begin;
        else if(key>lastKey) first=end;
        else { first=begin; last=end; }
    }
    while(first < last)
    {
        const auto middle = first+(last-first)/2;
        const auto [found,payload] = m_impl->View(middle);
        if(found < key) first = middle+1;
        else last = middle;
    }
    return first;
}
AnalysisCachePage AnalysisCacheTableReader::ReadPage(uint64_t ordinal, size_t limit, uint64_t maxResponseBytes)
{
    if(limit == 0 || limit > 1000 || maxResponseBytes == 0 || maxResponseBytes > 16*1024*1024) Fail("page_budget");
    if(ordinal > RecordCount()) Fail("ordinal_out_of_range");
    CheckCancel(m_impl->options);
    AnalysisCachePage page; page.nextOrdinal = ordinal;
    uint64_t bytes = 0;
    while(page.records.size() < limit && page.nextOrdinal < RecordCount())
    {
        const auto [key,payload] = m_impl->View(page.nextOrdinal);
        const auto size = uint64_t(key.size())+payload.size();
        if(size > maxResponseBytes-bytes)
        {
            if(page.records.empty()) Fail("response_budget");
            break;
        }
        // Construct (and reserve) before push_back may grow the page vector.
        AnalysisCacheRecord record(key,payload,m_impl->options.workspace);
        page.records.push_back(std::move(record));
        bytes += size; ++page.nextOrdinal;
    }
    page.done = page.nextOrdinal == RecordCount();
    return page;
}
}
