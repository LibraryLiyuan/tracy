#pragma once
#include "../../../stream/src/TracyEventStore.hpp"
#include <sstream>
#include <type_traits>
namespace tracy
{
inline const char* AuxiliaryKindName( stream::StoreAuxiliaryKind kind )
{
    constexpr const char* names[] = { "Frame identity", "Gfx dispatch", "Gfx entity", "Gfx link", "Relation", "Runtime state" };
    return unsigned( kind ) < 6 ? names[unsigned( kind )] : "Unknown";
}
inline const char* EntityKindName( uint8_t kind )
{
    constexpr const char* names[] = { "Unknown", "Frame", "CPU Zone", "Job", "Gfx Entity", "GPU Pass", "GPU Segment", "GPU Taxonomy", "GPU Resource", "GPU Allocation", "I/O Request", "Script Zone", "Camera", "View", "Command List", "Submission" };
    return kind < std::size( names ) ? names[kind] : "Unknown kind";
}
inline std::string AuxiliaryDetails( const stream::StoreAuxiliaryRecord& record )
{
    using namespace stream;
    std::ostringstream out;
    out << AuxiliaryKindName( record.kind ) << " | event " << record.id << "\nTime (ns): " << record.time << "\nThread: " << record.thread << '\n';
    std::visit( [&]( const auto& p ) {
        using T = std::decay_t<decltype( p )>;
        if constexpr( std::is_same_v<T, StoreFrameIdentity> ) out << "Frame ID: " << p.frame << "\nDomain index: " << p.domainIndex << "\nDomain: " << unsigned( p.domain ) << "\nPhase: " << unsigned( p.phase );
        else if constexpr( std::is_same_v<T, StoreGfxDispatch> ) out << "Dispatch ID: " << p.dispatch << "\nRecorded frame: " << p.frame << "\nExpected jobs: " << p.expectedJobs << "\nThreading mode: " << unsigned( p.threadingMode );
        else if constexpr( std::is_same_v<T, StoreGfxEntity> ) out << "Entity ID: " << p.entity << "\nParent ID: " << p.parent << "\nGPU query: " << p.gpuQuery << "\nGPU context: " << unsigned( p.gpuContext ) << "\nEntity kind: " << unsigned( p.kind );
        else if constexpr( std::is_same_v<T, StoreGfxLink> ) out << "Source ID: " << p.source << "\nTarget ID: " << p.target << "\nRelation: " << unsigned( p.relation );
        else if constexpr( std::is_same_v<T, StoreRelation> ) out << "Source ID: " << p.source << " (" << EntityKindName( p.sourceKind ) << ", kind " << unsigned( p.sourceKind ) << ")\nTarget ID: " << p.target << " (" << EntityKindName( p.targetKind ) << ", kind " << unsigned( p.targetKind ) << ")\nNamespace: " << unsigned( p.nameSpace ) << "\nRelation: " << unsigned( p.relation );
        else if constexpr( std::is_same_v<T, StoreRuntimeState> ) out << "Generation: " << p.generation << "\nRequested frame: " << p.requestedFrame << "\nDomain: " << unsigned( p.domain ) << "\nRequested mode: " << unsigned( p.requestedMode ) << "\nEffective mode: " << unsigned( p.effectiveMode ) << "\nReason: " << unsigned( p.reason );
        out << "\nFlags: " << unsigned( p.flags );
    },
                record.data );
    return out.str();
}
}
