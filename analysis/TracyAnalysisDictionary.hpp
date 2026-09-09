#ifndef __TRACYANALYSISDICTIONARY_HPP__
#define __TRACYANALYSISDICTIONARY_HPP__
#include "TracyAnalysisWorkspaceBudget.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>
namespace tracy::analysis
{
// IDs are deterministic insertion ordinals within one dictionary generation.
// They are not a replacement for the externally visible strong signature ID.
// Exact byte comparisons resolve identity; no probabilistic deduplication.
struct AnalysisResolvedPath
{
    AnalysisWorkspaceReservation workspace;
    std::string text;
    AnalysisResolvedPath(std::shared_ptr<AnalysisWorkspaceBudget> budget,size_t bytes)
        :workspace(std::move(budget),64+uint64_t(bytes)*2),text(bytes,'\0') {}
    AnalysisResolvedPath(AnalysisResolvedPath&&)=default;
    AnalysisResolvedPath& operator=(AnalysisResolvedPath&&)=delete;
};
class AnalysisDictionary
{
public:
    using StringId=uint32_t;
    using PathId=uint32_t;
    explicit AnalysisDictionary(std::shared_ptr<AnalysisWorkspaceBudget> workspace={})
        :m_workspace(std::move(workspace)),m_stringsBudget(m_workspace),m_stringIndexBudget(m_workspace)
        ,m_pathsBudget(m_workspace),m_pathIndexBudget(m_workspace) {}
    AnalysisDictionary(const AnalysisDictionary&)=delete;
    AnalysisDictionary& operator=(const AnalysisDictionary&)=delete;
    StringId Intern(std::string_view text)
    {
        if(text.empty()) return 0;
        if(const auto found=m_strings.find(text);found!=m_strings.end()) return found->second;
        if(m_stringIndex.size()>=UINT32_MAX-1) throw std::runtime_error("analysis_dictionary_string_limit");
        if(text.size()>(UINT64_MAX-128)/2) throw std::runtime_error("analysis_workspace_budget");
        const auto previous=m_stringsBudget.Bytes();
        m_stringsBudget.Add(128+uint64_t(text.size())*2);
        try {
            Grow(m_stringIndex,m_stringIndexBudget);
            const auto id=StringId(m_stringIndex.size()+1);
            const auto inserted=m_strings.emplace(std::string(text),id).first;
            m_stringIndex.push_back(&inserted->first);
            return id;
        } catch(...) { m_stringsBudget.Resize(previous); throw; }
    }
    std::string_view Text(StringId id) const
    {
        if(id==0) return {};
        if(id>m_stringIndex.size()) throw std::runtime_error("analysis_dictionary_string_id");
        return *m_stringIndex[id-1];
    }
    PathId AppendPath(PathId parent,StringId segment)
    {
        const auto prefix=PathLength(parent);
        const auto text=Text(segment);
        const auto key=(uint64_t(parent)<<32)|segment;
        if(const auto found=m_paths.find(key);found!=m_paths.end()) return found->second;
        if(m_pathIndex.size()>=UINT32_MAX-1) throw std::runtime_error("analysis_dictionary_path_limit");
        const auto separator=prefix==0 ? 0ull : 3ull;
        if(text.size()>UINT64_MAX-separator || prefix>UINT64_MAX-separator-text.size())
            throw std::runtime_error("analysis_dictionary_path_size");
        const auto previous=m_pathsBudget.Bytes();
        m_pathsBudget.Add(128);
        try {
            Grow(m_pathIndex,m_pathIndexBudget);
            const auto id=PathId(m_pathIndex.size()+1);
            m_paths.emplace(key,id);
            m_pathIndex.push_back({parent,segment,prefix+separator+text.size()});
            return id;
        } catch(...) { m_pathsBudget.Resize(previous); throw; }
    }
    AnalysisResolvedPath ResolvePath(PathId id) const
    {
        const auto bytes=PathLength(id);
        if(bytes>std::numeric_limits<size_t>::max() || bytes>(UINT64_MAX-64)/2)
            throw std::runtime_error("analysis_dictionary_path_size");
        AnalysisResolvedPath result(m_workspace,size_t(bytes));
        auto end=size_t(bytes);
        // Fill backwards using stored lengths, without recursion or a second
        // ancestor vector. Even deeply nested paths need one output buffer.
        while(id!=0)
        {
            const auto& node=m_pathIndex[id-1];
            const auto segment=Text(node.segment);
            end-=segment.size();
            std::copy(segment.begin(),segment.end(),result.text.begin()+end);
            if(PathLength(node.parent)!=0) { end-=3; result.text.replace(end,3," > "); }
            id=node.parent;
        }
        return result;
    }
private:
    struct PathNode { PathId parent; StringId segment; uint64_t bytes; };
    uint64_t PathLength(PathId id) const
    {
        if(id==0) return 0;
        if(id>m_pathIndex.size()) throw std::runtime_error("analysis_dictionary_path_id");
        return m_pathIndex[id-1].bytes;
    }
    template<class T> static void Grow(std::vector<T>& values,AnalysisWorkspaceReservation& budget)
    {
        if(values.size()!=values.capacity()) return;
        const auto capacity=std::max<size_t>(64,values.capacity()*2);
        if(capacity>UINT64_MAX/(2*sizeof(T))) throw std::runtime_error("analysis_workspace_budget");
        // Includes the previous allocation during vector reallocation.
        budget.Resize(uint64_t(capacity)*2*sizeof(T));
        values.reserve(capacity);
    }
    std::shared_ptr<AnalysisWorkspaceBudget> m_workspace;
    AnalysisWorkspaceReservation m_stringsBudget,m_stringIndexBudget,m_pathsBudget,m_pathIndexBudget;
    std::map<std::string,StringId,std::less<>> m_strings;
    std::vector<const std::string*> m_stringIndex;
    std::map<uint64_t,PathId> m_paths;
    std::vector<PathNode> m_pathIndex;
};
}
#endif
