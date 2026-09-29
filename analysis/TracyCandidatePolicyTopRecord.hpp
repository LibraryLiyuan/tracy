#ifndef __TRACYCANDIDATEPOLICYTOPRECORD_HPP__
#define __TRACYCANDIDATEPOLICYTOPRECORD_HPP__

#include "TracyFrameWindowPolicy.hpp"
#include "TracyAnalysisCacheKey.hpp"
#include <bit>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tracy::analysis::candidate_policy::top_record
{
// Build-only spool payload. The sorted table key already carries the group;
// the final Candidate cache and its public JSON format are unchanged.
inline void AppendNumber(std::string& output,uint64_t value)
{ output+=cache_key::Unsigned(value); }

inline void AppendText(std::string& output,std::string_view value)
{
    AppendNumber(output,value.size());
    output.append(value.data(),value.size());
}

inline std::string Encode(const frame_window::TopItem& item)
{
    std::string output;
    output.reserve(64+item.manifestation.size()+item.signature.size()+item.sourceKey.size());
    AppendNumber(output,1);
    AppendNumber(output,std::bit_cast<uint64_t>(item.cost));
    AppendNumber(output,item.context);
    AppendNumber(output,item.frame);
    AppendText(output,item.manifestation);
    AppendText(output,item.evidence.is_object() && item.evidence.empty() ?
        std::string_view{} : std::string_view(item.evidence.dump()));
    AppendText(output,item.signature);
    AppendText(output,item.sourceKey);
    return output;
}

inline frame_window::TopItem Decode(std::string_view input)
{
    size_t position=0;
    const auto fail=[]() -> void {throw std::runtime_error("candidate_policy_top_input_format");};
    const auto number=[&]() -> uint64_t {
        if(input.size()-position<8) {fail();return 0;}
        uint64_t value=0;
        for(size_t i=0;i<8;++i) value=(value<<8)|uint8_t(input[position++]);
        return value;
    };
    const auto text=[&]() -> std::string {
        const auto size=number();
        if(size>input.size()-position) {fail();return {};}
        auto result=std::string(input.substr(position,size_t(size)));
        position+=size_t(size);
        return result;
    };
    if(number()!=1) fail();
    const auto cost=std::bit_cast<int64_t>(number());
    const auto context=number();
    if(context>std::numeric_limits<size_t>::max()) fail();
    const auto frame=number();
    auto manifestation=text();
    auto encodedEvidence=text();
    auto signature=text();
    auto sourceKey=text();
    if(position!=input.size()) fail();
    auto evidence=encodedEvidence.empty()?nlohmann::json::object():nlohmann::json::parse(encodedEvidence);
    frame_window::TopItem result{cost,size_t(context),frame,std::move(manifestation),
        std::move(evidence),std::move(signature)};
    result.sourceKey=std::move(sourceKey);
    return result;
}
}
#endif
