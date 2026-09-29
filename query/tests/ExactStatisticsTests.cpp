#include "TracyExactStatistics.hpp"
#include "TracyHash.hpp"
#include "TracyAnalysisCacheTable.hpp"
#include "TracyAnalysisExternalSort.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <iostream>
#include <sstream>

using namespace tracy::analysis;

namespace
{

std::filesystem::path TemporaryRoot()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        ( "jn-tracy-exact-statistics-test-" + std::to_string( stamp ) );
}

const NeutralSignatureAggregate& Find( const NeutralStatisticsResult& result, const char* id )
{
    const auto found = std::find_if( result.signatures.begin(), result.signatures.end(),
        [&]( const auto& value ) { return value.signatureId == id; } );
    assert( found != result.signatures.end() );
    return *found;
}

NeutralSignatureFrameInput Input( const char* id, uint64_t frame, int64_t inclusive,
    int64_t exclusive, int64_t wait = 0, int64_t critical = 0 )
{
    NeutralSignatureFrameInput value;
    value.domain = "cpu"; value.signatureId = id; value.frameScope = "Player.Frame";
    value.frameIndex = frame; value.inclusiveNs = inclusive; value.exclusiveNs = exclusive;
    value.waitNs = wait; value.criticalPathNs = critical; value.occurrenceCount = 1;
    return value;
}

NeutralSignatureDenominatorInput Denominator( const char* id, uint64_t count )
{
    return { "cpu", id, "Player.Frame", count };
}

double PercentileOracle( const std::vector<double>& sorted, double percentile )
{
    if( sorted.empty() ) return 0;
    const auto position = percentile * double( sorted.size() - 1 );
    const auto lower = size_t( std::floor( position ) );
    const auto upper = size_t( std::ceil( position ) );
    return sorted[lower] + ( sorted[upper] - sorted[lower] ) * ( position - double( lower ) );
}

ExactDistribution DistributionOracle( const std::vector<int64_t>& explicitValues, uint64_t zeros )
{
    std::vector<double> values( size_t( zeros ), 0 );
    for( const auto value : explicitValues ) values.push_back( double( value ) );
    std::sort( values.begin(), values.end() );
    ExactDistribution result;
    result.count = values.size();
    result.zeroCount = std::count( values.begin(), values.end(), 0.0 );
    if( values.empty() ) return result;
    for( const auto value : values ) result.total += int64_t( value );
    result.min = int64_t( values.front() ); result.max = int64_t( values.back() );
    result.mean = double( result.total ) / double( result.count );
    result.median = result.p50 = PercentileOracle( values, 0.5 );
    result.p90 = PercentileOracle( values, 0.9 );
    result.p95 = PercentileOracle( values, 0.95 );
    result.p99 = PercentileOracle( values, 0.99 );
    std::vector<double> deviations;
    for( const auto value : values ) deviations.push_back( std::abs( value - result.median ) );
    std::sort( deviations.begin(), deviations.end() );
    result.mad = PercentileOracle( deviations, 0.5 );
    return result;
}

void AssertDistribution( const ExactDistribution& actual, const ExactDistribution& expected )
{
    assert( actual.exact && actual.count == expected.count && actual.zeroCount == expected.zeroCount );
    assert( actual.total == expected.total && actual.min == expected.min && actual.max == expected.max );
    assert( std::abs( actual.mean - expected.mean ) < 0.00001 );
    assert( std::abs( actual.median - expected.median ) < 0.00001 );
    assert( std::abs( actual.mad - expected.mad ) < 0.00001 );
    assert( std::abs( actual.p50 - expected.p50 ) < 0.00001 );
    assert( std::abs( actual.p90 - expected.p90 ) < 0.00001 );
    assert( std::abs( actual.p95 - expected.p95 ) < 0.00001 );
    assert( std::abs( actual.p99 - expected.p99 ) < 0.00001 );
}

NeutralStatisticsStreamBuilder BudgetedBuilder(const std::filesystem::path& root,
    const std::shared_ptr<AnalysisWorkspaceBudget>& workspace, size_t raw = 4)
{
    return NeutralStatisticsStreamBuilder(root, 4, raw, workspace);
}

template<class Action> bool BudgetRejected(Action&& action)
{
    try { action(); }
    catch(const std::runtime_error& error)
    { return std::string(error.what()).find("analysis_workspace_budget") != std::string::npos; }
    return false;
}

