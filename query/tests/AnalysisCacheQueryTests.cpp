#include "TracyAnalysisCacheQuery.hpp"
#include "TracyAnalysisCacheKey.hpp"
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
using namespace tracy::analysis;
using nlohmann::json;
namespace
{
void Require(bool value,const char* reason) { if(!value) throw std::runtime_error(reason); }
template<class F> void Reject(F&& f,const char* message)
{
    try { f(); } catch(const std::exception& e) {
        if(std::string(e.what()).find(message)!=std::string::npos) return;
        throw;
    }
    throw std::runtime_error(std::string("expected rejection: ")+message);
}
void Queries(const std::filesystem::path& root)
{
    AnalysisCacheTableOptions options; options.blockBytes=4096;
    const std::string identity(64,'a');
    AnalysisCacheTableWriter writer(root/"source",identity,"source",options);
    for(uint64_t i=0;i<100;++i) writer.Append(cache_key::Unsigned(i),json{
        {"ordinal",std::to_string(i)},{"domain",i%2==0?"cpu":"gpu"},{"selected",i%3==0},
        {"payload",std::string(180,'x')}}.dump());
    writer.Commit();
    AnalysisCacheTableReader source(root/"source",identity,"source",options);
    size_t reads=0;
    auto read=[&](uint64_t i) { ++reads; return source.GetAt(i); };
    AnalysisCacheQuery query(root/"filters",identity,100,read,options,1024);
    const auto direct=query.Page(2,"90",{"ordinal"},json::object());
    Require(reads==2,"unfiltered page must read only requested source rows");
    Require(direct.at("items")==json::array({{{"ordinal","90"}},{{"ordinal","91"}}}),"direct page/projection");
    Require(direct.at("page").at("total")=="100" && direct.at("page").at("next_cursor")=="92","direct cursor/count");
    const json filter={{"domain","cpu"},{"selected",true}};
    reads=0;
    const auto first=query.Page(2,"2",{"ordinal"},filter);
    Require(reads==102,"first filter builds one streaming index then reads one page");
    Require(first.at("items")==json::array({{{"ordinal","12"}},{{"ordinal","18"}}}),"filtered ordinal is match ordinal");
    Require(first.at("page").at("total")=="17" && first.at("page").at("next_cursor")=="4","filtered total/cursor");
    reads=0;
    const auto next=query.Page(2,"4",{"ordinal"},filter);
    Require(reads==2,"next filtered page must not rescan source table");
    Require(next.at("items")==json::array({{{"ordinal","24"}},{{"ordinal","30"}}}),"filtered next values");
    reads=0;
    AnalysisCacheQuery reopened(root/"filters",identity,100,read,options,1024);
    const auto last=reopened.Page(2,"16",{"ordinal"},filter);
    Require(reads==1 && last.at("items")[0].at("ordinal")=="96","cold filter reopen uses persisted ordinal index");
    Require(last.at("page").at("done")==true && last.at("page").at("next_cursor").is_null(),"last filtered page terminates");
    const auto absent=query.Page(2,"",{},{{"missing",nullptr}});
    Require(absent.at("items").empty() && absent.at("page").at("total")=="0","missing key is not equal to null");
    size_t observed=0; std::string cursor; bool limited=false;
    do {
        const auto page=query.Page(1000,cursor,{},json::object());
        observed+=page.at("items").size(); limited=limited || page.at("page").at("byte_limited").get<bool>();
        Require(page.at("items").dump().size()<=1024,"projected response is byte bounded");
        Require(!page.at("items").empty(),"bounded pages make progress");
        if(page.at("page").at("done").get<bool>()) break;
        cursor=page.at("page").at("next_cursor").get<std::string>();
    } while(true);
    Require(observed==100 && limited,"byte-limited paging preserves all rows");
    for(const char* bad:{"-1","+1","1x","18446744073709551616"})
        Reject([&]{query.Page(1,bad,{},json::object());},"invalid_cursor");
    Reject([&]{query.Page(0,"",{},json::object());},"invalid_page");
    Reject([&]{query.Page(1001,"",{},json::object());},"invalid_page");
    const auto beyond=query.Page(1,"200",{},json::object());
    Require(beyond.at("items").empty() && beyond.at("page").at("done")==true,"cursor beyond end is done");
    AnalysisCacheQuery tooSmall(root/"small-filters",identity,100,read,options,32);
    Reject([&]{tooSmall.Page(1,"",{},json::object());},"page_item_too_large");

    // An interrupted build must not make this filter permanently unqueryable,
    // nor may its first few matches be mistaken for a complete result.
    reads=0; bool interrupt=true;
    auto cancellable=options;
    cancellable.cancelled=[&]{return interrupt && reads>=9;};
    AnalysisCacheQuery interrupted(root/"interrupted-filters",identity,100,read,cancellable,1024);
    Reject([&]{interrupted.Page(3,"",{"ordinal"},filter);},"cancelled");
    Require(reads==9,"cancellation fixture reached a partially built index");
    interrupt=false; reads=0;
    const auto restarted=interrupted.Page(3,"",{"ordinal"},filter);
    Require(reads==103,"incomplete filter must rebuild all source rows once");
    Require(restarted.at("items")==json::array({{{"ordinal","0"}},{{"ordinal","6"}},{{"ordinal","12"}}}),
        "retry must not skip or duplicate interrupted matches");
    Require(restarted.at("page").at("total")=="17","retry total includes all matches");
    reads=0;
    AnalysisCacheQuery recovered(root/"interrupted-filters",identity,100,read,options,1024);
    const auto recoveredLast=recovered.Page(2,"16",{"ordinal"},filter);
    Require(reads==1 && recoveredLast.at("items")==json::array({{{"ordinal","96"}}}),
        "restarted filter is complete and cold-reopenable");

    // Hold one real builder inside its first source read, then let a second
    // independent reader publish the same filter. Both must return all rows.
    const json concurrentFilter={{"domain","gpu"}};
    std::promise<void> reached,release;
    auto reachedFuture=reached.get_future(); auto releaseFuture=release.get_future();
    auto slower=std::async(std::launch::async,[&] {
        AnalysisCacheTableReader ownSource(root/"source",identity,"source",options);
        bool firstRead=true;
        AnalysisCacheQuery ownQuery(root/"concurrent-filters",identity,100,[&](uint64_t i) {
            if(firstRead) { firstRead=false; reached.set_value(); releaseFuture.wait(); }
            return ownSource.GetAt(i);
        },options,1024);
        return ownQuery.Page(2,"",{"ordinal"},concurrentFilter);
    });
    const auto ready=reachedFuture.wait_for(std::chrono::seconds(5))==std::future_status::ready;
    if(!ready) { release.set_value(); slower.get(); Require(false,"concurrent builder did not enter source read"); }
    json fasterResult; std::exception_ptr fasterError;
    try {
        AnalysisCacheQuery faster(root/"concurrent-filters",identity,100,read,options,1024);
        fasterResult=faster.Page(2,"",{"ordinal"},concurrentFilter);
    } catch(...) { fasterError=std::current_exception(); }
    release.set_value(); const auto slowerResult=slower.get();
    if(fasterError) std::rethrow_exception(fasterError);
    const auto twoGpu=json::array({{{"ordinal","1"}},{{"ordinal","3"}}});
    Require(fasterResult.at("items")==twoGpu && slowerResult.at("items")==twoGpu &&
        fasterResult.at("page").at("total")=="50" && slowerResult.at("page").at("total")=="50",
        "concurrent publishers must observe the same complete index");

    // A corrupt committed index is an error, never a reason to return a partial
    // array or silently rescan. Corrupt only this owned synthetic fixture.
    std::filesystem::path publishedIndex;
    for(const auto& entry:std::filesystem::recursive_directory_iterator(root/"concurrent-filters"))
        if(entry.path().filename()=="index.bin" && entry.path().parent_path().filename()=="complete")
            publishedIndex=entry.path();
    Require(!publishedIndex.empty(),"committed filter corruption fixture exists");
    { std::ofstream corrupt(publishedIndex,std::ios::binary|std::ios::app);
      corrupt.put('x'); corrupt.flush(); Require(bool(corrupt),"corruption fixture write succeeded"); }
    reads=0;
    AnalysisCacheQuery corrupt(root/"concurrent-filters",identity,100,read,options,1024);
    Reject([&]{corrupt.Page(2,"",{},concurrentFilter);},"index_size");
    Require(reads==0,"corrupt complete filter is rejected without a hidden rebuild");

    auto workspace=std::make_shared<AnalysisWorkspaceBudget>(1024*1024,512*1024);
    auto budgetOptions=options; budgetOptions.workspace=workspace;
    AnalysisCacheQuery budgetQuery(root/"budget-query",identity,100,read,budgetOptions,1024);
    reads=0;
    {
        AnalysisWorkspaceReservation occupied(workspace,1024*1024-512);
        Reject([&]{budgetQuery.Page(1,"",{},json::object());},"workspace_budget");
        Require(reads==0,"page must reserve parse/response workspace before reading source records");
    }
    const auto budgetPage=budgetQuery.Page(1,"",{"ordinal"},json::object());
    Require(budgetPage.at("items")==json::array({{{"ordinal","0"}}}),"page works after shared capacity is released");
    Require(workspace->Snapshot().currentBytes==0,"completed unfiltered page releases its temporary workspace");

    const auto diskRoot=root/"disk-filters";
    auto diskUsage=std::make_shared<AnalysisDiskUsage>(std::vector<std::filesystem::path>{diskRoot});
    auto disk=std::make_shared<AnalysisDiskBudget>(AnalysisDiskBudget{diskUsage,48,{}});
    auto diskOptions=options; diskOptions.disk=disk;
    AnalysisCacheQuery diskQuery(diskRoot,identity,100,read,diskOptions,1024);
    Reject([&]{diskQuery.Page(2,"",{"ordinal"},filter);},"cache_disk_budget");
    Require(AnalysisDiskUsage::Bytes(diskRoot)==48 && diskUsage->Current()==48,
        "failed filter generation retains only the admitted index header and first row");
    reads=0;
    Require(diskQuery.Page(1,"5",{"ordinal"},json::object()).at("items")[0].at("ordinal")=="5" && reads==1,
        "disk pressure must not block an unfiltered read of completed source data");
    disk->maximumBytes=1024*1024; reads=0;
    const auto resumedDisk=diskQuery.Page(2,"",{"ordinal"},filter);
    Require(reads==102 && resumedDisk.at("page").at("total")=="17" &&
        resumedDisk.at("items")==json::array({{{"ordinal","0"}},{{"ordinal","6"}}}),
        "filter index retry must rebuild every source row and preserve match ordinals");
    const auto diskBytes=AnalysisDiskUsage::Bytes(diskRoot);
    Require(diskUsage->Current()==diskBytes,"completed and failed filter generations share the live disk ledger");
    disk->maximumBytes=1; reads=0;
    AnalysisCacheQuery coldDisk(diskRoot,identity,100,read,diskOptions,1024);
    Require(coldDisk.Page(2,"",{"ordinal"},filter).at("items")==resumedDisk.at("items") && reads==2,
        "a completed index can reopen read-only even when the quota is below existing usage");
    reads=0;
    Reject([&]{coldDisk.Page(2,"",{"ordinal"},{{"domain","gpu"}});},"cache_disk_budget");
    Require(reads==0 && AnalysisDiskUsage::Bytes(diskRoot)==diskBytes,
        "a new over-quota filter must reject growth before scanning or writing an index");
}
}
int main()
{
    const auto root=std::filesystem::temp_directory_path()/
        ("jn-cache-query-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try { Queries(root); std::filesystem::remove_all(root); std::cout<<"cache query tests passed\n"; return 0; }
    catch(const std::exception& e) { std::cerr<<e.what()<<"\nfixture: "<<root<<'\n'; return 1; }
}
