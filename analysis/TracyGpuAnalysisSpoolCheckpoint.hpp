#ifndef __TRACYGPUANALYSISSPOOLCHECKPOINT_HPP__
#define __TRACYGPUANALYSISSPOOLCHECKPOINT_HPP__

#include "TracyGpuAnalysisStore.hpp"

namespace tracy::analysis
{

bool SaveGpuCatalogSpoolCheckpoint( const GpuAnalysisCatalogSpool& spool,
    const GpuAnalysisTraceIdentity& identity, std::string& error );
bool LoadGpuCatalogSpoolCheckpoint( const std::filesystem::path& root,
    const GpuAnalysisTraceIdentity& identity, GpuAnalysisCatalogSpool& spool,
    std::string& error );

bool SaveGpuPassSpoolCheckpoint( const GpuAnalysisPassSpool& spool,
    const std::vector<GpuResourceAnalysisRecord>& appendedResources,
    const GpuAnalysisTraceIdentity& identity, std::string& error );
bool LoadGpuPassSpoolCheckpoint( const std::filesystem::path& root,
    const GpuAnalysisTraceIdentity& identity, GpuAnalysisPassSpool& spool,
    std::vector<GpuResourceAnalysisRecord>& appendedResources,
    std::string& error );

}

#endif
