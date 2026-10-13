// Stands in for a JN Tracy player in tests: it resolves its INI as the engine
// does (-jn-tracy-config, else JN_TRACY_CONFIG, else JNTracy.ini beside it),
// publishes JN_TRACY_INI_* as JNTracyIniConfig.cpp does, and keeps a window
// until it is closed. --ignore-close keeps it after the close message;
// --silent publishes nothing, like a player that has not started JN Tracy.
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <string>
#include <vector>
namespace
{
bool ignoreClose = false;
std::wstring Sha256(const std::wstring &path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    std::vector<unsigned char> bytes(GetFileSize(file, nullptr));
    DWORD read = 0;
    const bool ok = bytes.empty() || (ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr) && read == bytes.size());
    CloseHandle(file);
    unsigned char digest[32] = {};
    if (!ok || BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, bytes.data(), ULONG(bytes.size()), digest, 32) < 0)
        return {};
    std::wstring text;
    for (auto b : digest)
    {
        wchar_t hex[3];
        swprintf(hex, 3, L"%02X", unsigned(b));
        text += hex;
    }
    return text;
}
LRESULT CALLBACK Window(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_CLOSE)
    {
        if (!ignoreClose)
            DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    auto **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring ini;
    bool silent = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring argument = argv[i];
        if (argument == L"-jn-tracy-config" && i + 1 < argc)
            ini = argv[++i];
        ignoreClose = ignoreClose || argument == L"--ignore-close";
        silent = silent || argument == L"--silent";
    }
    LocalFree(argv);
    if (ini.empty())
    {
        wchar_t value[32768];
        const auto length = GetEnvironmentVariableW(L"JN_TRACY_CONFIG", value, 32768);
        if (length && length < 32768)
            ini = value;
    }
    if (ini.empty())
    {
        wchar_t self[32768];
        std::wstring path(self, GetModuleFileNameW(nullptr, self, 32768));
        ini = path.substr(0, path.find_last_of(L'\\') + 1) + L"JNTracy.ini";
    }
    if (!silent)
    {
        const auto sha = Sha256(ini);
        SetEnvironmentVariableW(L"JN_TRACY_INI_PATH", ini.c_str());
        SetEnvironmentVariableW(L"JN_TRACY_INI_SHA256", sha.c_str());
        SetEnvironmentVariableW(L"JN_TRACY_INI_STATUS", sha.empty() ? L"not_found_compiled_defaults" : L"loaded");
        SetEnvironmentVariableW(L"JN_TRACY_INI_PRESET", L"Fixture");
    }
    WNDCLASSW type{};
    type.lpfnWndProc = Window;
    type.hInstance = instance;
    type.lpszClassName = L"JNTracyIniPlayerFixture";
    RegisterClassW(&type);
    // Visible, as a game's window is, but off screen and never activated.
    CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, type.lpszClassName, L"INI player fixture", WS_POPUP | WS_VISIBLE,
                    -32000, -32000, 1, 1, nullptr, nullptr, instance, nullptr);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
