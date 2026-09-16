#include "Core.hpp"
#include "Presentation.hpp"
#include <iostream>
#include <stdexcept>
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
        Require(FormatDuration(15999) == "00:15", "duration must show recorded seconds");
        Require(FormatDuration(3612000) == "01:00:12", "hour-long recording duration wrapped");
        Require(RecordingDuration(Json::object()) == "未记录", "missing duration displayed as zero");
        Require(RecordingDuration({{"elapsed_ms",0}}) == "00:00", "known zero duration lost");
        Require(RecordingDuration({{"elapsed_ms",15432}}) == "00:15", "history omitted saved duration");
        Require(RecordingDuration({{"capture_status",{{"elapsed_ms",65000}}}}) == "01:05", "legacy capture duration omitted");
        auto indexing = DescribeProcessing("validating", "indexing");
        auto checking = DescribeProcessing("validating", "checking");
        auto converting = DescribeProcessing("converting", "DecodeBuild");
        Require(indexing.step == 1 && checking.step == 2 && converting.step == 0, "processing stages conflated");
        Require(!indexing.title.empty() && indexing.title != checking.title && indexing.title != converting.title, "processing stages have indistinguishable headings");
        Require(!ValidName("../escape"), "path traversal accepted");
        Require(!ValidName("CON"), "Windows reserved name accepted");
        Require(!ValidName("task."), "ambiguous trailing period accepted");
        Require(ValidName("主城 录制"), "Unicode display name rejected");
        Require(Quote(L"D:\\some dir\\") == L"\"D:\\some dir\\\\\"", "trailing slash quote corrupted");
        Require(Quote(L"a\"b") == L"\"a\\\"b\"", "embedded quote corrupted");
        Require(!CanPublish(false, true, true), "unverified candidate publishable");
        Require(!CanPublish(true, false, true), "changed candidate publishable");
        Require(!CanPublish(true, true, false), "unrecoverable trace publishable");
        Require(CanPublish(true, true, true), "verified candidate blocked");
        Settings missing;
        Require(!PreflightErrors(missing, {}).empty(),
                "empty target and missing toolchain enabled recording");
        auto root = fs::temp_directory_path() / fs::path(L"TracyGui-核心测试");
        fs::create_directories(root);
        auto one = NewTaskDirectory(root, "repeat"), two = NewTaskDirectory(root, "repeat");
        Require(one != two, "existing capture directory reused");
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
