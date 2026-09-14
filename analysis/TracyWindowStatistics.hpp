#ifndef __TRACYWINDOWSTATISTICS_HPP__
#define __TRACYWINDOWSTATISTICS_HPP__

#include "TracyCandidatePolicy.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace tracy::analysis::frame_window
{
// Exact, coordinate-compressed order statistics. Storage is linear in the
// timeline; sliding/merged windows reuse it instead of sorting growing arrays.
struct WindowWork { uint64_t updates = 0, probes = 0, queries = 0; };
class WindowStatistics
{
public:
    WindowStatistics(const std::vector<PolicyFrameEvidence>& frames, double budget,
        const std::function<bool()>& cancelled, WindowWork* work)
        : m_frames(frames), m_budget(budget), m_cancelled(cancelled), m_work(work)
    {
        for(const auto& frame:frames) { Check(); if(frame.exact) m_values.push_back(frame.valueNs); }
        std::sort(m_values.begin(),m_values.end(),[&](int64_t a,int64_t b){Check();return a<b;});
        m_values.erase(std::unique(m_values.begin(),m_values.end()),m_values.end());
        m_counts.resize(m_values.size()+1);
    }
    void Range(size_t begin,size_t end)
    {
        // Expand before shrinking so disjoint windows never remove absent rows.
        while(m_begin>begin) Add(--m_begin,1);
        while(m_end<end) Add(m_end++,1);
        while(m_begin<begin) Add(m_begin++,-1);
        while(m_end>end) Add(--m_end,-1);
    }
    uint64_t Count() const {return m_count;}
    uint64_t Over() const {return m_over;}
    double Median() const
    {
        if(m_work) ++m_work->queries;
        return m_count%2?double(Kth(m_count/2)):double(Kth(m_count/2-1))/2+double(Kth(m_count/2))/2;
    }
    double Mad(double median) const
    {
        if(m_values.empty() || m_count==0)return 0;
        const auto deviation=[&](int64_t value){return uint64_t(std::abs(double(value)-median));};
        const auto kth=[&](uint64_t rank) {
            uint64_t lo=0,hi=std::max(deviation(m_values.front()),deviation(m_values.back()));
            while(lo<hi)
            {
                Check(); if(m_work) ++m_work->probes;
                const auto mid=lo+(hi-lo)/2;
                // Evaluate the original integer-truncated deviation, including
                // half-integer medians, instead of approximating the MAD.
                auto first=std::partition_point(m_values.begin(),m_values.end(),[&](int64_t v){
                    return double(v)<median && deviation(v)>mid;});
                auto last=std::partition_point(m_values.begin(),m_values.end(),[&](int64_t v){
                    return double(v)<=median || deviation(v)<=mid;});
                if(Prefix(size_t(last-m_values.begin()))-Prefix(size_t(first-m_values.begin()))>rank)hi=mid;
                else lo=mid+1;
            }
            return lo;
        };
        return m_count%2?double(kth(m_count/2)):double(kth(m_count/2-1))/2+double(kth(m_count/2))/2;
    }
private:
    void Check() const
    {
        if((++m_checks & 255)==0 && m_cancelled && m_cancelled())throw std::runtime_error("cancelled");
    }
    void Add(size_t index,int delta)
    {
        Check(); if(m_work) ++m_work->updates;
        const auto& frame=m_frames[index]; if(!frame.exact)return;
        m_count+=delta; if(double(frame.valueNs)>m_budget)m_over+=delta;
        auto i=size_t(std::lower_bound(m_values.begin(),m_values.end(),frame.valueNs)-m_values.begin())+1;
        for(;i<m_counts.size();i+=i&(~i+1))m_counts[i]+=delta;
    }
    uint64_t Prefix(size_t i) const
    {
        uint64_t count=0; for(;i;i-=i&(~i+1))count+=m_counts[i]; return count;
    }
    int64_t Kth(uint64_t rank) const
    {
        size_t index=0,step=1;while(step<m_counts.size()/2)step*=2;
        for(;step;step/=2)if(index+step<m_counts.size() && m_counts[index+step]<=rank)
        {index+=step;rank-=m_counts[index];}
        return m_values.at(index);
    }
    const std::vector<PolicyFrameEvidence>& m_frames;
    double m_budget;
    const std::function<bool()>& m_cancelled;
    WindowWork* m_work;
    mutable uint64_t m_checks=0;
    std::vector<int64_t> m_values;
    std::vector<uint64_t> m_counts;
    size_t m_begin=0,m_end=0;
    uint64_t m_count=0,m_over=0;
};
}
#endif
