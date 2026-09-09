#ifndef __TRACYCANDIDATEPOLICYINTERNAL_HPP__
#define __TRACYCANDIDATEPOLICYINTERNAL_HPP__
#include "TracyCandidatePolicy.hpp"
#include <limits>
#include <map>
namespace tracy::analysis::frame_window { struct Signal; }
namespace tracy::analysis::candidate_policy
{
struct FamilyWork
{
    CandidateFamily candidate;
    bool policyEligible = false;
    bool mandatory = false;
    uint64_t bestTopRank = std::numeric_limits<uint64_t>::max();
    std::map<uint64_t, CandidateRepresentativeFrame> representatives;
    // Write masks preserve the order-sensitive legacy display-field semantics.
    bool contextWritten = false;
    bool localWritten = false;
};
using Families = std::map<std::string,FamilyWork>;
bool ValidateInput(const CandidatePolicyInput&,std::string&);
std::string Sha256Text(const std::string&);
void Local(Families&,const PolicySignatureContext&,const frame_window::Signal&);
void Budget(Families&,const CandidatePolicyInput&,const NeutralSignatureAggregate*,const PolicySignatureContext&,
    const std::function<void(uint64_t)>& beforeSignal={});
PolicyRankedSignature Top(Families&,const CandidatePolicyInput&,const std::string&,const char*,
    const NeutralRankingEntry&,uint64_t,double,const PolicySignatureContext*,const NeutralSignatureAggregate*);
void Anomalies(Families&,const NeutralSignatureAggregate&,const PolicySignatureContext*);
void Capacity(Families&,const CandidatePolicyInput&,const PolicyCapacityFact&);
void Focus(Families&,const nlohmann::json&,const PolicySignatureContext&,const NeutralSignatureAggregate*);
void Finalize(FamilyWork&,const std::string&,const std::string&);
void Select(FamilyWork&,const CandidatePolicyInput&,std::map<std::string,uint64_t>&);
nlohmann::json CandidateJson(const CandidateFamily&);
nlohmann::json RankedJson(const PolicyRankedSignature&);
}
#endif
