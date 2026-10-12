#include "Core.hpp"
#include "Presentation.hpp"
#include "Process.hpp"
#include <algorithm>
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
// What the process reports once it has started.
static LoadedIni Reported(const ProcessIdentity &target)
{
    for (int i = 0; i < 100; ++i)
    {
        auto loaded = QueryLoadedIni(target);
        if (loaded.source == "reported")
            return loaded;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw std::runtime_error("the player never reported its INI");
}
// A stand-in player is started with one INI, restarted with another, and
// closed; the INI the GUI shows comes from what the player reports.
static void PlayerRestart(const fs::path &fixture, const fs::path &root)
{
    // A player folder: the program with JNTracyClient.dll beside it.
    const auto folder = root / L"Player", game = folder / L"Game.exe";
    fs::create_directories(folder);
    fs::copy_file(fixture, game);
    std::ofstream(folder / L"JNTracyClient.dll").put('\0');
    const auto first = root / L"JNTracy.first.ini", second = root / L"JNTracy.second.ini", copy = root / L"copy of second.ini";
    std::ofstream(first) << "[Config]\nSchemaVersion=3\nPreset=First\n";
    std::ofstream(second) << "[Config]\nSchemaVersion=3\nPreset=Second\n";
    fs::copy_file(second, copy);
    // Players a failed check leaves running are ended.
    struct Players
    {
        std::vector<ProcessIdentity> started;
        ~Players()
        {
            for (auto &p : started)
                TerminateOwned(p);
        }
    } players;
    const auto launch = [&](const Settings &settings) { return players.started.emplace_back(LaunchPlayer(settings)); };
    Settings s;
    s.player = PathText(game);
    s.ini = PathText(first);
    s.admin = false;
    s.arguments = "--marker \"a b\"";
    const auto running = launch(s);
    const auto loaded = Reported(running);
    Require(fs::path(Wide(loaded.path)) == first && loaded.status == "loaded", "the player's INI was misread");
    Require(CompareIni(loaded, PathText(first)) == IniMatch::Same, "the loaded INI did not match itself");
    Require(CompareIni(loaded, PathText(second)) == IniMatch::Different, "another INI matched the loaded one");
    s.ini = PathText(second);
    auto check = RunPreflight(s, running);
    Require(check.alive && check.match == IniMatch::Different && check.loadedFileUnchanged, "preflight missed the INI mismatch");
    Require(std::any_of(check.errors.begin(), check.errors.end(), [](auto &e) { return e.find("未生效") != std::string::npos; }),
            "an INI mismatch allowed recording");
    // Started without elevation, it runs as elevated as this test.
    HANDLE token = nullptr;
    TOKEN_ELEVATION self{};
    DWORD size = 0;
    Require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
                GetTokenInformation(token, TokenElevation, &self, sizeof(self), &size),
            "own elevation unreadable");
    CloseHandle(token);
    Require(RunsElevated(running) == (self.TokenIsElevated != 0), "the player's elevation was misread");
    const auto info = ReadLaunchInfo(running);
    Require(fs::path(Wide(info.exe)) == game, "the player's program was misread");
    Require(info.arguments == "--marker \"a b\"", "restart would change the player's arguments");
    Require(fs::equivalent(Wide(info.directory), folder), "restart would change the player's directory");
    const auto restarted = players.started.emplace_back(RestartPlayer(s, running));
    Require(!Alive(running) && Alive(restarted), "restart did not replace the player");
    Require(CompareIni(Reported(restarted), PathText(copy)) == IniMatch::Same, "the restarted player kept its old INI");
    Require(ReadLaunchInfo(restarted).arguments == "--marker \"a b\"", "restart lost the player's arguments");
    Require(CloseProcess(restarted, 5000) && !Alive(restarted), "the player did not close");
    // A player that ignores the close message is ended after the grace time.
    s.arguments = "--ignore-close";
    const auto stubborn = launch(s);
    Reported(stubborn);
    Require(CloseProcess(stubborn, 300) && !Alive(stubborn), "a player that ignores closing kept running");
    // A player that reports nothing: its INI follows its command line.
    s.arguments = "--silent";
    const auto silent = launch(s);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto inferred = QueryLoadedIni(silent);
    Require(inferred.source == "inferred" && fs::path(Wide(inferred.path)) == second, "the INI was not inferred from -jn-tracy-config");
    Require(CompareIni(inferred, PathText(second)) == IniMatch::Same, "an inferred INI did not match");
    Require(CloseProcess(silent, 5000), "the silent player did not close");
    Require(QueryLoadedIni(silent).source == "unknown", "an exited player still reported an INI");
    // Without JNTracyClient.dll beside it a program is no JN Tracy game: no
    // INI is inferred, so nothing offers to restart it or blocks recording.
    if (!fs::exists(fixture.parent_path() / L"JNTracyClient.dll"))
    {
        s.player = PathText(fixture);
        const auto stranger = launch(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        Require(QueryLoadedIni(stranger).source == "unknown" && RunPreflight(s, stranger).match == IniMatch::Unknown,
                "an INI was inferred for a program without JN Tracy");
        Require(CloseProcess(stranger, 5000), "the program did not close");
    }
}
int main(int argc, char **argv)
{
    try
    {
        {
            const wchar_t raw[] = L"=C:=C:\\\0Path=C:\\Windows\0jn_tracy_ini_path=F:\\a b\\JNTracy.ini\0JN_TRACY_INI_STATUS=loaded\0";
            const std::wstring block(raw, std::size(raw));
            Require(EnvironmentValue(block, L"JN_TRACY_INI_PATH") == L"F:\\a b\\JNTracy.ini", "environment names must ignore case");
            Require(EnvironmentValue(block, L"JN_TRACY_INI_STATUS") == L"loaded", "the last environment entry was missed");
            Require(EnvironmentValue(block, L"JN_TRACY_INI_SHA256").empty() && EnvironmentValue(block, L"C:").empty(),
                    "a missing environment variable was found");
            Require(EnvironmentValue(block, L"JN_TRACY_INI").empty(), "a name prefix matched a longer name");
            Require(EnvironmentValue(L"", L"Path").empty(), "an empty environment produced a value");
        }
        {
            const auto [arguments, ini] = SplitIniArgument(
                L"\"F:\\Game dir\\Game.exe\" -screen-width 1280 -jn-tracy-config \"F:\\a b\\JNTracy.x.ini\" -logFile \"C:\\log dir\\x.log\"");
            Require(ini == L"F:\\a b\\JNTracy.x.ini", "the -jn-tracy-config value was misread");
            Require(arguments == L"-screen-width 1280 -logFile \"C:\\log dir\\x.log\"", "other arguments were changed");
            Require(SplitIniArgument(L"Game.exe").first.empty() && SplitIniArgument(L"Game.exe").second.empty(), "arguments invented");
            Require(SplitIniArgument(L"Game.exe --JN-TRACY-CONFIG a.ini x") == std::pair<std::wstring, std::wstring>(L"x", L"a.ini"),
                    "the option's spelling variants were missed");
            Require(SplitIniArgument(L"Game.exe \"\" y").first == L"\"\" y", "an empty argument was dropped");
        }
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
        {
            // The same content under another name is the same configuration;
            // without a file the game runs its compiled defaults.
            const auto a = one / "JNTracy.ini", b = one / "renamed.ini", c = one / "other.ini";
            std::ofstream(a) << "[Config]\nSchemaVersion=3\n";
            std::ofstream(b) << "[Config]\nSchemaVersion=3\n";
            std::ofstream(c) << "[Config]\nSchemaVersion=2\n";
            LoadedIni loaded;
            loaded.source = "reported";
            loaded.sha256 = Sha256(a);
            for (auto &ch : loaded.sha256)
                ch = char(toupper((unsigned char)ch));
            Require(CompareIni(loaded, PathText(b)) == IniMatch::Same, "a renamed copy of the loaded INI did not match");
            Require(CompareIni(loaded, PathText(c)) == IniMatch::Different, "another INI matched");
            Require(CompareIni(loaded, PathText(one / "missing.ini")) == IniMatch::Unknown, "a missing selection was compared");
            loaded.sha256.clear();
            Require(CompareIni(loaded, PathText(b)) == IniMatch::Different, "compiled defaults matched an INI file");
            Require(CompareIni(LoadedIni{}, PathText(b)) == IniMatch::Unknown, "an unknown INI was compared");
            Require(QueryLoadedIni({}).source == "unknown", "no process reported an INI");
        }
        if (argc > 1)
            PlayerRestart(fs::absolute(argv[1]), fs::absolute(one));
        fs::remove_all(root);
        std::cout << "Core behavior checks passed\n";
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
