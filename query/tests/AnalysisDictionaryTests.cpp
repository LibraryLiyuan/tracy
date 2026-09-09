#include <iostream>
#include <stdexcept>
#include "TracyAnalysisDictionary.hpp"
#include <string>
using namespace tracy::analysis;
namespace {
void Require(bool value,const char* why) { if(!value) throw std::runtime_error(why); }
template<class F> void Reject(F&& f,const char* reason)
{
    try { f(); } catch(const std::exception& e) {
        if(std::string(e.what()).find(reason)!=std::string::npos) return;
        throw;
    }
    throw std::runtime_error(std::string("expected rejection: ")+reason);
}
void ExactStringsAndPaths()
{
    auto workspace=std::make_shared<AnalysisWorkspaceBudget>(4*1024*1024,2*1024*1024);
    {
        AnalysisDictionary dictionary(workspace);
        const auto rootName=dictionary.Intern("Root");
        const auto root=dictionary.AppendPath(0,rootName);
        const auto childName=dictionary.Intern("Child");
        const auto child=dictionary.AppendPath(root,childName);
        Require(dictionary.Intern("Root")==rootName,"identical text has one exact identity");
        Require(dictionary.AppendPath(root,childName)==child,"duplicate edges reuse the same path");
        Require(dictionary.ResolvePath(child).text=="Root > Child","complete ancestry is reconstructed");
        const auto another=dictionary.AppendPath(0,childName);
        Require(another!=child && dictionary.ResolvePath(another).text=="Child","same leaf under different parents must not merge");
        const std::string binary("a\0b",3);
        const auto binaryId=dictionary.Intern(binary);
        Require(dictionary.Text(binaryId)==std::string_view(binary),"dictionary preserves embedded NUL bytes");
        Require(dictionary.Intern("a")!=binaryId,"dictionary compares exact lengths as well as bytes");
        const auto stable=dictionary.Text(rootName);
        for(int i=0;i<1000;++i) dictionary.Intern("unique-"+std::to_string(i));
        Require(stable=="Root" && dictionary.Text(rootName).data()==stable.data(),"views survive index growth");
        Reject([&]{dictionary.Text(UINT32_MAX);},"dictionary_string_id");
        Reject([&]{dictionary.AppendPath(UINT32_MAX,rootName);},"dictionary_path_id");
        Reject([&]{dictionary.AppendPath(root,UINT32_MAX);},"dictionary_string_id");
        Reject([&]{dictionary.ResolvePath(UINT32_MAX);},"dictionary_path_id");
        const auto empty=dictionary.Intern("");
        const auto emptyPath=dictionary.AppendPath(0,empty);
        Require(dictionary.ResolvePath(dictionary.AppendPath(emptyPath,childName)).text=="Child",
            "empty parent display paths do not invent a leading separator");
    }
    Require(workspace->Snapshot().currentBytes==0,"dictionary and resolved paths return all owned budget");
}
void PrefixSharingAndBudget()
{
    auto workspace=std::make_shared<AnalysisWorkspaceBudget>(2*1024*1024,1024*1024);
    {
        AnalysisDictionary dictionary(workspace);
        auto path=dictionary.AppendPath(0,dictionary.Intern(std::string(32768,'x')));
        const auto leaf=dictionary.Intern("leaf");
        for(int i=0;i<4096;++i) path=dictionary.AppendPath(path,leaf);
        // Expanded prefixes would retain over 180 MiB. Exact parent/segment
        // sharing must allow this chain under the 2 MiB analysis budget.
        auto resolved=dictionary.ResolvePath(path);
        Require(resolved.text.size()==61440,"deep full path is not truncated");
        Require(resolved.text.substr(32768,14)==" > leaf > leaf","deep path preserves segment order");
        Require(resolved.text.substr(resolved.text.size()-7)==" > leaf","last segment is preserved");
        const auto charged=workspace->Snapshot().currentBytes;
        AnalysisWorkspaceReservation pressure(workspace,2*1024*1024-charged-1);
        Require(dictionary.Intern("leaf")==leaf,"duplicate lookup succeeds without allocating at the budget limit");
        const auto before=workspace->Snapshot().currentBytes;
        Reject([&]{dictionary.Intern("new-text");},"workspace_budget");
        Reject([&]{dictionary.AppendPath(path,leaf);},"workspace_budget");
        Reject([&]{dictionary.ResolvePath(path);},"workspace_budget");
        Require(workspace->Snapshot().currentBytes==before,"denied growth leaves prior ownership intact");
        pressure.Resize(0);
        Require(dictionary.Text(leaf)=="leaf" && dictionary.ResolvePath(path).text==resolved.text,
            "dictionary remains usable after a denied reservation");
    }
    Require(workspace->Snapshot().currentBytes==0,"prefix dictionary releases all reservations");
}
}
int main()
{
    try {
        ExactStringsAndPaths(); PrefixSharingAndBudget();
        std::cout<<"analysis dictionary tests passed\n"; return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
