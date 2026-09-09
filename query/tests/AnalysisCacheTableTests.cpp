#include <iostream>
#include "TracyAnalysisCacheTable.hpp"
#include "TracyAnalysisCacheSort.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

using namespace tracy::analysis;
namespace {
void Require(bool condition, const char* message)
{
    if(!condition) throw std::runtime_error(message);
}
template<class F> void Reject(F&& action, const std::string& reason)
{
    try { action(); }
    catch(const std::exception& e)
    {
        if(std::string(e.what()).find(reason) != std::string::npos) return;
        throw std::runtime_error("wrong rejection: " + std::string(e.what()));
    }
    throw std::runtime_error("expected rejection: " + reason);
}
std::string Key(uint64_t value)
{
    std::ostringstream stream;
    stream << std::setw(12) << std::setfill('0') << value;
    return stream.str();
}
std::string Payload(uint64_t value) { return std::string(72, 'x') + std::to_string(value); }
const std::string Identity(64, 'a');

void LookupLocality(const std::filesystem::path& root)
{
    AnalysisCacheTableOptions options; options.blockBytes=16384;
    {
        AnalysisCacheTableWriter writer(root,Identity,"lookup-locality",options);
        for(uint64_t i=0;i<300;++i) writer.Append(Key(2*i+2),Payload(i));
        Require(writer.Commit().blocks==2,"locality fixture must cross exactly two immutable blocks");
    }
    AnalysisCacheTableReader reader(root,Identity,"lookup-locality",options);
    Require(reader.LowerBound(Key(0))==0 && !reader.Find(Key(1)),"missing keys below the first record");
    for(uint64_t i=0;i<300;++i) {
        const auto found=reader.Find(Key(2*i+2));
        Require(found && found->payload==Payload(i),"ordered repeated point lookup must preserve every record");
        Require(reader.LowerBound(Key(2*i+3))==i+1 && !reader.Find(Key(2*i+3)),"ordered missing-key lookup retains exact lower-bound ordinal");
    }
    const auto ascending=reader.Metrics().blocksLoaded;
    for(uint64_t i=300;i>0;--i) {
        const auto found=reader.Find(Key(2*i));
        Require(found && found->payload==Payload(i-1),"descending lookup remains exact across cached block bounds");
    }
    const auto descending=reader.Metrics().blocksLoaded-ascending;
    std::cerr<<"lookup locality: blocks=2 ascending_loads="<<ascending<<" descending_loads="<<descending<<'\n';
    Require(ascending<=16 && descending<=16,"monotone point lookups must not rehash other blocks for every record");
    for(uint64_t i=0;i<300;++i) {
        const auto ordinal=(i*73)%300;
        const auto found=reader.Find(Key(2*ordinal+2));
        Require(found && found->payload==Payload(ordinal),"random lookup remains correct after local-bound searches");
    }
    Require(reader.LowerBound(Key(9999))==300 && !reader.Find(Key(9999)),"missing key above the final cached block");
    Require(reader.Metrics().peakBlockBytes<=16384,"lookup locality must not retain a second payload block");
}

void ReturnedRecordWorkspace(const std::filesystem::path& root)
{
    AnalysisCacheTableOptions options; options.blockBytes=65536; options.manifestBytes=65536;
    { AnalysisCacheTableWriter writer(root,Identity,"owned-record",options);
      writer.Append("a",std::string(8192,'a')); writer.Append("b",std::string(8192,'b')); writer.Commit(); }
    auto budget=std::make_shared<AnalysisWorkspaceBudget>(2*1024*1024,1024*1024);
    options.workspace=budget;
    {
        AnalysisCacheTableReader reader(root,Identity,"owned-record",options);
        reader.GetAt(0); // Warm the block; no block allocation may mask this boundary.
        const auto readerBytes=budget->Snapshot().currentBytes;
        AnalysisWorkspaceReservation pressure(budget,2*1024*1024-readerBytes-128);
        unsigned bypasses=0;
        for(const unsigned operation:{0,1,2}) {
            bool rejected=false;
            try { if(operation==0) reader.GetAt(0); else if(operation==1) reader.Find("a"); else reader.ReadPage(0,2,65536); }
            catch(const std::exception& e) { if(std::string(e.what()).find("workspace_budget")==std::string::npos) throw; rejected=true; }
            bypasses+=!rejected;
        }
        if(bypasses) std::cerr<<"unbudgeted warm reader return paths="<<bypasses<<'\n';
        Require(bypasses==0,"GetAt, Find and ReadPage must reserve returned strings before copying a warm source block");
        pressure.Resize(0);
        {
            auto first=reader.GetAt(0);
            Require(first.payload==std::string(8192,'a') && budget->Snapshot().currentBytes>=readerBytes+8192,
                "returned record must retain its workspace after GetAt returns");
            auto moved=std::move(first);
            auto other=reader.GetAt(1); const auto both=budget->Snapshot().currentBytes;
            other=std::move(moved);
            Require(other.payload==std::string(8192,'a') && budget->Snapshot().currentBytes<both &&
                budget->Snapshot().currentBytes>=readerBytes+8192,"move assignment releases only the replaced record after its strings die");
        }
        Require(budget->Snapshot().currentBytes==readerBytes,"record destruction releases its budget without dropping the source block");
        {
            auto page=reader.ReadPage(0,2,65536);
            Require(page.records.size()==2 && budget->Snapshot().currentBytes>=readerBytes+16384,
                "page must hold all returned record payloads through caller use");
            auto moved=std::move(page);
            Require(moved.records[1].payload==std::string(8192,'b') && budget->Snapshot().currentBytes>=readerBytes+16384,
                "moving a page transfers, not drops, its memory ownership");
        }
        Require(budget->Snapshot().currentBytes==readerBytes,"page destruction releases its strings and record capacity budget");
    }
    Require(budget->Snapshot().currentBytes==0,"reader and all returned objects release every shared reservation");
}

void Scale(uint64_t count, const std::filesystem::path& root)
{
    Require(count > 0 && count <= 10000000, "invalid synthetic fixture size");
    const auto start = std::chrono::steady_clock::now();
    AnalysisCacheTableOptions options;
    options.blockBytes = 1024 * 1024;
    AnalysisCacheTableDescriptor description;
    uint64_t writerPeak = 0;
    {
        AnalysisCacheTableWriter writer(root, Identity, "scale-fixture", options);
        for(uint64_t i = 0; i < count; ++i) writer.Append(Key(i), Payload(i));
        description = writer.Commit(); writerPeak = writer.PeakBufferedBytes();
    }
    const auto written = std::chrono::steady_clock::now();
    uint64_t readPeak = 0;
    {
        AnalysisCacheTableReader reader(root, Identity, "scale-fixture", options);
        Require(reader.RecordCount() == count, "scale count mismatch");
        Require(reader.GetAt(count-1).payload == Payload(count-1), "scale direct read mismatch");
        Require(reader.Metrics().blocksLoaded == 1, "scale direct read loaded all blocks");
        uint64_t seen = 0;
        while(seen < count)
        {
            const auto page = reader.ReadPage(seen, 1000, 256*1024);
            for(const auto& record : page.records)
            {
                Require(record.key == Key(seen) && record.payload == Payload(seen), "scale pagination mismatch");
                ++seen;
            }
            Require(page.nextOrdinal == seen && page.done == (seen == count), "scale cursor mismatch");
        }
        readPeak = reader.Metrics().peakBlockBytes;
    }
    const auto finished = std::chrono::steady_clock::now();
    Require(writerPeak <= options.blockBytes && readPeak <= options.blockBytes, "scale payload buffer growth");
    std::cout << "{\"records\":" << count << ",\"blocks\":" << description.blocks
        << ",\"write_ms\":" << std::chrono::duration_cast<std::chrono::milliseconds>(written-start).count()
        << ",\"read_verify_ms\":" << std::chrono::duration_cast<std::chrono::milliseconds>(finished-written).count()
        << ",\"peak_buffered_payload_bytes\":" << writerPeak << ",\"peak_loaded_payload_bytes\":" << readPeak
        << ",\"content_sha256\":\"" << description.contentSha256 << "\",\"verified\":true}\n";
}
}