template<class Builder>
Builder DiskBuilder(const std::filesystem::path& root,const std::shared_ptr<AnalysisDiskBudget>& disk)
{
    if constexpr(std::is_constructible_v<Builder,std::filesystem::path,uint64_t,size_t,
        std::shared_ptr<AnalysisWorkspaceBudget>,std::shared_ptr<AnalysisDiskBudget>>)
        return Builder(root,2,16,{},disk);
    else return Builder(root,2,16);
}
template<class Disk> ExactDistribution DiskDistribution(const std::filesystem::path& root,const Disk& disk)
{
    const std::vector<int64_t> values{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    if constexpr(requires{ComputeExactDistributionExternal(values,0,root,"disk",2,disk);})
        return ComputeExactDistributionExternal(values,0,root,"disk",2,disk);
    else return ComputeExactDistributionExternal(values,0,root,"disk",2);
}
void DiskStatistics(const std::filesystem::path& root)
{
    unsigned missed=0;
    for(const bool raw:{true,false})
    {
        const auto path=root/(raw?"disk-raw":"disk-distribution");
        auto usage=std::make_shared<AnalysisDiskUsage>(std::vector<std::filesystem::path>{path});
        AnalysisDiskActivity activity(usage);
        auto disk=std::make_shared<AnalysisDiskBudget>(AnalysisDiskBudget{usage,128,{}});
        bool denied=false;
        try
        {
            if(raw)
            {
                auto source=DiskBuilder<NeutralStatisticsStreamBuilder>(path,disk);
                for(uint64_t i=0;i<32;++i) source.AddRun(Input("disk",i,i+1,i+1));
                NeutralStatisticsStreamSummary summary; std::string error;
                if(!source.FinishToSink(summary,error,[](const auto&,const auto&){}))
                    denied=error=="analysis_scan_cache_disk_budget";
            }
            else (void)DiskDistribution(path,disk);
        }
        catch(const std::runtime_error& e) { if(std::string(e.what())!="analysis_scan_cache_disk_budget") throw; denied=true; }
        if(!denied || AnalysisDiskUsage::Bytes(path)>128)
        { ++missed; std::cerr<<"Unbudgeted exact statistics disk growth: "<<(raw?"raw":"distribution")<<'\n'; }
    }
    if(missed) throw std::runtime_error("Exact statistics must reject file growth before exceeding the shared disk quota");
    const auto path=root/"disk-recovery";
    auto usage=std::make_shared<AnalysisDiskUsage>(std::vector<std::filesystem::path>{path});
    AnalysisDiskActivity activity(usage);
    auto disk=std::make_shared<AnalysisDiskBudget>(AnalysisDiskBudget{usage,256*1024,{}});
    const auto distribution=DiskDistribution(path,disk);
    if(!distribution.exact || distribution.median!=8.5 || distribution.total!=136 ||
        usage->Current()!=0 || AnalysisDiskUsage::Bytes(path)!=0)
        throw std::runtime_error("Exact distribution cleanup must return disk capacity without changing exact results");
    auto source=DiskBuilder<NeutralStatisticsStreamBuilder>(path,disk);
    for(uint64_t i=0;i<128;++i) assert(source.AddRun(Input("disk",i,i+1,i+1)));
    source.AddDenominator(Denominator("disk",128));
    NeutralStatisticsStreamSummary summary; std::string error; unsigned emitted=0;
    if(!source.FinishToSink(summary,error,[&](const auto& s,const auto& frames){
        ++emitted;
        if(s.inclusive.perCompleteFrame.median!=64.5 || frames.size()!=128)
            throw std::runtime_error("Disk-accounted stream changed exact statistics");
    }) || emitted!=1 || usage->Current()!=0 || AnalysisDiskUsage::Bytes(path)!=0)
        throw std::runtime_error("Raw runs, merge runs and distribution scratch files must release their live disk charge");
    {
        const auto empty=root/"empty-sort";
        std::filesystem::create_directories(empty);
        { std::ofstream source(empty/"source",std::ios::binary); }
        { std::ofstream output(empty/"output",std::ios::binary); output<<"old output"; }
        auto emptyUsage=std::make_shared<AnalysisDiskUsage>(std::vector<std::filesystem::path>{empty});
        AnalysisDiskActivity emptyActivity(emptyUsage);
        auto emptyDisk=std::make_shared<AnalysisDiskBudget>(AnalysisDiskBudget{emptyUsage,10,{}});
        if(!SortAnalysisUInt64Pairs(empty/"source",empty/"output",empty,"empty",0,2,error,emptyDisk) ||
            AnalysisDiskUsage::Bytes(empty)!=0 || emptyUsage->Current()!=0)
        {
            std::cerr<<"Empty sort replacement: actual="<<AnalysisDiskUsage::Bytes(empty)
                <<" charged="<<emptyUsage->Current()<<" error="<<error<<'\n';
            throw std::runtime_error("Replacing a prior sort output with an empty result must release its old disk charge");
        }
    }
    {
        const auto streaming=root/"streaming-pair-sort";
        std::filesystem::create_directories(streaming);
        auto pairUsage=std::make_shared<AnalysisDiskUsage>(std::vector<std::filesystem::path>{streaming});
        AnalysisDiskActivity pairActivity(pairUsage);
        auto pairDisk=std::make_shared<AnalysisDiskBudget>(AnalysisDiskBudget{pairUsage,256*1024,{}});
        auto pairWorkspace=std::make_shared<AnalysisWorkspaceBudget>(256*1024,128*1024);
        AnalysisExternalSortStatistics statistics;
        AnalysisUInt64PairRunCollector collector(streaming/"output",streaming,"pairs",2,
            pairDisk,pairWorkspace,{},&statistics);
        collector.Emit({2,1}); collector.Emit({1,3}); collector.Emit({1,2});
        if(!collector.Finish(error)) throw std::runtime_error(error);
        std::ifstream input(streaming/"output",std::ios::binary);
        std::vector<AnalysisUInt64Pair> values(3);
        input.read(reinterpret_cast<char*>(values.data()),std::streamsize(values.size()*sizeof(values[0])));
        if(!input || values[0].key!=1 || values[0].value!=2 || values[1].key!=1 ||
            values[1].value!=3 || values[2].key!=2 || values[2].value!=1 ||
            statistics.inputRecords!=3 || statistics.initialRuns!=2)
            throw std::runtime_error("Streaming pair collector changed key/value ordering or statistics");
        input.close();
        std::error_code cleanupError;
        AnalysisDiskRemove(streaming,pairDisk,cleanupError,true);
        if(cleanupError || pairUsage->Current()!=0 || pairWorkspace->Snapshot().currentBytes!=0)
            throw std::runtime_error("Streaming pair collector did not release its disk/workspace charge");
    }
}

template<class Builder> bool CheckpointRoundTrip(const std::filesystem::path& root)
{
    const std::string identity(64,'c');
    if constexpr(requires(Builder& b) { b.SaveCheckpoint(root,identity); b.RestoreCheckpoint(root,identity); })
    {
        const auto populate=[](auto& b,uint64_t first,uint64_t last) {
            for(uint64_t f=first;f<last;++f) for(const auto* id:{"z","a"})
                assert(b.AddRun(Input(id,f,int64_t(f+1),int64_t(f))));
        };
        const auto metadata=[](auto& b) {
            for(const auto* id:{"z","a"})b.AddDenominator(Denominator(id,4));
            b.AddDomainAudit({"cpu",true,"complete",8,8,2,std::string(64,'f'),std::string(64,'f'),true,{}});
        };
        NeutralStatisticsResult expected;std::string error;
        {Builder b(root/"baseline",8,2);populate(b,0,4);metadata(b);assert(b.Finish(expected,error));}
        {
            Builder b(root/"resume",8,2);populate(b,0,2);metadata(b);
            b.SaveCheckpoint(root/"resume"/"checkpoint",identity);
        }
        const auto abandoned=root/"resume"/"neutral-run-9999.work";
        const auto unrelated=root/"resume"/"unrelated.work";
        {std::ofstream file(abandoned,std::ios::binary);file<<"partial";}
        {std::ofstream file(unrelated,std::ios::binary);file<<"keep";}
        for(int attempt=0;attempt<2;++attempt) {
            Builder b(root/"resume",8,2);b.RestoreCheckpoint(root/"resume"/"checkpoint",identity);
            if constexpr(requires {b.CleanupUnreferencedRuns();})b.CleanupUnreferencedRuns();
            if(std::filesystem::exists(abandoned) || !std::filesystem::exists(unrelated))
            {std::cerr<<"restored phase must retire only abandoned owned runs"<<std::endl;return false;}
            assert(b.InputRunCount()==4 && b.FindSignatureToken("cpu","z","Player.Frame")==0 &&
                b.FindSignatureToken("cpu","a","Player.Frame")==1);
            populate(b,2,4); NeutralStatisticsResult result;assert(b.Finish(result,error));
            assert(result.qualityComplete && result.contentSha256==expected.contentSha256);
        }
        bool wrong=false;
        try {Builder b(root/"resume",8,2);b.RestoreCheckpoint(root/"resume"/"checkpoint",std::string(64,'d'));}
        catch(const std::exception&){wrong=true;}
        assert(wrong);
        std::filesystem::path run;
        for(const auto& file:std::filesystem::directory_iterator(root/"resume"))
            if(file.path().filename().string().starts_with("neutral-run-") && file.path().extension()==".work") {run=file.path();break;}
        assert(!run.empty()); {std::ofstream output(run,std::ios::binary|std::ios::trunc);output<<'x';}
        bool corrupt=false;
        try {Builder b(root/"resume",8,2);b.RestoreCheckpoint(root/"resume"/"checkpoint",identity);}
        catch(const std::exception&){corrupt=true;}
        assert(corrupt);return true;
    }
    else {std::cerr<<"statistics has no complete resumable checkpoint"<<std::endl;return false;}
}

}

