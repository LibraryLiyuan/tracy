#include "Core.hpp"
#include "Presentation.hpp"
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <fstream>
#include <thread>
using namespace capturegui;
static void Require(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
int main()
{
    try
    {
        Require(!CaptureConnectionTimedOut(true, false, 95071),
                "transient missing capture status stopped an established recording");
        Require(!CaptureConnectionTimedOut(false, true, 95071), "active connection timed out");
        Require(!CaptureConnectionTimedOut(false, false, 59999), "connection timed out early");
        Require(CaptureConnectionTimedOut(false, false, 60001), "idle connection never timed out");
        {
            // i7-12700: 16 performance logical processors (class 1), 4 efficiency (class 0).
            std::vector<CpuSet> hybrid;
            for (uint8_t i = 0; i < 20; ++i)
                hybrid.push_back({uint32_t(256 + i), uint8_t(i < 16 ? 1 : 0), i});
            const auto efficient = EfficiencyCpuSets(hybrid);
            Require(efficient.size() == 4 && efficient.front().logical == 16 && efficient.back().id == 275,
                    "hybrid CPU must keep tools on its efficiency cores");
            std::vector<CpuSet> uniform(8);
            for (uint8_t i = 0; i < 8; ++i)
                uniform[i] = {uint32_t(256 + i), 0, i};
            Require(EfficiencyCpuSets(uniform).empty(), "single core type must not restrict tools");
            Require(EfficiencyCpuSets({}).empty(), "no CPU set information must not restrict tools");
        }
        Require(FormatDuration(15999) == "00:15", "duration must show recorded seconds");
        Require(FormatDuration(3612000) == "01:00:12", "hour-long recording duration wrapped");
        Require(RecordingDuration(Json::object()) == "未记录", "missing duration displayed as zero");
        Require(RecordingDuration({{"elapsed_ms",0}}) == "00:00", "known zero duration lost");
        Require(RecordingDuration({{"elapsed_ms",15432}}) == "00:15", "history omitted saved duration");
        Require(RecordingDuration({{"capture_status",{{"elapsed_ms",65000}}}}) == "01:05", "legacy capture duration omitted");
        auto saving = DescribeProcessing("draining", "");
        auto finishing = DescribeProcessing("converting", "EventStore");
        auto building = DescribeProcessing("converting", "EventStoreBuild");
        Require(saving.step == 0 && finishing.step == 1 && building.step == 1, "saving steps conflated");
        Require(!saving.title.empty() && saving.title != finishing.title && finishing.title != building.title, "saving steps have indistinguishable headings");
        Require(DescribeProcessing("validating", "checking").title.empty(), "the removed .tracy validation is still described");
        Require(ParseStoreProgress("").percent < 0, "progress invented without a log");
        Require(ParseStoreProgress("Open/build event store\r\nIndex 250/1000\r\n").percent == 22.5, "indexing progress misread");
        const auto finishingStore = ParseStoreProgress("Index 1000/1000\r\nFinish store elapsed=1.0s\r\nBuild store indexes elapsed=2.0s\r\nIndex zone ranges elapsed=3.0s\r\n");
        Require(finishingStore.percent == 94 && !finishingStore.step.empty(), "finishing steps after indexing misread");
        Require(ParseStoreProgress("Index journal elapsed=0.01s\r\n").percent < 0, "a stage named Index read as progress");
        Require(ParseStoreProgress("Index 10/0\r\n").percent < 0, "progress of an empty recording");
        Require(ParseStoreProgress("x 900/1000\r\nIndex 500/1000").percent == 45, "the last progress line is not the latest");
        Require(!ValidName("../escape"), "path traversal accepted");
        Require(!ValidName("CON"), "Windows reserved name accepted");
        Require(!ValidName("task."), "ambiguous trailing period accepted");
        Require(ValidName("主城 录制"), "Unicode display name rejected");
        Require(Quote(L"D:\\some dir\\") == L"\"D:\\some dir\\\\\"", "trailing slash quote corrupted");
        Require(Quote(L"a\"b") == L"\"a\\\"b\"", "embedded quote corrupted");
        Settings missing;
        Require(!PreflightErrors(missing, {}).empty(),
                "empty target and missing toolchain enabled recording");
        auto root = fs::temp_directory_path() / fs::path(L"TracyGui-核心测试");
        fs::create_directories(root);
        auto one = NewTaskDirectory(root, "repeat"), two = NewTaskDirectory(root, "repeat");
        Require(one != two, "existing capture directory reused");
        auto bundled = one / "bin";
        fs::create_directories(bundled);
        Settings legacy = Settings::FromJson({{"tools", "old-release"}});
        for (auto name : {"tracy-capture.exe", "tracy-query.exe"})
            std::ofstream(bundled / name).put('\0');
        PreferBundledTools(legacy, bundled);
        Require(legacy.toolDirectory == "old-release", "incomplete bundle replaced configured tools");
        std::ofstream(bundled / "tracy-stream-export.exe").put('\0');
        PreferBundledTools(legacy, bundled);
        Require(legacy.toolDirectory == PathText(fs::absolute(bundled)), "old persisted tools overrode current bundle");
        legacy.toolDirectory = "explicit-custom";
        legacy.customTools = true;
        auto custom = Settings::FromJson(legacy.ToJson());
        PreferBundledTools(custom, bundled);
        Require(custom.toolDirectory == "explicit-custom", "explicit custom tools were lost on restart");
        const auto sharedStatus = one / "capture-status.json";
        const Json status = {{"first_data_received", true}, {"padding", std::string(512 * 1024, 'x')}};
        AtomicJson(sharedStatus, status);
        std::atomic<bool> done = false;
        std::exception_ptr writerError;
        std::jthread writer([&] {
            try { for (int i = 0; i < 32; ++i) AtomicJson(sharedStatus, status); }
            catch (...) { writerError = std::current_exception(); }
            done = true;
        });
        do { Require(ReadJson(sharedStatus).at("first_data_received") == true, "atomic status read was torn"); }
        while (!done);
        writer.join();
        if (writerError) std::rethrow_exception(writerError);
        AtomicJson(one / "state.json", {{"value", "中文"}});
        Require(ReadJson(one / "state.json").at("value") == "中文", "Unicode JSON round trip failed");
        auto before = Sha256(one / "state.json");
        AtomicJson(one / "state.json", {{"value", "changed"}});
        Require(before != Sha256(one / "state.json"), "content change not detected");
        fs::remove_all(root);
        std::cout << "Core behavior checks passed\n";
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