int main(int argc, char** argv)
{
    const auto temporary = std::filesystem::temp_directory_path() /
        ("jn-analysis-cache-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try
    {
        std::filesystem::create_directories(temporary);
        if(argc == 6 && std::string(argv[1]) == "--inspect-body")
        {
            // Read-only diagnostics of one individually published table. This
            // never upgrades an incomplete parent generation to valid output.
            AnalysisCacheTableReader reader(argv[2],argv[3],argv[4]);
            uint64_t matches=0;
            for(uint64_t i=0;i<reader.RecordCount();++i) {
                const auto record=reader.GetAt(i);
                if(record.payload.find(argv[5])==std::string::npos) continue;
                std::cout<<i<<'\t'<<record.payload<<'\n'; ++matches;
            }
            std::filesystem::remove_all(temporary);
            return matches?0:2;
        }
        if(argc == 3 && std::string(argv[1]) == "--scale")
        {
            Scale(std::stoull(argv[2]), temporary / "scale");
            std::filesystem::remove_all(temporary);
            return 0;
        }
        ReturnedRecordWorkspace(temporary/"returned-workspace");
        LookupLocality(temporary/"lookup-locality");
        { // Close every reader/writer handle before deleting Windows fixtures.
        AnalysisCacheTableOptions options;
        options.blockBytes = 256;
        {
            AnalysisCacheSortedWriter unordered(temporary / "sorted", Identity, "index", options, 1024);
            for(uint64_t i = 40; i > 0; --i) unordered.Append(Key(i-1), Payload(i-1));
            const auto sorted = unordered.Commit();
            Require(sorted.records == 40, "external sort lost records");
            AnalysisCacheTableReader ordered(temporary / "sorted", Identity, "index", options);
            for(uint64_t i = 0; i < 40; ++i)
            {
                const auto value = ordered.GetAt(i);
                Require(value.key == Key(i) && value.payload == Payload(i), "external sort changed content or order");
            }
        }
        const auto root = temporary / "table";
        AnalysisCacheTableWriter writer(root, Identity, "statistics", options);
        for(uint64_t i = 0; i < 40; ++i) writer.Append(Key(i), Payload(i));
        Require(!std::filesystem::exists(root / "manifest.json"), "uncommitted table appeared complete");
        Reject([&] { AnalysisCacheTableReader incomplete(root, Identity, "statistics", options); }, "not_complete");
        const auto descriptor = writer.Commit();
        Require(descriptor.records == 40 && descriptor.blocks > 1, "writer did not split records into blocks");
        Require(writer.PeakBufferedBytes() <= options.blockBytes, "writer retained more than one payload block");

        AnalysisCacheTableReader reader(root, Identity, "statistics", options);
        Require(reader.RecordCount() == 40, "record count lost on reopen");
        const auto last = reader.GetAt(39);
        Require(last.key == Key(39) && last.payload == Payload(39), "random ordinal returned wrong record");
        Require(reader.Metrics().blocksLoaded == 1, "single record read loaded the complete table");
        const auto found = reader.Find(Key(17));
        Require(found && found->payload == Payload(17), "key index lookup failed");
        Require(!reader.Find("not-present"), "absent key was fabricated");
        Require(reader.LowerBound(Key(17)) == 17 && reader.LowerBound(Key(17)+"a") == 18 &&
            reader.LowerBound("") == 0 && reader.LowerBound(Key(999)) == 40, "range index did not find exact insertion boundary");
        const auto page = reader.ReadPage(3, 10, 200);
        Require(page.records.size() == 2 && page.nextOrdinal == 5 && !page.done, "byte-limited cross-block page incorrect");
        Require(page.records[0].key == Key(3) && page.records[1].payload == Payload(4), "page skipped or duplicated records");
        uint64_t ordinal = 0, visited = 0;
        while(ordinal < 40)
        {
            const auto values = reader.ReadPage(ordinal, 7, 1024);
            for(const auto& value : values.records)
            {
                Require(value.key == Key(visited) && value.payload == Payload(visited), "pagination order/content changed");
                ++visited;
            }
            Require(values.nextOrdinal > ordinal, "cursor failed to advance");
            ordinal = values.nextOrdinal;
        }
        Require(visited == 40 && reader.Metrics().peakBlockBytes <= 256, "reader lost records or retained oversized block");
        Reject([&] { reader.ReadPage(0, 1, 8); }, "response_budget");
        Reject([&] { reader.GetAt(40); }, "ordinal_out_of_range");
        Reject([&] { AnalysisCacheTableReader wrong(root, std::string(64,'b'), "statistics", options); }, "identity_mismatch");
        Reject([&] { AnalysisCacheTableReader wrong(root, Identity, "candidates", options); }, "kind_mismatch");
        Reject([&] { AnalysisCacheTableWriter overwrite(root, Identity, "statistics", options); }, "already_exists");

        // Block boundaries may change without changing the exact record stream.
        auto larger = options;
        larger.blockBytes = 1024;
        AnalysisCacheTableWriter second(temporary / "larger-blocks", Identity, "statistics", larger);
        for(uint64_t i = 0; i < 40; ++i) second.Append(Key(i), Payload(i));
        Require(second.Commit().contentSha256 == descriptor.contentSha256, "content identity depends on block size");

        AnalysisCacheTableWriter empty(temporary / "empty", Identity, "statistics", options);
        empty.Commit();
        AnalysisCacheTableReader emptyReader(temporary / "empty", Identity, "statistics", options);
        Require(emptyReader.RecordCount() == 0 && emptyReader.ReadPage(0, 8, 100).done && !emptyReader.Find("x"), "empty table was not exact empty");

        AnalysisCacheTableWriter duplicate(temporary / "duplicate", Identity, "statistics", options);
        duplicate.Append("same", "first");
        Reject([&] { duplicate.Append("same", "second"); }, "key_order");
        Reject([&] { duplicate.Commit(); }, "writer_failed");
        Require(!std::filesystem::exists(temporary / "duplicate" / "manifest.json"), "failed writer published");
        {
            AnalysisCacheSortedWriter duplicateRuns(temporary / "duplicate-runs",Identity,"index",options,1024);
            for(uint64_t i = 0; i < 20; ++i) duplicateRuns.Append(Key(i),Payload(i));
            duplicateRuns.Append(Key(0),"different payload with same identity");
            Reject([&] { duplicateRuns.Commit(); }, "key_order");
            Require(!std::filesystem::exists(temporary / "duplicate-runs" / "manifest.json"), "merge silently discarded duplicate identity");
        }

        AnalysisCacheTableWriter oversized(temporary / "oversized", Identity, "statistics", options);
        Reject([&] { oversized.Append("key", std::string(256,'x')); }, "record_budget");
        Require(!std::filesystem::exists(temporary / "oversized" / "manifest.json"), "oversize record was truncated and published");

        bool cancelled = false;
        auto interruptible = options;
        interruptible.cancelled = [&] { return cancelled; };
        AnalysisCacheTableWriter interrupted(temporary / "cancelled", Identity, "statistics", interruptible);
        interrupted.Append("first", "valid");
        cancelled = true;
        Reject([&] { interrupted.Commit(); }, "cancelled");
        Require(!std::filesystem::exists(temporary / "cancelled" / "manifest.json"), "cancelled writer published");

        // Corruption must be reported at the first access, not replaced by []
        // or a stale result. Existing clean generations are not touched.
        const auto block = root / "blocks" / "0000000000000000.bin";
        {
            std::fstream corrupt(block, std::ios::in | std::ios::out | std::ios::binary);
            Require(bool(corrupt), "cannot open fixture block");
            corrupt.seekp(-1, std::ios::end); corrupt.put('!');
        }
        AnalysisCacheTableReader corruptReader(root, Identity, "statistics", options);
        Reject([&] { corruptReader.GetAt(0); }, "block_checksum");
        std::filesystem::resize_file(temporary / "larger-blocks" / "index.bin", 1);
        Reject([&] { AnalysisCacheTableReader truncated(temporary / "larger-blocks", Identity, "statistics", larger); }, "index_size");
        }
        std::filesystem::remove_all(temporary);
        std::cout << "PASS: exact multi-block storage, indexed reads, bounded pages, identity/corruption/cancellation\n";
        return 0;
    }
    catch(const std::exception& e)
    {
        std::cerr << "FAIL: " << e.what() << "\nfixtures: " << temporary.string() << '\n';
        return 1;
    }
}