int main( int argc, char** argv )
{
    if(argc>1 && std::string_view(argv[1])=="--checkpoint-only")
    {
        const auto root=TemporaryRoot();const auto ok=CheckpointRoundTrip<NeutralStatisticsStreamBuilder>(root);
        std::filesystem::remove_all(root);return ok?0:1;
    }
    if(argc>1 && std::string_view(argv[1])=="--constant-distribution-only")
    {
        const auto root=TemporaryRoot(); auto disk=std::make_shared<AnalysisDiskBudget>();
        disk->checkSpace=[] {throw std::runtime_error("constant distribution wrote sort scratch");};
        try {
            for(const int64_t value:{0,3,9}) for(const uint64_t zeros:{0,7,4096,6000}) {
                const std::vector<int64_t> values(4096,value);
                const auto actual=ComputeExactDistributionExternal(values,zeros,root,"constant",128,disk);
                AssertDistribution(actual,DistributionOracle(values,zeros));
            }
            const auto maximum=std::numeric_limits<int64_t>::max();
            const auto large=ComputeExactDistributionExternal({maximum},1,root,"large",1,disk);
            assert(large.exact && large.total==maximum && large.median==double(maximum)/2 && large.mad==double(maximum)/2);
            const auto overflow=ComputeExactDistributionExternal({maximum,maximum},0,root,"overflow",1,disk);
            assert(!overflow.exact && overflow.unavailableReason=="statistics_total_overflow");
            assert(!std::filesystem::exists(root)); return 0;
        } catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;return 1;}
    }
    if(argc>1 && std::string_view(argv[1])=="--equal-distribution-only")
    {
        const auto root=TemporaryRoot();
        auto disk=std::make_shared<AnalysisDiskBudget>(); uint64_t writes=0;
        disk->checkSpace=[&]{++writes;};
        std::vector<int64_t> values(512);
        for(size_t i=0;i<values.size();++i) values[i]=(i*47)%1009;
        const auto expected=ComputeExactDistributionExternal(values,0,root,"oracle",128,disk);
        const auto singleWrites=writes;
        NeutralStatisticsStreamBuilder stream(root/"stream",128,512,{},disk);
        for(size_t i=0;i<values.size();++i) assert(stream.AddRun(Input("equal",i,values[i],values[i],values[i],values[i])));
        stream.AddDenominator(Denominator("equal",values.size()));
        writes=0; NeutralStatisticsStreamSummary summary; std::string error; uint64_t outputs=0;
        assert(stream.FinishToSink(summary,error,[&](const auto& stat,const auto&) {
            ++outputs;
            for(const auto* metric:{&stat.inclusive,&stat.exclusive,&stat.wait,&stat.criticalPath}) {
                AssertDistribution(metric->whenPresent,expected); AssertDistribution(metric->perCompleteFrame,expected);
            }
        }));
        assert(outputs==1);
        if(writes>4*singleWrites)
        {std::cerr<<"identical complete/present distributions sorted twice: writes="<<writes<<" single="<<singleWrites<<std::endl;return 1;}
        std::filesystem::remove_all(root); return 0;
    }
    if(argc>1 && std::string_view(argv[1])=="--distribution-cancel-only")
    {
        const auto root=TemporaryRoot();
        auto usage=std::make_shared<AnalysisDiskUsage>(std::vector<std::filesystem::path>{root});
        AnalysisDiskActivity activity(usage);
        auto disk=std::make_shared<AnalysisDiskBudget>(); disk->usage=usage;
        std::vector<int64_t> values(8192);
        for(size_t i=0;i<values.size();++i) values[i]=(i*47)%1009;
        for(const uint64_t buffer:{128,65536})
        {
            size_t checks=0;
            const auto result=ComputeExactDistributionExternal(values,7,root,"cancel",buffer,disk,[&]{return ++checks==16;});
            if(result.exact || result.unavailableReason!="cancelled" || checks!=16)
            {std::cerr<<"distribution failed cancellation buffer="<<buffer<<" checks="<<checks<<std::endl;return 1;}
            assert(usage->Snapshot().currentBytes==0 && AnalysisDiskUsage::Bytes(root)==0);
        }
        std::filesystem::remove_all(root); return 0;
    }
    if(argc>1 && std::string_view(argv[1])=="--reduce-cancel-only")
    {
        const auto root=TemporaryRoot();
        NeutralStatisticsStreamBuilder stream(root,128,1024);
        for(uint64_t i=0;i<256;++i) assert(stream.AddRun(Input("cancel",i,i,i)));
        stream.AddDenominator(Denominator("cancel",256));
        bool cancel=false; uint64_t outputs=0, classified=0;
        stream.SetCompleteFrameFilter([&](auto,auto,auto){ ++classified; cancel=true; return true; });
        NeutralStatisticsStreamSummary result; std::string error;
        const auto ok=stream.FinishToSink(result,error,[&](auto&,auto&){++outputs;},[&]{return cancel;});
        if(ok || error!="cancelled" || outputs!=0 || classified>128)
        { std::cerr<<"reduce ignored cancellation inside one signature: outputs="<<outputs<<" classified="<<classified<<std::endl;return 1; }
        std::filesystem::remove_all(root);
        return 0;
    }
    if(argc>1 && std::string_view(argv[1])=="--distribution-disk-only")
    {
        const auto root=TemporaryRoot();
        auto usage=std::make_shared<AnalysisDiskUsage>(std::vector<std::filesystem::path>{root});
        AnalysisDiskActivity activity(usage);
        auto disk=std::make_shared<AnalysisDiskBudget>(); disk->usage=usage;
        std::vector<int64_t> values;
        for(int i=0;i<4096;++i) values.push_back((i*47)%1009);
        const auto actual=ComputeExactDistributionExternal(values,701,root,"bounded",128,disk);
        AssertDistribution(actual,DistributionOracle(values,701));
        const auto state=usage->Snapshot();
        std::cout<<"distribution_disk_peak="<<state.peakBytes<<" remaining="<<state.currentBytes<<'\n';
        if(state.peakBytes>3*values.size()*sizeof(AnalysisUInt64Pair))
        { std::cerr<<"distribution retains redundant unsorted source files; peak="<<state.peakBytes<<std::endl; return 1; }
        assert(state.currentBytes==0 && AnalysisDiskUsage::Bytes(root)==0);
        {
            const auto zeroRoot=root/"zero-dominant";
            auto zeroUsage=std::make_shared<AnalysisDiskUsage>(std::vector<std::filesystem::path>{zeroRoot});
            AnalysisDiskActivity zeroActivity(zeroUsage);
            auto zeroDisk=std::make_shared<AnalysisDiskBudget>();zeroDisk->usage=zeroUsage;
            const auto zeroResult=ComputeExactDistributionExternal(values,3*values.size(),zeroRoot,"zero",128,zeroDisk);
            AssertDistribution(zeroResult,DistributionOracle(values,3*values.size()));
            if(zeroUsage->Snapshot().peakBytes>2*values.size()*sizeof(AnalysisUInt64Pair))
            {std::cerr<<"zero median rebuilt an unnecessary deviation sort; peak="<<zeroUsage->Snapshot().peakBytes<<std::endl;return 1;}
            assert(zeroUsage->Current()==0 && AnalysisDiskUsage::Bytes(zeroRoot)==0);
        }
        std::filesystem::remove_all(root);
        return 0;
    }
    const auto temporary = TemporaryRoot();
    {
        constexpr uint64_t Count=1025;
        for(const uint64_t maximum:{256ull*1024*1024,16ull*1024*1024})
        {
            auto workspace=std::make_shared<AnalysisWorkspaceBudget>(maximum,maximum/2);
            NeutralStatisticsStreamBuilder stream(temporary/("adaptive-"+std::to_string(maximum)),16,1,workspace);
            uint64_t mergedRecords=0;
            stream.SetProgress([&](std::string_view stage,uint64_t done,uint64_t total){
                if(stage.starts_with("neutral_statistics_merge_pass_") && done==total)mergedRecords+=done;
            });
            for(uint64_t frame=0;frame<Count;++frame)assert(stream.AddRun(Input("adaptive",frame,1,1)));
            stream.AddDenominator(Denominator("adaptive",Count));
            stream.AddDomainAudit({"cpu",true,"complete",Count,Count,1,std::string(64,'a'),std::string(64,'a'),true,{}});
            NeutralStatisticsStreamSummary result;std::string error;uint64_t signatures=0;
            const bool ok=stream.FinishToSink(result,error,[&](const auto& value,const auto& frames){
                ++signatures;assert(frames.size()==Count && value.exclusive.perCompleteFrame.total==Count &&
                    value.exclusive.perCompleteFrame.median==1);
            });
            if(!ok || !result.qualityComplete || signatures!=1){std::cerr<<"Adaptive merge failed: "<<error<<'\n';return 1;}
            if(maximum>=256ull*1024*1024 && mergedRecords>2*Count)
            {std::cerr<<"Statistics merge reread an avoidable third pass despite available workspace\n";return 1;}
            assert(workspace->Snapshot().peakBytes<=maximum);
        }
        if(argc>1 && std::string_view(argv[1])=="--adaptive-merge-only")
        {std::filesystem::remove_all(temporary);return 0;}
    }
    std::filesystem::create_directories( temporary );

    {
        const auto compressedRoot = temporary / "compressed-multiblock-run";
        NeutralStatisticsStreamBuilder source( compressedRoot, 64, 5000 );
        const auto token = source.RegisterSignature( "cpu", "compressed", "Player.Frame" );
        assert( token != NeutralStatisticsStreamBuilder::InvalidSignatureToken );
        for( uint64_t frame = 0; frame < 5000; frame++ )
            assert( source.AddRegisteredRun( token, frame, int64_t( frame % 17 ), int64_t( frame % 13 ),
                int64_t( frame % 5 ), int64_t( frame % 7 ), 1, true, false ) );
        source.AddDenominator( { "cpu", "compressed", "Player.Frame", 5000 } );
        source.AddDomainAudit( { "cpu", true, "complete", 5000, 5000, 1,
            std::string( 64, 'a' ), std::string( 64, 'a' ), true, {} } );
        const auto run = *std::filesystem::directory_iterator( compressedRoot );
        std::ifstream header( run.path(), std::ios::binary );
        char magic[8] {};
        header.read( magic, sizeof( magic ) );
        const auto runBytes = std::filesystem::file_size( run.path() );
        if( header.gcount() != sizeof( magic ) || std::memcmp( magic, "JNNRUN2", 8 ) != 0 ||
            runBytes >= 64 + 5000 * 56 / 2 )
        {
            std::cerr << "Neutral run is not the bounded compressed v2 format: bytes=" << runBytes << '\n';
            return 1;
        }
        size_t sinkCalls = 0;
        NeutralStatisticsStreamSummary summary;
        std::string error;
        if( !source.FinishToSink( summary, error, [&]( const auto& aggregate, const auto& frames ) {
                ++sinkCalls;
                assert( aggregate.signatureId == "compressed" && frames.size() == 5000 );
            } ) || sinkCalls != 1 )
        {
            std::cerr << "Compressed multi-block run failed: " << error << '\n';
            return 1;
        }
    }
    {
        NeutralStatisticsStreamBuilder source( temporary / "compressed-multilevel-merge", 64, 64 );
        const auto token = source.RegisterSignature( "cpu", "multilevel", "Player.Frame" );
        assert( token != NeutralStatisticsStreamBuilder::InvalidSignatureToken );
        for( uint64_t frame = 0; frame < 5000; frame++ )
            assert( source.AddRegisteredRun( token, frame, int64_t( frame ), int64_t( frame ),
                0, 0, 1, true, false ) );
        source.AddDenominator( { "cpu", "multilevel", "Player.Frame", 5000 } );
        source.AddDomainAudit( { "cpu", true, "complete", 5000, 5000, 1,
            std::string( 64, 'b' ), std::string( 64, 'b' ), true, {} } );
        size_t frameCount = 0;
        NeutralStatisticsStreamSummary summary;
        std::string error;
        if( !source.FinishToSink( summary, error, [&]( const auto&, const auto& frames ) {
                frameCount += frames.size();
            } ) || frameCount != 5000 )
        {
            std::cerr << "Compressed 32-way multi-level merge failed: " << error << '\n';
            return 1;
        }
    }

    const auto rejectDamagedRun = [&]( std::string_view label, const auto& damage,
        std::string_view expectedError ) {
        const auto root = temporary / std::string( label );
        NeutralStatisticsStreamBuilder source( root, 4, 2 );
        const auto token = source.RegisterSignature( "cpu", "damaged", "Player.Frame" );
        assert( token != NeutralStatisticsStreamBuilder::InvalidSignatureToken );
        assert( source.AddRegisteredRun( token, 0, 1, 1, 0, 0, 1, true, false ) );
        assert( source.AddRegisteredRun( token, 1, 2, 2, 0, 0, 1, true, false ) );
        const auto run = *std::filesystem::directory_iterator( root );
        damage( run.path() );
        NeutralStatisticsStreamSummary summary;
        std::string error;
        const auto ok = source.FinishToSink( summary, error, []( const auto&, const auto& ) {} );
        if( ok || error != expectedError )
        {
            std::cerr << "Damaged neutral run was accepted: case=" << label << " ok=" << ok <<
                " error=" << error << " expected=" << expectedError << '\n';
            return false;
        }
        return true;
    };
    if( !rejectDamagedRun( "truncated-neutral-run", []( const auto& path ) {
            const auto bytes = std::filesystem::file_size( path );
            assert( bytes > 1 );
            std::filesystem::resize_file( path, bytes - 1 );
        }, "neutral_run_truncated" ) ||
        !rejectDamagedRun( "missing-record-neutral-run", []( const auto& path ) {
            const auto bytes = std::filesystem::file_size( path );
            assert( bytes > 64 + 56 );
            std::filesystem::resize_file( path, bytes - 56 );
        }, "neutral_run_truncated" ) ||
        !rejectDamagedRun( "extra-data-neutral-run", []( const auto& path ) {
            std::ofstream output( path, std::ios::binary | std::ios::app );
            output.put( '\x01' );
        }, "neutral_run_extra_data" ) ||
        !rejectDamagedRun( "header-neutral-run", []( const auto& path ) {
            std::fstream file( path, std::ios::binary | std::ios::in | std::ios::out );
            file.seekp( 0 );
            file.put( 'X' );
            file.flush();
        }, "neutral_run_header_invalid" ) ||
        !rejectDamagedRun( "checksum-neutral-run", []( const auto& path ) {
            std::fstream file( path, std::ios::binary | std::ios::in | std::ios::out );
            file.seekg( -1, std::ios::end );
            char value = 0;
            file.read( &value, 1 );
            value ^= char( 0x80 );
            file.seekp( -1, std::ios::end );
            file.write( &value, 1 );
            file.flush();
        }, "neutral_run_checksum_mismatch" ) ||
        !rejectDamagedRun( "missing-neutral-run", []( const auto& path ) {
            std::filesystem::remove( path );
        }, "neutral_final_run_open_failed" ) ) return 1;
    {
        if( argc > 1 && std::string( argv[1] ) == "--run-integrity-only" )
        { std::filesystem::remove_all( temporary ); return 0; }
    }

    if(argc == 3 && std::string(argv[1]) == "--scale-streamed")
    {
        const auto count = std::stoull(argv[2]);
        if(count == 0 || count > 2000000) return 2;
        const auto start = std::chrono::steady_clock::now();
        {
            NeutralStatisticsStreamBuilder source(temporary / "raw", 65536, 65536);
            for(uint64_t i = 0; i < count; ++i)
            {
                const auto name = "scale-" + std::to_string(i);
                assert(source.AddRun(Input(name.c_str(), 0, i%2 ? 10 : 0, i%2 ? 10 : 0)));
                source.AddDenominator(Denominator(name.c_str(), 4));
            }
            source.AddDomainAudit({"cpu",true,"complete",count,count,count,
                std::string(64,'a'),std::string(64,'a'),true,{}});
            AnalysisCacheTableWriter cache(temporary / "table", std::string(64,'a'), "neutral-signatures");
            NeutralStatisticsStreamSummary summary;
            std::string error;
            uint64_t emitted = 0;
            assert(source.FinishToSink(summary, error, [&](const auto& signature, const auto& frames) {
                assert(frames.size() == 1 && signature.completeFrameCount == 4);
                auto key = std::to_string(emitted++); key.insert(0,20-key.size(),'0');
                cache.Append(key, SerializeNeutralSignatureRecord(signature));
            }));
            assert(summary.qualityComplete && emitted == count && summary.materializedResultPeak == 0);
            const auto written = cache.Commit();
            const auto writeEnd = std::chrono::steady_clock::now();
            AnalysisCacheTableReader reader(temporary / "table", std::string(64,'a'), "neutral-signatures");
            uint64_t seen = 0;
            while(seen < count)
            {
                const auto page = reader.ReadPage(seen, 100, 1024*1024);
                for(const auto& record : page.records)
                {
                    const auto value = nlohmann::json::parse(record.payload);
                    assert(value.at("signature_id") == "scale-"+std::to_string(seen));
                    assert(value.at("exclusive").at("per_complete_frame").at("total_ns") == (seen%2 ? "10" : "0"));
                    assert(value.at("complete_frame_count") == "4" && value.at("unknown_frame_count") == "0");
                    ++seen;
                }
                assert(page.nextOrdinal == seen && page.done == (seen == count));
            }
            const auto end = std::chrono::steady_clock::now();
            std::cout << "{\"records\":" << count << ",\"blocks\":" << written.blocks
                << ",\"build_ms\":" << std::chrono::duration_cast<std::chrono::milliseconds>(writeEnd-start).count()
                << ",\"read_verify_ms\":" << std::chrono::duration_cast<std::chrono::milliseconds>(end-writeEnd).count()
                << ",\"materialized_result_peak\":" << summary.materializedResultPeak
                << ",\"content_sha256\":\"" << written.contentSha256 << "\",\"verified\":true}\n";
        }
        std::filesystem::remove_all(temporary);
        return 0;
    }

    DiskStatistics(temporary);
    if(argc>1 && std::string(argv[1])=="--disk-only")
    { std::filesystem::remove_all(temporary); std::cout<<"disk statistics tests passed\n"; return 0; }
    {
        // Missing registry accounting lets one scan consume space reserved by
        // another scan. A repeated key must not consume a second reservation.
        auto workspace = std::make_shared<AnalysisWorkspaceBudget>(1024*1024,512*1024);
        {
            auto source = BudgetedBuilder(temporary / "budget-signatures",workspace);
            const auto token = source.RegisterSignature("cpu","existing","Frame");
            const auto retained = workspace->Snapshot().currentBytes;
            AnalysisWorkspaceReservation occupied(workspace,1024*1024-retained-128);
            assert(source.RegisterSignature("cpu","existing","Frame") == token);
            assert(workspace->Snapshot().currentBytes == 1024*1024-128);
            if(!BudgetRejected([&] { source.RegisterSignature("cpu",std::string(1024,'x'),"Frame"); }))
            { std::cerr << "Statistics registry bypassed shared workspace budget\n"; return 1; }
        }
        assert(workspace->Snapshot().currentBytes == 0);
    }

    {
        bool allRejected = true;
        auto workspace = std::make_shared<AnalysisWorkspaceBudget>(2*1024*1024,1024*1024);
        if(!BudgetRejected([&] { auto source = BudgetedBuilder(temporary / "budget-raw",workspace,65536); }))
        { std::cerr << "Statistics raw buffers bypassed shared workspace budget\n"; allRejected = false; }
        assert(workspace->Snapshot().currentBytes == 0);
        {
            auto source = BudgetedBuilder(temporary / "budget-denominator",workspace);
            AnalysisWorkspaceReservation occupied(workspace,2*1024*1024-workspace->Snapshot().currentBytes-128);
            if(!BudgetRejected([&] { source.AddDenominator(Denominator(std::string(1024,'d').c_str(),5)); }))
            { std::cerr << "Statistics denominators bypassed shared workspace budget\n"; allRejected = false; }
            if(!BudgetRejected([&] { source.AddDomainAudit({std::string(1024,'a'),true,"complete"}); }))
            { std::cerr << "Statistics audit bypassed shared workspace budget\n"; allRejected = false; }
        }
        assert(workspace->Snapshot().currentBytes == 0);
        {
            // A streaming finish must not bypass the budget just because its
            // raw input was externally merged in small batches.
            auto source = BudgetedBuilder(temporary / "budget-merged-frames",workspace,256);
            const auto token = source.RegisterSignature("cpu","large","Player.Frame");
            for(uint64_t frame=0; frame<8192; ++frame)
                assert(source.AddRegisteredRun(token,frame,1,1,0,0,1,true,false));
            source.AddDenominator(Denominator("large",8192));
            source.AddDomainAudit({"cpu",true,"complete",8192,8192,1,
                std::string(64,'e'),std::string(64,'e'),true,{}});
            uint64_t emitted = 0;
            NeutralStatisticsStreamSummary summary;
            std::string error;
            const auto rejected = BudgetRejected([&] {
                if(!source.FinishToSink(summary,error,[&](const auto&,const auto&) { ++emitted; }) &&
                    error.find("analysis_workspace_budget") != std::string::npos)
                    throw std::runtime_error(error);
            });
            if(!rejected || emitted != 0)
            { std::cerr << "Streaming statistics finish bypassed workspace budget\n"; allRejected = false; }
        }
        assert(workspace->Snapshot().currentBytes == 0);
        if(!allRejected) return 1;
    }

    {
        // Shared scope text must not be copied into every registry and
        // denominator key. Removing dictionary use breaks this bounded scan.
        auto workspace=std::make_shared<AnalysisWorkspaceBudget>(8*1024*1024,4*1024*1024);
        const char* dictionaryStage="construct"; uint64_t insertedKeys=0;
        try {
            // This test isolates registry/string retention. Four-record raw
            // batches would create 256 runs and legitimately exceed 8 MiB
            // when opening 32 merge readers; use bounded 64-record batches.
            auto source=BudgetedBuilder(temporary/"dictionary-scope-sharing",workspace,64);
            const std::string scope="Frame-"+std::string(64*1024,'x');
            for(uint64_t i=0;i<1024;++i)
            {
                dictionaryStage="register";
                const auto id="distinct-"+std::to_string(i);
                const auto token=source.RegisterSignature("cpu",id,scope);
                assert(token!=NeutralStatisticsStreamBuilder::InvalidSignatureToken);
                assert(source.AddRegisteredRun(token,0,6,6,0,0,1,true,false));
                dictionaryStage="denominator";
                source.AddDenominator({"cpu",id,scope,4});
                ++insertedKeys;
            }
            dictionaryStage="audit";
            source.AddDomainAudit({"cpu",true,"complete",1024,1024,1024,
                std::string(64,'a'),std::string(64,'a'),true,{}});
            NeutralStatisticsStreamSummary summary;
            std::string error; uint64_t count=0;
            dictionaryStage="finish";
            assert(source.FinishToSink(summary,error,[&](const auto& signature,const auto& frames) {
                assert(signature.frameScope==scope && signature.completeFrameCount==4 && frames.size()==1);
                assert(signature.exclusive.perCompleteFrame.total==6 && signature.exclusive.perCompleteFrame.zeroCount==3);
                ++count;
            }));
            assert(count==1024 && summary.qualityComplete && summary.materializedResultPeak==0);
        } catch(const std::exception& e) {
            std::cerr<<"Shared dictionary scope must fit 1024 signatures under 2 MiB: "<<e.what()
                <<" stage="<<dictionaryStage<<" inserted="<<insertedKeys<<" peak="<<workspace->Snapshot().peakBytes<<'\n'; return 1;
        }
        assert(workspace->Snapshot().currentBytes==0);
    }

    {
        // A disk consumer receives one signature at a time. Completed result
        // objects must not also accumulate behind that consumer's back.
        NeutralStatisticsStreamBuilder streamed(temporary / "sink-output", 64, 64);
        bool mergeProgress = false, reduceProgress = false;
        streamed.SetProgress([&](std::string_view stage,uint64_t completed,uint64_t total) {
            assert(completed<=total);
            mergeProgress = mergeProgress || stage.starts_with("neutral_statistics_merge_pass_");
            reduceProgress = reduceProgress || stage=="neutral_statistics_reduce";
        });
        constexpr uint64_t Count = 256;
        for(uint64_t i = 0; i < Count; ++i)
        {
            const auto name = "sink-" + std::to_string(i);
            streamed.AddDenominator(Denominator(name.c_str(), 4));
            for(uint64_t frame = 0; frame < 3; ++frame)
                assert(streamed.AddRun(Input(name.c_str(), frame, int64_t(frame+1), int64_t(frame+1))));
        }
        streamed.AddDomainAudit({"cpu",true,"complete",Count*3,Count*3,Count,
            std::string(64,'e'),std::string(64,'e'),true,{}});
        NeutralStatisticsStreamSummary summary;
        uint64_t received = 0;
        std::string error;
        AnalysisCacheTableOptions cacheOptions;
        cacheOptions.blockBytes = 16384;
        AnalysisCacheTableWriter cache(temporary / "streamed-cache", std::string(64,'a'), "neutral-signatures", cacheOptions);
        std::ostringstream timingLog;
        const auto oldError=std::cerr.rdbuf(timingLog.rdbuf());
        bool ok=false;
        try { ok = streamed.FinishToSink(summary, error, [&](const auto& signature, const auto& frames) {
            ++received;
            assert(frames.size() == 3 && signature.presentFrameCount == 3);
            assert(signature.exclusive.perCompleteFrame.total == 6);
            assert(signature.exclusive.perCompleteFrame.count == 4);
            assert(signature.exclusive.perCompleteFrame.zeroCount == 1);
            assert(signature.exclusive.perCompleteFrame.median == 1.5);
            assert(std::abs(signature.exclusive.perCompleteFrame.p95-2.85) < 0.00001);
            auto key = std::to_string(received);
            key.insert(0, 20-key.size(), '0');
            cache.Append(key, SerializeNeutralSignatureRecord(signature));
        }); } catch(...) { std::cerr.rdbuf(oldError); throw; }
        std::cerr.rdbuf(oldError);
        const auto log=timingLog.str();
        assert(log.find("Query statistics-phase stage=merge duration_ms=")!=std::string::npos);
        assert(log.find("Query statistics-phase stage=reduce duration_ms=")!=std::string::npos);
        if(!ok || received != Count || summary.signatureCount != Count || !mergeProgress || !reduceProgress ||
            !summary.qualityComplete || summary.materializedResultPeak != 0)
        { std::cerr << "Sink output still retains complete signature results: " << summary.materializedResultPeak << '\n'; return 1; }
        assert(summary.domains.size() == 1 && summary.domains[0].actualOutputCount == Count);
        const auto cached = cache.Commit();
        assert(cached.records == Count && cached.blocks > 1);
        {
            AnalysisCacheTableReader reader(temporary / "streamed-cache", std::string(64,'a'), "neutral-signatures", cacheOptions);
            const auto record = nlohmann::json::parse(reader.GetAt(Count-1).payload);
            assert(record.at("signature_id") == "sink-255");
            assert(record.at("exclusive").at("per_complete_frame").at("total_ns") == "6");
            assert(record.at("exclusive").at("per_complete_frame").at("median_ns") == "1.5");
            assert(reader.Metrics().blocksLoaded == 1);
        }
        NeutralStatisticsStreamBuilder stopped(temporary / "cancelled-sink", 64, 64);
        assert(stopped.AddRun(Input("cancelled", 0, 1, 1)));
        stopped.AddDenominator(Denominator("cancelled", 1));
        stopped.AddDomainAudit({"cpu",true,"complete",1,1,1,
            std::string(64,'f'),std::string(64,'f'),true,{}});
        uint64_t cancelledEmissions = 0;
        NeutralStatisticsStreamSummary cancelledSummary;
        const auto cancelledResult = stopped.FinishToSink(cancelledSummary, error,
            [&](const auto&, const auto&) { ++cancelledEmissions; }, [] { return true; });
        if(cancelledResult || cancelledSummary.qualityComplete || cancelledEmissions != 0)
        { std::cerr << "Cancelled short streaming input was processed/passed\n"; return 1; }
        if(argc > 1 && std::string(argv[1]) == "--stream-sink-only")
        { std::filesystem::remove_all(temporary); return 0; }
    }

    {
        // Million-signature captures expose retained vector capacity, even
        // though each signature has few rows and no wait/critical anomalies.
        NeutralStatisticsInput sparseSignatures;
        sparseSignatures.temporaryRoot = temporary / "anomaly-retention";
        constexpr uint64_t Count = 4096;
        for( uint64_t index = 0; index < Count; ++index )
        {
            const auto name = "sparse-" + std::to_string( index );
            sparseSignatures.runs.push_back( Input( name.c_str(), 1,
                index % 2 == 0 ? 0 : 10, index % 2 == 0 ? 0 : 10 ) );
            sparseSignatures.denominators.push_back( Denominator( name.c_str(), 4 ) );
        }
        sparseSignatures.domainAudit = { { "cpu", true, "complete", Count, Count, Count,
            std::string( 64, 'd' ), std::string( 64, 'd' ), true, {} } };
        const auto sparseResult = BuildNeutralStatistics( sparseSignatures );
        assert( sparseResult.qualityComplete && sparseResult.signatures.size() == Count );
        uint64_t retained = 0, useful = 0;
        for( const auto& signature : sparseResult.signatures )
            for( const auto* metric : { &signature.inclusive, &signature.exclusive,
                &signature.wait, &signature.criticalPath } )
            {
                retained += metric->anomalies.capacity() * sizeof( AnomalyInstance );
                useful += metric->anomalies.size() * sizeof( AnomalyInstance );
            }
        std::cout << "ANOMALY_RETENTION signatures=" << Count << " retained_bytes=" << retained
            << " useful_bytes=" << useful << " result_sha256=" << sparseResult.contentSha256 << '\n';
        if( retained != useful )
        { std::cerr << "Finished metrics retain empty or unused provisional anomaly slots\n"; return 1; }
        if( argc > 1 && std::string( argv[1] ) == "--anomaly-retention-only" )
        {
            std::filesystem::remove_all( temporary );
            return 0;
        }
    }

    {
        NeutralStatisticsInput two;
        two.temporaryRoot = temporary / "two-spikes";
        for( uint64_t i = 0; i < 20; ++i ) two.runs.push_back( Input( "two", i,
            i == 3 || i == 15 ? 100 : 1, i == 3 || i == 15 ? 100 : 1 ) );
        two.denominators = { Denominator( "two", 20 ) };
        const auto measured = BuildNeutralStatistics( two );
        if( Find( measured, "two" ).exclusive.pattern != AnomalyPattern::RecurrentSpike )
        { std::cerr << "Two isolated anomalies cannot disappear between one and three\n"; return 1; }
    }

    {
        NeutralStatisticsInput scoped;
        scoped.temporaryRoot = temporary / "independent-frame-sets";
        scoped.runs = { Input( "player", 1, 10, 10 ), Input( "render", 1, 100, 100 ) };
        scoped.runs[1].frameScope = "Render.Frame";
        scoped.denominators = { Denominator( "player", 1 ),
            { "cpu", "render", "Render.Frame", 1 } };
        scoped.domainAudit = { { "cpu", true, "complete", 2, 2, 2,
            std::string( 64, 'a' ), std::string( 64, 'a' ), true, {} } };
        const auto scopedResult = BuildNeutralStatistics( scoped );
        if( scopedResult.rankings.size() != 2 || scopedResult.rankings[0].exclusive.size() != 1 ||
            scopedResult.rankings[1].exclusive.size() != 1 )
        { std::cerr << "FrameSets must not compete in one ranking\n"; return 1; }
    }
    {
        NeutralStatisticsInput unknown;
        unknown.temporaryRoot = temporary / "unknown-is-not-zero";
        unknown.runs = { Input( "partial", 1, 10, 10 ), Input( "partial", 2, 0, 0 ) };
        unknown.runs[1].exact = false;
        unknown.denominators = { Denominator( "partial", 3 ) };
        unknown.domainAudit = { { "cpu", true, "invalid", 2, 2, 1,
            std::string( 64, 'a' ), std::string( 64, 'a' ), true, "source_open_zone" } };
        const auto partial = BuildNeutralStatistics( unknown );
        const auto& metric = Find( partial, "partial" ).exclusive;
        if( !partial.qualityComplete || metric.perCompleteFrame.exact ||
            metric.perCompleteFrame.count != 2 || metric.perCompleteFrame.zeroCount != 1 ||
            metric.whenPresent.count != 1 )
        { std::cerr << "Unknown source frame must not count as known zero or scanner failure\n"; return 1; }
    }

    const auto even = ComputeExactDistributionExternal( { 1, 2, 3, 4 }, 0,
        temporary, "even", 2 );
    assert( even.exact && even.count == 4 && even.min == 1 && even.max == 4 );
    assert( even.total == 10 && even.mean == 2.5 && even.median == 2.5 );
    assert( even.p50 == 2.5 && std::abs( even.p90 - 3.7 ) < 0.00001 );
    assert( even.mad == 1.0 );

    const auto odd = ComputeExactDistributionExternal( { 1, 2, 3 }, 0,
        temporary, "odd", 2 );
    assert( odd.median == 2.0 && odd.mad == 1.0 );

    const auto sparse = ComputeExactDistributionExternal( { 10, 20 }, 3,
        temporary, "sparse", 1 );
    assert( sparse.count == 5 && sparse.zeroCount == 3 );
    assert( sparse.median == 0 && sparse.p90 == 16 && sparse.max == 20 );

    const auto allZero = ComputeExactDistributionExternal( {}, 7, temporary, "all-zero", 1 );
    AssertDistribution( allZero, DistributionOracle( {}, 7 ) );
    const auto invalidDuration = ComputeExactDistributionExternal( { 1, -1 }, 0,
        temporary, "negative", 1 );
    assert( !invalidDuration.exact && invalidDuration.unavailableReason == "negative_duration_not_supported" );

    // A signature that fits the configured bounded buffer must not depend on
    // filesystem-backed external sorting.  A regular file is deliberately
    // supplied where a temporary directory would otherwise be required.
    const auto blockedTemporary = temporary / "not-a-directory";
    {
        std::ofstream blocker( blockedTemporary, std::ios::binary | std::ios::trunc );
        blocker << "block";
    }
    const auto boundedInMemory = ComputeExactDistributionExternal(
        { 9, 3, 7, 3 }, 12, blockedTemporary, "bounded-in-memory", 4 );
    AssertDistribution( boundedInMemory, DistributionOracle( { 9, 3, 7, 3 }, 12 ) );

    std::mt19937_64 random( 0x515343414e38ull );
    for( size_t iteration = 0; iteration < 64; ++iteration )
    {
        std::vector<int64_t> values( size_t( random() % 34 ) );
        for( auto& value : values ) value = int64_t( random() % 101 );
        const auto zeros = uint64_t( random() % 11 );
        const auto actual = ComputeExactDistributionExternal( values, zeros, temporary,
            "oracle-" + std::to_string( iteration ), 1 + random() % 7 );
        AssertDistribution( actual, DistributionOracle( values, zeros ) );
    }
    std::vector<int64_t> externalMergeValues( 8192 );
    for( size_t index = 0; index < externalMergeValues.size(); ++index )
        externalMergeValues[index] = int64_t( ( index * 7919 ) % 100003 );
    const auto externalMerge = ComputeExactDistributionExternal( externalMergeValues, 317,
        temporary, "external-merge-corpus", 257 );
    AssertDistribution( externalMerge, DistributionOracle( externalMergeValues, 317 ) );

    NeutralStatisticsInput anomalyInput;
    anomalyInput.temporaryRoot = temporary / "bounded-anomaly-summary";
    anomalyInput.maximumBufferedValues = 4096;
    for( uint64_t frame = 0; frame < 2000; ++frame )
        anomalyInput.runs.push_back( Input( "many-anomalies", frame,
            frame < 1800 ? 1 : 100, frame < 1800 ? 1 : 100 ) );
    anomalyInput.denominators.push_back( Denominator( "many-anomalies", 2000 ) );
    const auto boundedAnomalies = BuildNeutralStatistics( anomalyInput );
    const auto& manyAnomalies = Find( boundedAnomalies, "many-anomalies" ).exclusive;
    assert( manyAnomalies.anomalyCount == 200 );
    assert( !manyAnomalies.anomaliesComplete );
    assert( manyAnomalies.anomalies.size() <= NeutralMaximumRepresentativeAnomalies );
    assert( manyAnomalies.longestBurstFrames == 200 );
    assert( manyAnomalies.longestBurstStartFrame == 1800 );
    assert( manyAnomalies.longestBurstEndFrame == 1999 );

    NeutralStatisticsInput logicalOnlyInput;
    logicalOnlyInput.temporaryRoot = temporary / "logical-only-ranking";
    logicalOnlyInput.runs = { { "cpu", "cpu-logical:site", "Player.Frame", 0,
        10, 7, 0, 0, 1, true, true } };
    logicalOnlyInput.denominators = { { "cpu", "cpu-logical:site", "Player.Frame", 1 } };
    logicalOnlyInput.domainAudit = { { "cpu", true, "complete", 1, 1, 1,
        std::string( 64, 'e' ), std::string( 64, 'e' ), true, {} } };
    const auto logicalOnlyResult = BuildNeutralStatistics( logicalOnlyInput );
    assert( logicalOnlyResult.rankings.size() == 1 );
    assert( logicalOnlyResult.rankings.front().exclusive.size() == 1 );
    assert( logicalOnlyResult.rankings.front().exclusive.front().logical );

    NeutralStatisticsInput input;
    input.temporaryRoot = temporary / "build-a";
    input.maximumBufferedValues = 2;
    input.runs = {
        Input( "sparse", 1, 10, 10 ), Input( "sparse", 3, 20, 20 ),
        Input( "sparse", 3, 5, 5 ),
        Input( "isolated", 0, 1, 1 ), Input( "isolated", 1, 1, 1 ),
        Input( "isolated", 2, 1, 1 ), Input( "isolated", 3, 20, 20 ),
        Input( "recurrent", 0, 1, 1 ), Input( "recurrent", 1, 20, 20 ),
        Input( "recurrent", 2, 1, 1 ), Input( "recurrent", 3, 20, 20 ),
        Input( "recurrent", 4, 1, 1 ), Input( "recurrent", 5, 20, 20 ),
        Input( "recurrent", 6, 1, 1 ), Input( "recurrent", 7, 1, 1 ),
        Input( "recurrent", 8, 1, 1 ), Input( "recurrent", 9, 1, 1 ),
        Input( "burst", 0, 1, 1 ), Input( "burst", 1, 1, 1 ),
        Input( "burst", 2, 1, 1 ), Input( "burst", 3, 20, 20 ),
        Input( "burst", 4, 20, 20 ), Input( "burst", 5, 20, 20 ),
        Input( "burst", 6, 1, 1 ), Input( "burst", 7, 1, 1 ),
        Input( "burst", 8, 1, 1 ), Input( "burst", 9, 1, 1 ),
        Input( "persistent", 0, 5, 5, 2 ), Input( "persistent", 1, 5, 5, 2 ),
        Input( "persistent", 2, 5, 5, 2 ), Input( "persistent", 3, 5, 5, 2 ),
        Input( "inclusive-parent", 0, 100, 0 )
    };
    input.denominators = {
        Denominator( "sparse", 5 ), Denominator( "isolated", 4 ),
        Denominator( "recurrent", 10 ), Denominator( "burst", 10 ),
        Denominator( "persistent", 4 ), Denominator( "inclusive-parent", 4 )
    };
    input.domainAudit = {
        { "cpu", true, "complete", input.runs.size(), input.runs.size(), 6,
            std::string( 64, 'a' ), std::string( 64, 'a' ), true, {} },
        { "gpu.catalog", false, "absent", 0, 0, 0,
            std::string( 64, '0' ), std::string( 64, '0' ), true, "catalog disabled for fixture" }
    };

    const auto result = BuildNeutralStatistics( input );
    assert( result.qualityComplete );
    assert( result.unreportedGapCount == 0 );
    assert( result.contentSha256.size() == 64 );
    assert( result.maximumBufferedValuesObserved <= 2 );

    const auto& sparseSignature = Find( result, "sparse" );
    assert( sparseSignature.exclusive.perCompleteFrame.count == 5 );
    assert( sparseSignature.exclusive.perCompleteFrame.zeroCount == 3 );
    assert( sparseSignature.exclusive.perCompleteFrame.median == 0 );
    assert( sparseSignature.exclusive.whenPresent.count == 2 );
    assert( sparseSignature.exclusive.whenPresent.median == 17.5 );
    assert( sparseSignature.occurrenceCount == 3 );

    const auto& isolated = Find( result, "isolated" );
    assert( isolated.exclusive.pattern == AnomalyPattern::IsolatedSpike );
    assert( isolated.exclusive.anomalies.size() == 1 );
    assert( isolated.exclusive.anomalies[0].frameIndex == 3 );
    assert( isolated.exclusive.anomalies[0].deltaFromMedianNs == 19 );

    const auto& recurrent = Find( result, "recurrent" );
    assert( recurrent.exclusive.pattern == AnomalyPattern::RecurrentSpike );
    assert( recurrent.exclusive.anomalies.size() == 3 );
    assert( recurrent.exclusive.periodFrames && *recurrent.exclusive.periodFrames == 2 );

    const auto& burst = Find( result, "burst" );
    assert( burst.exclusive.pattern == AnomalyPattern::BurstWindow );
    assert( burst.exclusive.longestBurstFrames == 3 );

    const auto& persistent = Find( result, "persistent" );
    assert( persistent.exclusive.pattern == AnomalyPattern::PersistentPressure );
    assert( persistent.exclusive.perCompleteFrame.mad == 0 );

    const auto cpuRanking = std::find_if( result.rankings.begin(), result.rankings.end(),
        []( const auto& value ) { return value.domain == "cpu"; } );
    assert( cpuRanking != result.rankings.end() );
    assert( !cpuRanking->inclusive.empty() && cpuRanking->inclusive.front().signatureId == "inclusive-parent" );
    assert( !cpuRanking->exclusive.empty() && cpuRanking->exclusive.front().signatureId == "burst" );
    assert( !cpuRanking->waitCritical.empty() && cpuRanking->waitCritical.front().signatureId == "persistent" );
    assert( std::abs( cpuRanking->exclusive.back().cumulativeContribution - 1.0 ) < 0.00001 );

    auto shuffled = input;
    shuffled.temporaryRoot = temporary / "build-b";
    std::reverse( shuffled.runs.begin(), shuffled.runs.end() );
    std::reverse( shuffled.denominators.begin(), shuffled.denominators.end() );
    const auto shuffledResult = BuildNeutralStatistics( shuffled );
    assert( shuffledResult.contentSha256 == result.contentSha256 );

    // The production path accepts arbitrarily many signature-frame inputs
    // without retaining them all in memory. The exact result must match the
    // legacy vector entry point while the raw-run buffer remains bounded.
    NeutralStatisticsStreamBuilder streamBuilder( temporary / "streaming", 127, 31 );
    NeutralStatisticsInput streamingOracle;
    streamingOracle.temporaryRoot = temporary / "streaming-oracle";
    streamingOracle.maximumBufferedValues = 127;
    constexpr uint64_t StreamingFrames = 2048;
    constexpr uint64_t StreamingSignatures = 96;
    for( uint64_t signature = 0; signature < StreamingSignatures; ++signature )
    {
        const auto name = "stream-" + std::to_string( signature );
        const auto denominator = Denominator( name.c_str(), StreamingFrames );
        const auto token = streamBuilder.RegisterSignature( "cpu", name, "Player.Frame" );
        assert( token != NeutralStatisticsStreamBuilder::InvalidSignatureToken );
        streamBuilder.AddDenominator( denominator );
        streamingOracle.denominators.push_back( denominator );
        for( uint64_t frame = 0; frame < StreamingFrames; ++frame )
        {
            const auto value = int64_t( ( signature * 17 + frame * 13 ) % 10000 );
            auto item = Input( name.c_str(), frame, value + 3, value );
            assert( streamBuilder.AddRegisteredRun( token, frame,
                item.inclusiveNs, item.exclusiveNs, item.waitNs,
                item.criticalPathNs, item.occurrenceCount,
                item.exact, item.logical ) );
            streamingOracle.runs.push_back( std::move( item ) );
        }
    }
    const auto streamingCount = StreamingFrames * StreamingSignatures;
    const auto streamingAudit = NeutralDomainAuditInput { "cpu", true, "complete",
        streamingCount, streamingCount, StreamingSignatures,
        std::string( 64, 'c' ), std::string( 64, 'c' ), true, {} };
    streamBuilder.AddDomainAudit( streamingAudit );
    streamingOracle.domainAudit.push_back( streamingAudit );
    NeutralStatisticsResult streamedResult;
    std::string streamingError;
    assert( streamBuilder.Finish( streamedResult, streamingError ) );
    const auto oracleResult = BuildNeutralStatistics( streamingOracle );
    assert( streamingError.empty() );
    assert( streamedResult.contentSha256 == oracleResult.contentSha256 );
    assert( streamBuilder.MaximumBufferedRunsObserved() <= 31 );
    assert( streamBuilder.InputRunCount() == streamingCount );

    NeutralAggregateIdentity identity;
    identity.traceStrongId = std::string( 64, '1' );
    identity.queryExecutableSha256 = std::string( 64, '2' );
    identity.querySchema = "1.35.0";
    NeutralAggregateManifest manifest;
    std::string error;
    const auto store = temporary / "store";
    assert( PublishNeutralStatistics( store, identity, "qscan-8", result, manifest, error ) );
    assert( manifest.state == NeutralAggregateState::Complete && manifest.completed );
    assert( manifest.runs.size() == 1 && manifest.runs[0].recordCount == result.signatures.size() );
    assert( VerifyNeutralAggregate( store, manifest, error ) );
    const auto loaded = LoadNeutralAggregateManifest( store, error );
    assert( loaded && loaded->qualityComplete && loaded->unreportedGapCount == 0 );

    // A source domain may be explicitly invalid while the scanner's own
    // transport/count/checksum audit is complete. This is reported evidence,
    // not an unreported scanner gap: publish the neutral aggregate so the
    // candidate policy can produce a P0 quality finding for the domain.
    auto sourceDegraded = input;
    sourceDegraded.temporaryRoot = temporary / "source-degraded";
    sourceDegraded.domainAudit[0].status = "invalid";
    sourceDegraded.domainAudit[0].qualityComplete = true;
    sourceDegraded.domainAudit[0].unavailableReason = "source_cpu_boundaries_incomplete";
    const auto sourceDegradedResult = BuildNeutralStatistics( sourceDegraded );
    assert( sourceDegradedResult.qualityComplete );
    assert( sourceDegradedResult.unreportedGapCount == 0 );
    assert( sourceDegradedResult.domains[0].status == "invalid" );
    assert( sourceDegradedResult.domains[0].qualityComplete );
    NeutralAggregateManifest sourceDegradedManifest;
    assert( PublishNeutralStatistics( temporary / "source-degraded-store", identity,
        "qscan-8-source-degraded", sourceDegradedResult, sourceDegradedManifest, error ) );

    auto invalid = input;
    invalid.temporaryRoot = temporary / "invalid";
    invalid.domainAudit[0].consumedInputCount--;
    const auto invalidResult = BuildNeutralStatistics( invalid );
    assert( !invalidResult.qualityComplete );
    assert( invalidResult.unreportedGapCount == 1 );
    NeutralAggregateManifest rejected;
    assert( !PublishNeutralStatistics( store, identity, "qscan-8-invalid", invalidResult,
        rejected, error ) );
    const auto preserved = LoadNeutralAggregateManifest( store, error );
    assert( preserved && preserved->generation == "qscan-8" );

    std::error_code ignored;
    std::filesystem::remove_all( temporary, ignored );
    return 0;
}
