#ifndef __TRACYFRAMEWINDOWPOLICY_HPP__
#define __TRACYFRAMEWINDOWPOLICY_HPP__

#include "TracyCandidatePolicy.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace tracy::analysis::frame_window
{
struct Signal
{
    size_t context = 0;
    std::string manifestation;
    std::string reason;
    int64_t valueNs = 0;
    std::vector<PolicyFrameEvidence> representatives;
    nlohmann::json evidence = nlohmann::json::object();
};

inline double Median( std::vector<int64_t> values )
{
    if( values.empty() ) return 0;
    std::sort( values.begin(), values.end() );
    const auto n = values.size();
    return n % 2 ? double( values[n/2] ) : double( values[n/2-1] ) / 2 + double( values[n/2] ) / 2;
}

inline double Mad( const std::vector<int64_t>& values, double median )
{
    std::vector<int64_t> deviations;
    for( const auto value : values ) deviations.push_back( int64_t( std::abs( double( value ) - median ) ) );
    return Median( std::move( deviations ) );
}

struct Window
{
    uint64_t begin = 0, end = 0;
    bool stable = false;
    std::set<uint64_t> scales;
};

inline std::vector<Window> Windows( const PolicyFrameTimeline& timeline,
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
using TopKey = std::tuple<std::string,std::string,std::string,std::string>;
struct TopItem
{
    int64_t cost;
    size_t context;
    uint64_t frame;
    std::string manifestation;
    nlohmann::json evidence;
    std::string signature;
    // Opaque join key used by a disk-backed caller; not part of Top comparison.
    std::string sourceKey;
    // Caller-owned accounting metadata, excluded from identity and comparison.
    uint64_t workspaceBytes = 0;
};
using SignalMap = std::map<std::pair<size_t,std::string>,Signal>;
struct TimelineState
{
    std::map<std::string,PolicyFrameTimeline> timelines;
    std::map<std::string,std::vector<Window>> windows;
};
template<class Visit>
TimelineState Prepare(const CandidatePolicyInput& input,Visit visit)
{
    const auto& config=input.normalizedProfile.at("candidate_policy");
    const auto budgetNs=input.normalizedProfile.at("frame_budget").at("frame_ms").get<double>()*1e6;
    TimelineState state; auto& timelines=state.timelines; auto& windows=state.windows;
    for(const auto& t:input.frameTimelines) timelines.emplace(t.frameScope,t);
    visit([&](const PolicySignatureContext& c) {
        if(c.frameRoot && c.frameSeriesComplete && !timelines.contains(c.frameScope))
            timelines.emplace(c.frameScope,PolicyFrameTimeline{c.frameScope,c.name,c.frames});
    });
    for( auto& [scope, t] : timelines )
    {
        std::sort( t.frames.begin(), t.frames.end(), []( const auto& a, const auto& b ) { return a.frameIndex < b.frameIndex; } );
        for( size_t i = 1; i < t.frames.size(); ++i )
            if( t.frames[i].frameIndex == t.frames[i-1].frameIndex ) throw std::runtime_error( "duplicate_frame_timeline" );
        windows.emplace( scope, Windows( t, config, budgetNs ) );
    }
    return state;
}
inline std::vector<Signal> Collect(SignalMap signals)
{
    std::vector<Signal> result;
    for(auto& [_,signal]:signals) result.push_back(std::move(signal));
    return result;
}
inline void InsertTop(std::vector<TopItem>& entries,TopItem item,uint64_t topN)
{
    entries.push_back(std::move(item));
    std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b) {
        if(a.cost!=b.cost) return a.cost>b.cost;
        return a.signature<b.signature;
    });
    if(entries.size()>topN) entries.resize(topN);
}
inline void ApplyTop(SignalMap& signals,const TopItem& item)
{
    const auto existing = signals.find( { item.context, item.manifestation } );
    if( existing == signals.end() )
    {
        Signal s; s.context=item.context; s.manifestation=item.manifestation;
        s.reason=item.manifestation.starts_with("stable_slow:")?"stable_slow_window":
            item.manifestation=="frame_cost"?"frame_local_top":"sustained_pressure_window";
        s.valueNs=item.cost; s.evidence=item.evidence;
        s.representatives.push_back({item.frame,item.cost,{},{},true});
        signals.emplace(std::make_pair(item.context,item.manifestation),std::move(s));
    }
    else if( item.cost > existing->second.valueNs )
    {
        existing->second.valueNs = item.cost;
        existing->second.representatives.push_back( { item.frame, item.cost, {}, {}, true } );
    }

}
template<class AcceptTop>
std::vector<Signal> ScanContext(const CandidatePolicyInput& input,size_t ci,const PolicySignatureContext& c,
    const TimelineState& state,AcceptTop acceptTop,
    const std::function<void(const std::vector<PolicyFrameEvidence>&)>& checkSeries = {})
{
    const auto& config = input.normalizedProfile.at( "candidate_policy" );
    const double budgetNs = input.normalizedProfile.at( "frame_budget" ).at( "frame_ms" ).get<double>() * 1e6;
    const double absoluteNs = config.value( "absolute_frame_cost_ms", 1.0 ) * 1e6;
    const double minimumNs = config.value( "local_min_cost_ms", 0.2 ) * 1e6;
    const double growthNs = config.value( "growth_absolute_ms", 0.5 ) * 1e6;
    const double growthRatio = config.value( "growth_ratio", 0.5 );
    const auto& timelines=state.timelines; const auto& windows=state.windows;
    SignalMap signals;
    const auto select = [&]( size_t ci, const std::string& manifestation, const std::string& reason,
        const PolicyFrameEvidence& frame, const nlohmann::json& evidence ) {
        auto& s = signals[{ci, manifestation}];
        s.context = ci; s.manifestation = manifestation; s.reason = reason;
        if( s.representatives.empty() || frame.valueNs > s.valueNs )
        { s.valueNs = frame.valueNs; s.evidence = evidence; }
        s.representatives.push_back( frame );
    };
    if( input.cancelled && input.cancelled() ) throw std::runtime_error( "cancelled" );
    if( !c.frameSeriesComplete ) return {};
    auto series = input.readFrameSeries ? input.readFrameSeries( c ) : c.frames;
    if(checkSeries) checkSeries(series);
    if( series.empty() ) return {};
    std::sort( series.begin(), series.end(), []( const auto& a, const auto& b ) { return a.frameIndex < b.frameIndex; } );
    for( size_t i = 1; i < series.size(); ++i )
        if( series[i].frameIndex == series[i-1].frameIndex ) throw std::runtime_error( "duplicate_signature_frame" );
    const auto at = [&]( uint64_t frame ) {
        auto it = std::lower_bound( series.begin(), series.end(), frame,
            []( const auto& x, uint64_t index ) { return x.frameIndex < index; } );
        return it != series.end() && it->frameIndex == frame ? *it : PolicyFrameEvidence { frame, 0, {}, {}, true };
    };
    const auto t = timelines.find( c.frameScope );
    if( c.frameRoot )
    {
        std::vector<PolicyFrameEvidence> bad;
        for( const auto& frame : series ) if( frame.exact && frame.valueNs > budgetNs ) bad.push_back( frame );
        if( !bad.empty() )
        {
            const auto peak = *std::max_element( bad.begin(), bad.end(), []( const auto& a, const auto& b ) { return a.valueNs < b.valueNs; } );
            select( ci, "frame_cost", "frame_budget_exceeded", peak,
                { { "over_budget_frames", bad.size() }, { "complete_frame_count", series.size() }, { "frame_budget_ns", int64_t( budgetNs ) } } );
            auto& reps = signals[{ci,"frame_cost"}].representatives;
            reps.push_back( bad.front() ); reps.push_back( bad[bad.size()/2] ); reps.push_back( bad.back() );
        }
        if( t != timelines.end() ) for( const auto& w : windows.at( c.frameScope ) )
        {
            const auto manifestation = std::string( w.stable ? "stable_slow:" : "sustained_pressure:" ) + std::to_string( w.begin ) + ":" + std::to_string( w.end );
            std::vector<PolicyFrameEvidence> part;
            for( const auto& frame : series ) if( frame.exact && frame.frameIndex >= w.begin && frame.frameIndex <= w.end ) part.push_back( frame );
            if( part.empty() ) continue;
            const auto peak = *std::max_element( part.begin(), part.end(), []( const auto& a, const auto& b ) { return a.valueNs < b.valueNs; } );
            select( ci, manifestation, w.stable ? "stable_slow_window" : "sustained_pressure_window", peak,
                { { "frame_begin", std::to_string( w.begin ) }, { "frame_end", std::to_string( w.end ) }, { "scales", w.scales } } );
            auto& reps = signals[{ci,manifestation}].representatives;
            reps.push_back( part.front() ); reps.push_back( part[part.size()/2] ); reps.push_back( part.back() );
        }
        return Collect(std::move(signals));
    }
    const auto top = [&]( const std::string& target, const std::string& manifestation,
        const PolicyFrameEvidence& frame, const nlohmann::json& evidence ) {
        if( double( frame.valueNs ) < minimumNs ) return;
        acceptTop(TopKey{c.frameScope, c.threadOrQueue, c.metricPreference, target},
            TopItem{frame.valueNs, ci, frame.frameIndex, manifestation, evidence, c.signatureId});
    };
    const auto reference = [&]( uint64_t begin, uint64_t end ) -> std::optional<PolicyFrameEvidence> {
        if( t == timelines.end() || c.observationUnit != "frame" ) return {};
        uint64_t distance = UINT64_MAX;
        std::optional<PolicyFrameEvidence> nearest;
        for( const auto& frame : t->second.frames )
        {
            if( !frame.exact || frame.valueNs > budgetNs || ( frame.frameIndex >= begin && frame.frameIndex <= end ) ) continue;
            const auto value = at( frame.frameIndex );
            if( !value.exact ) continue;
            const auto d = frame.frameIndex < begin ? begin - frame.frameIndex : frame.frameIndex - end;
            if( d < distance ) { distance = d; nearest = value; }
        }
        return nearest;
    };
    std::vector<PolicyFrameEvidence> qualified;
    std::set<std::string> reasons;
    for( const auto& frame : series )
    {
        if( !frame.exact ) continue;
        bool problem = c.observationUnit == "l0_segment";
        if( t != timelines.end() )
        {
            const auto it = std::lower_bound( t->second.frames.begin(), t->second.frames.end(), frame.frameIndex,
                []( const auto& f, uint64_t index ) { return f.frameIndex < index; } );
            problem = problem || ( it != t->second.frames.end() && it->frameIndex == frame.frameIndex && it->exact && it->valueNs > budgetNs );
        }
        if( problem ) top( "frame:" + std::to_string( frame.frameIndex ), "frame_cost", frame, {} );
        if( double( frame.valueNs ) >= absoluteNs ) { qualified.push_back( frame ); reasons.emplace( "absolute_frame_cost" ); }
        else if( problem && double( frame.valueNs ) >= growthNs )
        {
            const auto normal = reference( frame.frameIndex, frame.frameIndex );
            if( normal && normal->valueNs > 0 && double( frame.valueNs - normal->valueNs ) >= growthNs &&
                double( frame.valueNs - normal->valueNs ) / normal->valueNs >= growthRatio )
            { qualified.push_back( frame ); reasons.emplace( "relative_growth" ); }
        }
    }
    if( !qualified.empty() )
    {
        const auto peak = *std::max_element( qualified.begin(), qualified.end(), []( const auto& a, const auto& b ) { return a.valueNs < b.valueNs; } );
        nlohmann::json e = { { "qualified_frame_count", qualified.size() }, { "entry_reasons", reasons }, { "comparison", "same_frameset_observational_not_controlled_ab" } };
        e["observation_unit"] = c.observationUnit;
        if( c.observationUnit != "frame" )
        { e.erase( "qualified_frame_count" ); e["qualified_segment_count"] = qualified.size(); e["comparison"] = "physical_l0_segments_not_player_frames"; }
        nlohmann::json ranges = nlohmann::json::array();
        uint64_t begin = qualified.front().frameIndex, end = begin;
        for( size_t i = 1; i < qualified.size(); ++i )
            if( qualified[i].frameIndex == end + 1 ) end = qualified[i].frameIndex;
            else { ranges.push_back( { std::to_string( begin ), std::to_string( end ) } ); begin = end = qualified[i].frameIndex; }
        ranges.push_back( { std::to_string( begin ), std::to_string( end ) } );
        e[c.observationUnit == "frame" ? "affected_frame_ranges" : "affected_segment_ranges"] = std::move( ranges );
        select( ci, "frame_cost", "absolute_or_relative_frame_cost", peak, e );
        auto typical = qualified; std::sort( typical.begin(), typical.end(), []( const auto& a, const auto& b ) { return a.valueNs < b.valueNs; } );
        signals[{ci, "frame_cost"}].representatives.push_back( typical[typical.size()/2] );
        if( const auto normal = reference( peak.frameIndex, peak.frameIndex ) )
            signals[{ci, "frame_cost"}].representatives.push_back( *normal );
    }
    if( t == timelines.end() ) return Collect(std::move(signals));
    for( const auto& window : windows.at( c.frameScope ) )
    {
        std::vector<PolicyFrameEvidence> values;
        std::vector<int64_t> costs;
        for( const auto& frame : t->second.frames ) if( frame.frameIndex >= window.begin && frame.frameIndex <= window.end && frame.exact )
        { auto v = at( frame.frameIndex ); if( v.exact ) { values.push_back( v ); costs.push_back( v.valueNs ); } }
        if( values.empty() || double( values.size() ) / double( window.end-window.begin+1 ) < config.value( "window_valid_ratio", 0.8 ) ) continue;
        const auto median = Median( costs );
        const auto peak = *std::max_element( values.begin(), values.end(), []( const auto& a, const auto& b ) { return a.valueNs < b.valueNs; } );
        const auto manifestation = std::string( window.stable ? "stable_slow:" : "sustained_pressure:" ) + std::to_string( window.begin ) + ":" + std::to_string( window.end );
        nlohmann::json e = { { "frame_begin", std::to_string( window.begin ) }, { "frame_end", std::to_string( window.end ) },
            { "scales", window.scales }, { "valid_frames", values.size() }, { "event_median_ns", std::to_string( int64_t( median ) ) },
            { "comparison", "same_frameset_observational_not_controlled_ab" } };
        const auto normal = reference( window.begin, window.end );
        if( normal ) { e["reference_frame"] = std::to_string( normal->frameIndex ); e["reference_value_ns"] = std::to_string( normal->valueNs ); }
        else e["reference_unavailable_reason"] = "no_valid_lower_load_same_frameset_reference";
        top( manifestation, manifestation, { peak.frameIndex, int64_t( median ), {}, {}, true }, e );
        const bool growth = normal && normal->valueNs > 0 && median - normal->valueNs >= growthNs && ( median - normal->valueNs ) / normal->valueNs >= growthRatio;
        if( median >= absoluteNs || growth )
        {
            select( ci, manifestation, window.stable ? "stable_slow_window" : "sustained_pressure_window", peak, e );
            auto& reps = signals[{ci, manifestation}].representatives;
            reps.push_back( values.front() ); reps.push_back( values[values.size()/2] ); reps.push_back( values.back() );
            if( normal ) reps.push_back( *normal );
        }
    }
    return Collect(std::move(signals));
}
inline std::vector<Signal> Scan(const CandidatePolicyInput& input)
{
    const auto state=Prepare(input,[&](const auto& visit){for(const auto& c:input.signatures) visit(c);});
    const auto topN=input.normalizedProfile.at("candidate_policy").value("local_top_n",uint64_t(5));
    std::map<TopKey,std::vector<TopItem>> tops;
    SignalMap signals;
    for(size_t ci=0;ci<input.signatures.size();++ci)
    {
        auto rows=ScanContext(input,ci,input.signatures[ci],state,[&](const TopKey& key,TopItem item) {
            InsertTop(tops[key],std::move(item),topN);
        });
        for(auto& signal:rows) signals.emplace(std::make_pair(signal.context,signal.manifestation),std::move(signal));
    }
    for(const auto& [_,entries]:tops) for(const auto& item:entries) ApplyTop(signals,item);
    return Collect(std::move(signals));
}
}
#endif
