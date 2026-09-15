#ifndef TRACY_CONVERSION_FILES_HPP
#define TRACY_CONVERSION_FILES_HPP
#include "../../storage/TracyConversionResources.hpp"
#include "../../analysis/TracyAnalysisIoPath.hpp"
#include <fstream>
#include <set>
namespace tracy::conversion {
// All metadata replacements reserve the simultaneous old and new files.
// The caller must explicitly identify the one pre-existing stream mapping.
class ConversionFiles {
public:
 explicit ConversionFiles(storage::ConversionResources& resources):m_resources(resources){}
 void WriteText(const std::filesystem::path& path,const std::string& text,bool replaceHistoricalMap=false) {
    std::lock_guard lock(m_lock);
    const auto target=std::filesystem::absolute(path).lexically_normal();
    if(text.size()>65536)throw std::runtime_error("conversion_metadata_size");
    if(!m_written.contains(target)&&std::filesystem::exists(analysis::AnalysisIoPath(target))&&!replaceHistoricalMap)
        throw std::runtime_error("conversion_metadata_already_exists");
    auto temporary=target;temporary+=".tmp";
    if(std::filesystem::exists(analysis::AnalysisIoPath(temporary)))throw std::runtime_error("conversion_metadata_temporary_exists");
    m_resources.BeforeControlGrowth(temporary,text.size());
    try {
        std::ofstream out(analysis::AnalysisIoPath(temporary),std::ios::binary|std::ios::trunc);
        if(!out)throw std::runtime_error("conversion_metadata_open");
        out.write(text.data(),std::streamsize(text.size()));out.flush();out.close();
        if(!out)throw std::runtime_error("conversion_metadata_write");
        m_resources.ReplaceOwned(temporary,target,replaceHistoricalMap);
        m_resources.KeepOwned(target);m_written.insert(target);
    }catch(...){m_resources.RemoveOwned(temporary);throw;}
 }
private:
 storage::ConversionResources& m_resources;std::mutex m_lock;std::set<std::filesystem::path> m_written;
};
}
#endif
