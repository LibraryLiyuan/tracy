#include "TracyFrameWindowPolicy.hpp"
#include <iostream>
#include <random>
using namespace tracy::analysis;
using namespace tracy::analysis::frame_window;
namespace {
inline std::vector<Window> ReferenceWindows( const PolicyFrameTimeline& timeline,
    const nlohmann::json& config, double budgetNs )
{
    std::vector<Window> result;
    const auto& frames = timeline.frames;
    if( frames.empty() || timeline.observationUnit != "frame" ) return result;
    const auto coverage = config.value( "window_valid_ratio", 0.8 );
    const auto overRatio = config.value( "window_over_budget_ratio", 0.8 );
    const auto stableRatio = config.value( "window_stable_mad_ratio", 0.1 );
    const auto scales = config.value( "window_sizes", std::vector<uint64_t> { 30, 60, 120 } );
    for( const auto length : scales ) for( size_t start = 0; start < frames.size(); ++start )
    {
        const auto begin = frames[start].frameIndex;
        if( length == 0 || frames.back().frameIndex - begin < length - 1 ) break;
        const auto end = begin + length - 1;
        std::vector<int64_t> values;
        size_t over = 0;
        for( size_t i = start; i < frames.size() && frames[i].frameIndex <= end; ++i )
            if( frames[i].exact )
            { values.push_back( frames[i].valueNs ); over += double( frames[i].valueNs ) > budgetNs; }
        if( values.empty() || double( values.size() ) / length < coverage ||
            double( over ) / values.size() < overRatio ) continue;
        const auto median = Median( values );
        if( median <= budgetNs ) continue;
        result.push_back( { begin, end, Mad( values, median ) / median <= stableRatio, { length } } );
    }
    std::sort( result.begin(), result.end(), []( const auto& a, const auto& b ) {
        return std::tie( a.stable, a.begin, a.end ) < std::tie( b.stable, b.begin, b.end );
    } );
    std::vector<Window> merged;
    const auto mergedStillMatches = [&](const Window& left, const Window& right) {
        const auto end = std::max(left.end, right.end);
        std::vector<int64_t> values;
        size_t over = 0;
        for(const auto& frame : frames) if(frame.frameIndex >= left.begin && frame.frameIndex <= end && frame.exact)
        { values.push_back(frame.valueNs); over += double(frame.valueNs) > budgetNs; }
        if(values.empty() || double(values.size()) / double(end-left.begin+1) < coverage ||
            double(over) / values.size() < overRatio) return false;
        const auto median = Median(values);
        if(median <= budgetNs) return false;
        return (Mad(values,median) / median <= stableRatio) == left.stable;
    };
    for( const auto& window : result )
    {
        if( !merged.empty() && merged.back().stable == window.stable &&
            window.begin <= merged.back().end && mergedStillMatches(merged.back(), window) )
        {
            merged.back().end = std::max( merged.back().end, window.end );
            merged.back().scales.insert( window.scales.begin(), window.scales.end() );
        }
        else merged.push_back( window );
    }
    return merged;
}

// Shared per-context scanner. Whole-capture and disk-backed callers must use
// the same local/window math and the same cross-signature Top reducer.
}
int main() {
    CandidatePolicyInput input;
    input.normalizedProfile={{"frame_budget",{{"frame_ms",16.666667}}},{"candidate_policy",nlohmann::json::object()}};
    PolicyFrameTimeline timeline; timeline.frameScope="Player.Frame"; timeline.observationUnit="frame";
    for(uint64_t i=0;i<4000;++i) timeline.frames.push_back({i,24000000,{},{},true});
    input.frameTimelines.push_back(timeline); size_t checks=0;
    input.cancelled=[&]{return ++checks>=4;};
    bool cancelled=false;
    try { Prepare(input,[](const auto&){}); } catch(const std::runtime_error& e) { cancelled=std::string(e.what())=="cancelled"; }
    if(!cancelled) {std::cerr<<"window preparation must respond to cancellation inside its work\n";return 1;}
    std::mt19937 random(42);
    for(int scenario=0;scenario<80;++scenario) {
        PolicyFrameTimeline t; t.frameScope="Player.Frame"; t.observationUnit="frame";
        for(uint64_t i=0;i<240;++i) {
            if(scenario%3==0 && random()%7==0)continue;
            int64_t value=scenario%4==0?24000000:(scenario%4==1?(i<120?24000000:50000000):16000000+random()%30000000);
            t.frames.push_back({i,value,{},{},random()%15!=0});
        }
        auto config=nlohmann::json{{"window_sizes",{5,17,30,60,120}}};
        const auto expected=ReferenceWindows(t,config,16666667),actual=Windows(t,config,16666667);
        if(actual.size()!=expected.size()) {std::cerr<<"window count changed scenario "<<scenario;return 2;}
        for(size_t i=0;i<actual.size();++i) if(std::tie(actual[i].begin,actual[i].end,actual[i].stable,actual[i].scales)!=
            std::tie(expected[i].begin,expected[i].end,expected[i].stable,expected[i].scales)) {std::cerr<<"window semantics changed";return 3;}
    }
    uint64_t previousWork=0;
    for(uint64_t count:{1000,2000,4000,8000}) {
        PolicyFrameTimeline t;t.frameScope="Player.Frame";t.observationUnit="frame";
        for(uint64_t i=0;i<count;++i)t.frames.push_back({i,24000000+int64_t(i%3),{},{},true});
        WindowWork work;
        auto windows=Windows(t,nlohmann::json::object(),16666667,{},&work);
        const auto operations=work.updates+work.probes+work.queries;
        std::cout<<"frames="<<count<<" statistics_operations="<<operations<<" windows="<<windows.size()<<"\n";
        if(windows.size()!=1 || (previousWork && operations>previousWork*5/2)) {
            std::cerr<<"doubling stable sequence must not cause quadratic statistics work";return 4;
        }
        previousWork=operations;
        size_t polls=0;bool stopped=false;
        try {Windows(t,nlohmann::json::object(),16666667,[&]{return ++polls>=200;});}
        catch(const std::runtime_error& e) {stopped=std::string(e.what())=="cancelled";}
        if(!stopped) {std::cerr<<"window work must observe cancellation";return 5;}
    }
    std::cout<<"Window cancellation, scaling and reference-equivalence tests passed\n";
}
