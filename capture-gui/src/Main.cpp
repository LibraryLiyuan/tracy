#include "Controller.hpp"
#include "Presentation.hpp"
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <array>
#include <chrono>
#include <fstream>
#include <future>
#include <objbase.h>
#include <shellapi.h>
using namespace capturegui;
namespace
{
constexpr UINT TrayMessage = WM_APP + 41;
GLFWwindow *window = nullptr;
HWND hwnd = nullptr;
WNDPROC oldProc = nullptr;
Controller *controller = nullptr;
bool hotkeyPressed = false, closeRequested = false;
NOTIFYICONDATAW tray{};
LRESULT CALLBACK WindowProc(HWND h, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_HOTKEY)
    {
        hotkeyPressed = true;
        return 0;
    }
    if (message == TrayMessage)
    {
        if (l == WM_LBUTTONDBLCLK || l == WM_LBUTTONUP)
        {
            glfwShowWindow(window);
            SetForegroundWindow(h);
        }
        if (l == WM_RBUTTONUP)
        {
            POINT p;
            GetCursorPos(&p);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"显示窗口");
            AppendMenuW(menu, MF_STRING, 2, L"停止录制");
            SetForegroundWindow(h);
            auto cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, h, nullptr);
            DestroyMenu(menu);
            if (cmd == 1)
                glfwShowWindow(window);
            if (cmd == 2 && controller)
                controller->Stop();
        }
        return 0;
    }
    return CallWindowProcW(oldProc, h, message, w, l);
}
std::string Size(uint64_t n)
{
    char b[64];
    snprintf(b, sizeof(b), "%.2f GiB", double(n) / (1ull << 30));
    return b;
}
void Text(const std::string &s)
{
    ImGui::TextWrapped("%s", s.c_str());
}
void Muted(const std::string &s)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.62f, .68f, .76f, 1));
    Text(s);
    ImGui::PopStyleColor();
}
void Heading(const char *title)
{
    ImGui::Spacing();
    ImGui::SeparatorText(title);
    ImGui::Spacing();
}
void ProcessingPanel(const ViewState &view)
{
    const auto description = DescribeProcessing(view.state, view.stage);
    if (description.title.empty())
        return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(.13f, .19f, .28f, 1));
    ImGui::BeginChild("processing-phase", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PushFont(nullptr, 28.0f);
    Text(description.title);
    ImGui::PopFont();
    Text(description.detail);
    if (description.step >= 0 && ImGui::BeginTable("processing-steps", 3, ImGuiTableFlags_SizingStretchSame))
    {
        const char *labels[] = {"转换文件", "建立索引", "校验与保存"};
        for (int i = 0; i < 3; ++i)
        {
            ImGui::TableNextColumn();
            if (i == description.step)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.53f, .75f, 1.f, 1));
                Text(std::string("正在进行 · ") + labels[i]);
                ImGui::PopStyleColor();
            }
            else
                Muted(std::string(i < description.step ? "已完成 · " : "等待 · ") + labels[i]);
        }
        ImGui::EndTable();
    }
    if (view.percent >= 0)
    {
        Text("当前阶段进度");
        ImGui::ProgressBar(float(view.percent / 100), ImVec2(-1, 0));
    }
    else
        Muted("处理中，请等待…");
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();
}
void PathField(const char *label, std::string &value, bool folder = false,
               const wchar_t *filter = L"All files\0*.*\0")
{
    ImGui::PushID(label);
    Text(label);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70);
    ImGui::InputText("##path", &value);
    ImGui::SameLine();
    if (ImGui::Button("浏览…"))
    {
        auto p = ChoosePath(hwnd, folder, filter);
        if (!p.empty())
            value = p;
    }
    ImGui::PopID();
}
const char *StageName(const std::string &s)
{
    if (s == "ready")
        return "准备录制";
    if (s == "launching")
        return "启动游戏";
    if (s == "waiting_ready")
        return "等待场景就绪";
    if (s == "preflight")
        return "检查工具";
    if (s == "connecting")
        return "连接目标";
    if (s == "recording")
        return "录制中";
    if (s == "draining")
        return "排空与保存";
    if (s == "converting")
        return "转换中";
    if (s == "validating")
        return "校验中";
    if (s == "complete")
        return "完成";
    if (s == "partial")
        return "部分可用";
    if (s == "cancelled")
        return "已取消";
    if (s == "interrupted")
        return "已中断";
    return "失败";
}
bool Finished(const std::string &s)
{
    return s == "complete" || s == "partial";
}
void ResultActions(const Json &task, const Settings &settings, std::string &notice)
{
    auto final = fs::path(Wide(task.value("final", "")));
    if (Finished(task.value("state", "")))
    {
        if (ImGui::Button("用 Tracy Viewer 打开"))
        {
            if (settings.viewer.empty())
                notice = "请先在设置中选择 Tracy Viewer";
            else
            {
                auto args = Quote(final.wstring());
                if ((INT_PTR)ShellExecuteW(hwnd, L"open", Wide(settings.viewer).c_str(), args.c_str(),
                                           nullptr, SW_SHOWNORMAL) <= 32)
                    notice = "无法启动 Viewer";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("复制文件路径"))
        {
            ImGui::SetClipboardText(PathText(final).c_str());
            notice = "文件路径已复制";
        }
        if (ImGui::Button("复制分析请求"))
        {
            ImGui::SetClipboardText(Controller::AnalysisRequest(task).c_str());
            notice = "分析请求已复制，可粘贴给 AI";
        }
    }
    if (ImGui::Button("打开文件夹"))
        OpenPath(final.parent_path());
}
} // namespace
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    HANDLE instance = CreateMutexW(nullptr, FALSE, L"Local\\JNTracyCaptureGui-v1");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBoxW(nullptr, L"Tracy Capture 已经运行，请从任务栏或托盘打开。", L"Tracy Capture", MB_OK);
        if (instance)
            CloseHandle(instance);
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try
    {
        Settings settings;
        auto settingsPath = SettingsDirectory() / L"settings.json";
        try
        {
            settings = Settings::FromJson(ReadJson(settingsPath));
        }
        catch (...)
        {
            settings = Settings::FromJson(Json::object());
        }
        settings.name = "Player_" + Timestamp();
        Controller core;
        controller = &core;
        if (!glfwInit())
            throw std::runtime_error("GLFW initialization failed");
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        window = glfwCreateWindow(1120, 880, "Tracy Capture", nullptr, nullptr);
        if (!window)
            throw std::runtime_error("Cannot create OpenGL window");
        glfwSetWindowSizeLimits(window, 760, 600, GLFW_DONT_CARE, GLFW_DONT_CARE);
        glfwMakeContextCurrent(window);
        glfwSwapInterval(1);
        hwnd = glfwGetWin32Window(window);
        oldProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)WindowProc);
        glfwSetWindowCloseCallback(window, [](GLFWwindow *w) {
            closeRequested = true;
            glfwSetWindowShouldClose(w, FALSE);
        });
        tray.cbSize = sizeof(tray);
        tray.hWnd = hwnd;
        tray.uID = 1;
        tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        tray.uCallbackMessage = TrayMessage;
        tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(tray.szTip, L"Tracy Capture");
        Shell_NotifyIconW(NIM_ADD, &tray);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigDpiScaleFonts = true;
        io.ConfigDpiScaleViewports = true;
        wchar_t windowsDir[MAX_PATH];
        GetWindowsDirectoryW(windowsDir, MAX_PATH);
        auto font = PathText(fs::path(windowsDir) / L"Fonts/msyh.ttc");
        io.Fonts->AddFontFromFileTTF(font.c_str(), 18.0f);
        ImGui::StyleColorsDark();
        auto &style = ImGui::GetStyle();
        style.WindowPadding = ImVec2(24, 20);
        style.FramePadding = ImVec2(8, 6);
        style.ItemSpacing = ImVec2(10, 9);
        style.FrameRounding = 5;
        style.ChildRounding = 8;
        style.Colors[ImGuiCol_WindowBg] = ImVec4(.08f, .095f, .12f, 1);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(.115f, .135f, .165f, 1);
        style.Colors[ImGuiCol_Button] = ImVec4(.18f, .28f, .43f, 1);
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 130");
        std::string notice;
        auto registerKey = [&] {
            UnregisterHotKey(hwnd, 1);
            if (!RegisterHotKey(hwnd, 1, settings.hotkeyModifiers | MOD_NOREPEAT, settings.hotkey))
                notice = "快捷键已被占用，请在设置中修改；按钮仍可使用";
        };
        registerKey();
        std::vector<std::pair<ProcessIdentity, std::string>> processes;
        auto discovery = std::async(std::launch::async, LocalProcesses);
        ProcessIdentity selected{};
        std::vector<Json> history;
        std::future<std::vector<Json>> historyRead;
        bool refreshHistory = true;
        auto presets = IniFiles(settings);
        for (auto &p : presets)
            if (settings.ini.empty() && p.ends_with("JNTracy.HighEvidence.ini"))
                settings.ini = p;
        std::future<std::vector<std::string>> preflightFuture;
        std::vector<std::string> preflight = {"正在检查目标与配置"};
        std::string preflightSignature;
        auto nextPreflight = std::chrono::steady_clock::now();
        bool running = true;
        int tab = 0;
        std::string previousState;
        while (running)
        {
            glfwPollEvents();
            auto view = core.Snapshot();
            if (discovery.valid() && discovery.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                processes = discovery.get();
            if (view.target.pid && !settings.existing)
                selected = view.target;
            if (view.state != previousState)
            {
                if (!view.busy)
                    refreshHistory = true;
                previousState = view.state;
            }
            if (refreshHistory && !historyRead.valid())
            {
                auto output = settings.output;
                historyRead =
                    std::async(std::launch::async, [output] { return Controller::History(Wide(output)); });
                refreshHistory = false;
            }
            if (historyRead.valid() &&
                historyRead.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                history = historyRead.get();
            auto signature =
                settings.ToJson().dump() + std::to_string(selected.pid) + std::to_string(selected.created);
            if (preflightFuture.valid() &&
                preflightFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                try
                {
                    preflight = preflightFuture.get();
                }
                catch (const std::exception &e)
                {
                    preflight = {e.what()};
                }
            }
            if (!preflightFuture.valid() &&
                (signature != preflightSignature || std::chrono::steady_clock::now() >= nextPreflight))
            {
                if (signature != preflightSignature)
                    preflight = {"正在检查目标与配置"};
                preflightSignature = signature;
                nextPreflight = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                auto copy = settings;
                auto target = selected;
                preflightFuture =
                    std::async(std::launch::async, [copy, target] { return PreflightErrors(copy, target); });
            }
            auto start = [&] {
                try
                {
                    if (core.Start(settings, selected))
                        AtomicJson(settingsPath, settings.ToJson());
                }
                catch (const std::exception &e)
                {
                    notice = e.what();
                }
            };
            if (hotkeyPressed)
            {
                hotkeyPressed = false;
                if (view.state == "recording" || view.state == "connecting")
                    core.Stop();
                else if (!view.busy && settings.ready && preflight.empty())
                    start();
            }
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            auto vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->Pos);
            ImGui::SetNextWindowSize(vp->Size);
            ImGui::Begin("Tracy Capture", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextUnformatted("TRACY CAPTURE");
            ImGui::SameLine();
            Muted("独立录制工具  /  本机 Player · Editor");
            if (ImGui::Button("录制"))
                tab = 0;
            ImGui::SameLine();
            if (ImGui::Button("录制记录"))
            {
                tab = 1;
                refreshHistory = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("设置"))
                tab = 2;
            ImGui::Separator();
            if (tab == 0)
            {
                Heading(StageName(view.state));
                Text(view.message);
                bool setup = !view.busy && !Finished(view.state) && view.state != "failed" &&
                             view.state != "interrupted";
                if (setup)
                {
                    ImGui::BeginChild("setup", ImVec2(0, -105), ImGuiChildFlags_Borders);
                    const int setupColumns = ImGui::GetContentRegionAvail().x >= 850 ? 2 : 1;
                    ImGui::BeginTable("setup-columns", setupColumns, ImGuiTableFlags_SizingStretchSame);
                    ImGui::TableNextColumn();
                    Heading("录制目标");
                    bool before = settings.existing;
                    if (ImGui::RadioButton("启动 Player", !settings.existing))
                        settings.existing = false;
                    ImGui::SameLine();
                    if (ImGui::RadioButton("连接已有 Player / Editor", settings.existing))
                        settings.existing = true;
                    if (before != settings.existing)
                    {
                        settings.ready = false;
                        selected = settings.existing ? ProcessIdentity{} : view.target;
                    }
                    if (!settings.existing)
                    {
                        auto prior = settings.player;
                        PathField("Player 程序", settings.player, false, L"Windows program\0*.exe\0");
                        if (prior != settings.player)
                        {
                            presets = IniFiles(settings);
                            for (auto &p : presets)
                                if (p.ends_with("JNTracy.HighEvidence.ini"))
                                    settings.ini = p;
                            settings.name =
                                PathText(fs::path(Wide(settings.player)).stem()) + "_" + Timestamp();
                        }
                        ImGui::Checkbox("以管理员身份启动游戏", &settings.admin);
                    }
                    else
                    {
                        std::string label =
                            selected.pid ? "PID " + std::to_string(selected.pid) : "请选择目标进程";
                        if (ImGui::BeginCombo("目标进程", label.c_str()))
                        {
                            for (auto &[id, path] : processes)
                            {
                                auto name = PathText(fs::path(Wide(path)).filename()) + " · PID " +
                                            std::to_string(id.pid);
                                if (ImGui::Selectable(name.c_str(), id.pid == selected.pid))
                                {
                                    selected = id;
                                    settings.player = path;
                                    settings.ready = false;
                                    presets = IniFiles(settings);
                                }
                            }
                            ImGui::EndCombo();
                        }
                        if (ImGui::Button("刷新目标") && !discovery.valid())
                            discovery = std::async(std::launch::async, LocalProcesses);
                        Muted("选择本机游戏或 Unity Editor；连接成功后才开始计时。");
                    }
                    Text("采集配置 · INI 预设");
                    auto presetName = settings.ini.empty()
                                          ? "请选择 HighEvidence 或自定义文件"
                                          : PathText(fs::path(Wide(settings.ini)).filename());
                    if (ImGui::BeginCombo("##preset", presetName.c_str()))
                    {
                        for (auto &p : presets)
                            if (ImGui::Selectable(PathText(fs::path(Wide(p)).filename()).c_str(),
                                                  p == settings.ini))
                                settings.ini = p;
                        ImGui::EndCombo();
                    }
                    PathField("INI 文件", settings.ini, false, L"INI files\0*.ini\0");
                    Muted(settings.existing ? "所选配置用于核对；目标实际生效配置将在校验时读取。"
                          : selected.pid    ? "更改配置将在下次启动时使用；不会热切换当前游戏。"
                                            : "启动时使用所选 INI；不会覆盖游戏原配置。");
                    if (!settings.existing)
                    {
                        if (ImGui::Button("启动游戏"))
                        {
                            core.Launch(settings);
                            settings.ready = false;
                        }
                        ImGui::SameLine();
                        Muted(selected.pid ? "已登记 PID " + std::to_string(selected.pid) : "尚未启动");
                    }
                    ImGui::TableNextColumn();
                    Heading("本次录制");
                    Text("录制名称");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputText("##name", &settings.name);
                    Text("场景名称（选填）");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputText("##scene", &settings.scene);
                    Text("操作备注（选填）");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputText("##note", &settings.note);
                    PathField("保存位置", settings.output, true);
                    ImGui::Checkbox("定时停止", &settings.timed);
                    if (settings.timed)
                    {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(100);
                        ImGui::InputInt("秒（收到数据后计时）", &settings.seconds);
                    }
                    if (ImGui::CollapsingHeader("高级设置"))
                    {
                        ImGui::InputInt("本机端口", &settings.port);
                        ImGui::InputInt("排空无进展超时（秒，0 禁用）", &settings.drainSeconds);
                        PathField("工作目录", settings.workingDirectory, true);
                        ImGui::InputText("启动参数", &settings.arguments);
                        ImGui::InputScalar("磁盘警告（GiB）", ImGuiDataType_U64, &settings.warnGiB);
                        ImGui::InputScalar("保护停止（GiB）", ImGuiDataType_U64, &settings.stopGiB);
                        ImGui::InputScalar("Capture 内存上限（MiB）", ImGuiDataType_U64, &settings.memoryMiB);
                    }
                    ImGui::EndTable();
                    ImGui::EndChild();
                    ImGui::Checkbox("场景已就绪，可以开始录制", &settings.ready);
                    ImGui::BeginDisabled(!settings.ready || !preflight.empty());
                    if (ImGui::Button("开始录制", ImVec2(210, 42)))
                        start();
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    Muted(preflight.empty() ? "检查通过 · 停止后自动转换与校验" : preflight.front());
                }
                else
                {
                    ImGui::BeginChild("status", ImVec2(0, -65), ImGuiChildFlags_Borders);
                    Text(view.task.value("name", settings.name));
                    if (!view.busy)
                        Text("录制时长：" + RecordingDuration(view.task));
                    if (view.busy)
                    {
                        ProcessingPanel(view);
                        Heading(("录制时长  " + FormatDuration(view.elapsedMs)).c_str());
                        if (ImGui::BeginTable("metrics", 3))
                        {
                            ImGui::TableNextColumn();
                            Muted("stream 大小");
                            Text(Size(view.streamBytes));
                            ImGui::TableNextColumn();
                            Muted("工作进程内存");
                            Text(Size(view.memoryBytes));
                            ImGui::TableNextColumn();
                            Muted("磁盘剩余");
                            Text(Size(view.freeBytes));
                            ImGui::EndTable();
                        }

                        Muted(view.gameMayClose ? "现在可以关闭游戏。"
                                                : "请保持游戏运行，直到排空与保存完成。");
                        if (view.state == "recording" || view.state == "connecting")
                        {
                            if (ImGui::Button(view.state == "connecting" ? "取消连接" : "停止录制并处理",
                                              ImVec2(230, 42)))
                                core.Stop();
                        }
                    }
                    else
                    {
                        Text("文件完整性：" + view.task.value("completeness", "尚未确定"));
                        if (Finished(view.state))
                            ResultActions(view.task, settings, notice);
                        else
                        {
                            if (!view.directory.empty() && ImGui::Button("恢复检查 / 重试"))
                                core.Recover(Wide(view.directory));
                            ImGui::SameLine();
                            if (ImGui::Button("保留文件，开始新任务"))
                            {
                                core.Reset();
                                settings.ready = false;
                                settings.name = "Player_" + Timestamp();
                            }
                        }
                        if (ImGui::CollapsingHeader("实际配置与采集质量"))
                        {
                            Text(view.actualConfig);
                            Text(view.quality);
                            if (view.task.contains("quality") && view.task["quality"].contains("findings"))
                                for (const auto &finding : view.task["quality"]["findings"])
                                {
                                    auto severity = finding.value("severity", "");
                                    Text(std::string(severity == "error"     ? "错误："
                                                     : severity == "warning" ? "警告："
                                                                             : "说明：") +
                                         finding.value("message", ""));
                                }
                            if (view.task.contains("validation_evidence"))
                            {
                                if (ImGui::Button("打开完整校验证据"))
                                    OpenPath(Wide(view.task["validation_evidence"].get<std::string>()));
                            }
                        }
                        if (Finished(view.state) && ImGui::Button("再次录制"))
                        {
                            core.Reset();
                            settings.ready = false;
                            settings.name = "Player_" + Timestamp();
                        }
                    }
                    if (ImGui::CollapsingHeader("详细日志与文件"))
                    {
                        Text(view.directory);
                        if (!view.directory.empty() && ImGui::Button("打开任务目录"))
                            OpenPath(Wide(view.directory));
                        Text(view.message);
                    }
                    ImGui::EndChild();
                }
            }
            if (tab == 1)
            {
                Heading("录制记录");
                if (ImGui::Button("刷新记录"))
                    refreshHistory = true;
                ImGui::BeginChild("history", ImVec2(0, -65));
                for (auto &t : history)
                {
                    ImGui::PushID(t.value("directory", "").c_str());
                    if (ImGui::CollapsingHeader((t.value("name", "") + "  ·  录制时长 " +
                                                 RecordingDuration(t) + "  ·  " +
                                                 StageName(t.value("state", "failed")))
                                                    .c_str()))
                    {
                        Text(t.value("created", "") + " · 录制时长 " + RecordingDuration(t) + " · " +
                             Size(t.value("final_bytes", 0ull)));
                        Text(t.value("message", ""));
                        ResultActions(t, settings, notice);
                        if (!Finished(t.value("state", "")))
                        {
                            ImGui::BeginDisabled(view.busy);
                            if (ImGui::Button("恢复 / 重试"))
                            {
                                core.Recover(Wide(t.at("directory").get<std::string>()));
                                tab = 0;
                            }
                            ImGui::EndDisabled();
                        }
                    }
                    ImGui::PopID();
                }
                if (history.empty())
                    Muted("当前保存位置尚无录制记录");
                ImGui::EndChild();
            }
            if (tab == 2)
            {
                Heading("设置");
                ImGui::BeginDisabled(view.busy);
                PathField("工具目录（Capture / Convert / Query）", settings.toolDirectory, true);
                PathField("Tracy Viewer（可选）", settings.viewer, false, L"Windows program\0*.exe\0");
                Heading("全局快捷键");
                bool ctrl = settings.hotkeyModifiers & MOD_CONTROL,
                     shift = settings.hotkeyModifiers & MOD_SHIFT, alt = settings.hotkeyModifiers & MOD_ALT;
                ImGui::Checkbox("Ctrl", &ctrl);
                ImGui::SameLine();
                ImGui::Checkbox("Shift", &shift);
                ImGui::SameLine();
                ImGui::Checkbox("Alt", &alt);
                settings.hotkeyModifiers =
                    (ctrl ? MOD_CONTROL : 0) | (shift ? MOD_SHIFT : 0) | (alt ? MOD_ALT : 0);
                std::string key = "F" + std::to_string(settings.hotkey - VK_F1 + 1);
                if (ImGui::BeginCombo("按键", key.c_str()))
                {
                    for (int i = 0; i < 12; i++)
                        if (ImGui::Selectable(("F" + std::to_string(i + 1)).c_str(),
                                              settings.hotkey == VK_F1 + i))
                            settings.hotkey = VK_F1 + i;
                    ImGui::EndCombo();
                }
                ImGui::Checkbox("录制开始与完成提示音", &settings.sound);
                if (ImGui::Button("保存设置并应用快捷键"))
                {
                    AtomicJson(settingsPath, settings.ToJson());
                    notice = "设置已保存";
                    registerKey();
                }
                ImGui::EndDisabled();
                Muted("INI 预设来自所选 Player 目录、当前 INI 目录及工具旁 presets 目录中的实际文件。");
            }
            if (closeRequested)
            {
                closeRequested = false;
                if (view.busy)
                    ImGui::OpenPopup("任务尚未完成");
                else
                    running = false;
            }
            if (ImGui::BeginPopupModal("任务尚未完成", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                Text("当前任务尚未完成，不能直接关闭。");
                Text("可以放入后台继续录制或转换。强制关闭可能使本轮数据不完整，已有文件会保留。");
                if (ImGui::Button("返回界面"))
                    ImGui::CloseCurrentPopup();
                ImGui::SameLine();
                if (ImGui::Button("放入后台继续处理"))
                {
                    glfwHideWindow(window);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("强制关闭"))
                {
                    core.ForceClose();
                    Shell_NotifyIconW(NIM_DELETE, &tray);
                    ExitProcess(130);
                }
                ImGui::EndPopup();
            }
            ImGui::Separator();
            Muted(notice.empty() ? "本机录制 · 未连接 AI 软件" : notice);
            ImGui::End();
            ImGui::Render();
            int width, height;
            glfwGetFramebufferSize(window, &width, &height);
            glViewport(0, 0, width, height);
            glClearColor(.08f, .095f, .12f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
            if (!glfwGetWindowAttrib(window, GLFW_VISIBLE))
                Sleep(100);
        }
        AtomicJson(settingsPath, settings.ToJson());
        UnregisterHotKey(hwnd, 1);
        Shell_NotifyIconW(NIM_DELETE, &tray);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
    }
    catch (const std::exception &e)
    {
        MessageBoxW(nullptr, Wide(e.what()).c_str(), L"Tracy Capture", MB_OK | MB_ICONERROR);
        if (instance)
            CloseHandle(instance);
        CoUninitialize();
        return 1;
    }
    if (instance)
        CloseHandle(instance);
    CoUninitialize();
    return 0;
}
