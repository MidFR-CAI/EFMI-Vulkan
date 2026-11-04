// Turns the layer on for one game session. The Vulkan loader loads implicit layers into
// every Vulkan program, other games with their own anti-cheats included, so the layer
// is registered only from here to when Endfield exits:
//
//   1. adds efmivulkan.json under HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers
//   2. opens the official Gryphline launcher (the game refuses to start any other way)
//   3. waits for Endfield.exe to start and to exit
//   4. removes the entry again, also on Ctrl+C or when this window is closed
//
// HKLM, not HKCU: Endfield runs as administrator, and in an elevated program the loader
// ignores layers registered for the current user. Writing HKLM needs admin rights, so
// this asks for them (as XXMI's launch does).
//
// Everything it does goes to %USERPROFILE%\efmivulkan\launch.log as well.
//
//   efmivulkan_launch               all of the above
//   efmivulkan_launch --register    only step 1, until --unregister
//   efmivulkan_launch --unregister
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

#include "../src/settings.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

#include <cstdarg>
#include <cstdio>
#include <string>

static const wchar_t kLayersKey[] = L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers";
static const wchar_t kProcess[] = L"Endfield.exe";
static const wchar_t kLauncherExe[] = L"Launcher.exe";  // GRYPHLINK's, in the folder config.ini names.
static const DWORD kLayerCheckMs = 60000;  // By then the game has created its Vulkan instance.

static std::wstring g_manifest;
static std::wstring g_logDir;
static FILE* g_log = nullptr;

// To the console and launch.log.
static void Say(const wchar_t* fmt, ...) {
    wchar_t text[1024];
    va_list args;
    va_start(args, fmt);
    vswprintf(text, 1024, fmt, args);
    va_end(args);
    wprintf(L"%s\n", text);
    if (g_log) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fwprintf(g_log, L"%02d:%02d:%02d %s\n", t.wHour, t.wMinute, t.wSecond, text);
        fflush(g_log);
    }
}

static std::wstring Manifest() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    return dir.substr(0, dir.find_last_of(L'\\') + 1) + L"efmivulkan.json";
}

static std::wstring LogDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"EFMIVULKAN_LOG_DIR", buf, MAX_PATH);
    if (n && n < MAX_PATH) return buf;
    GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    return std::wstring(buf) + L"\\efmivulkan";
}

static bool Register() {
    HKEY key;
    LSTATUS s = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kLayersKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (s != ERROR_SUCCESS) {
        Say(L"Could not open HKLM\\%s (error %ld). Run this as administrator.", kLayersKey, s);
        return false;
    }
    const DWORD enabled = 0;  // 0 means enabled for the loader.
    s = RegSetValueExW(key, g_manifest.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&enabled), sizeof(enabled));
    RegCloseKey(key);
    if (s != ERROR_SUCCESS) Say(L"Could not register the layer (error %ld).", s);
    return s == ERROR_SUCCESS;
}

static void Unregister() {
    // HKCU too: the first version of this tool registered there.
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        HKEY key;
        if (RegOpenKeyExW(root, kLayersKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) continue;
        RegDeleteValueW(key, g_manifest.c_str());
        RegCloseKey(key);
    }
}

static BOOL WINAPI OnConsoleClose(DWORD) {
    Unregister();
    return FALSE;
}

static DWORD FindProcess() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    DWORD pid = 0;
    if (Process32FirstW(snap, &e)) {
        do {
            if (_wcsicmp(e.szExeFile, kProcess) == 0) {
                pid = e.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &e));
    }
    CloseHandle(snap);
    return pid;
}

// "yes", "no", or "unknown" when the game does not let us ask (a limited query only).
static const wchar_t* Elevated(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return L"unknown";
    const wchar_t* answer = L"unknown";
    HANDLE token;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation{};
        DWORD size = 0;
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) answer = elevation.TokenIsElevated ? L"yes" : L"no";
        CloseHandle(token);
    }
    CloseHandle(process);
    return answer;
}

// Whether the layer started logging since the game did: its log is (re)written then.
static bool LayerLogged(const FILETIME& since) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW((g_logDir + L"\\efmivulkan.log").c_str(), GetFileExInfoStandard, &data)) return false;
    return CompareFileTime(&data.ftLastWriteTime, &since) >= 0;
}

int wmain(int argc, wchar_t** argv) {
    g_manifest = Manifest();
    g_logDir = LogDir();
    CreateDirectoryW(g_logDir.c_str(), nullptr);
    _wfopen_s(&g_log, (g_logDir + L"\\launch.log").c_str(), L"w, ccs=UTF-8");
    if (GetFileAttributesW(g_manifest.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Say(L"not found: %s", g_manifest.c_str());
        return 1;
    }
    if (argc > 1 && wcscmp(argv[1], L"--unregister") == 0) {
        Unregister();
        Say(L"Layer unregistered.");
        return 0;
    }
    Unregister();  // A leftover from an earlier run, under either root.
    if (!Register()) return 1;
    if (argc > 1 && wcscmp(argv[1], L"--register") == 0) {
        Say(L"Layer registered (HKLM) until efmivulkan_launch --unregister: %s", g_manifest.c_str());
        return 0;
    }
    // The folders, asked for once and kept in config.ini.
    const std::wstring appData = settings::Env(L"APPDATA"), programs = settings::Env(L"ProgramFiles");
    const std::wstring mods = settings::Folder(L"mods", L"EFMIVulkan: select your EFMI Mods folder (XXMI Launcher\\EFMI\\Mods)",
                                               {appData + L"\\XXMI Launcher\\EFMI\\Mods"}, settings::IsDir);
    if (mods.empty()) {
        Say(L"No Mods folder chosen; nothing to do.");
        Unregister();
        return 1;
    }
    Say(L"Mods: %s", mods.c_str());
    const std::wstring launcher = settings::Folder(L"launcher", L"EFMIVulkan: select the GRYPHLINK folder (the Endfield launcher, with Launcher.exe)",
                                                   {programs + L"\\GRYPHLINK", L"C:\\GRYPHLINK"},
                                                   [](const std::wstring& d) { return settings::IsFile(d + L"\\" + kLauncherExe); });
    SetConsoleCtrlHandler(OnConsoleClose, TRUE);
    Say(L"Layer registered (HKLM) for this session: %s", g_manifest.c_str());

    if (!FindProcess()) {
        const std::wstring exe = launcher + L"\\" + kLauncherExe;
        if (launcher.empty() ||
            reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, launcher.c_str(), SW_SHOWNORMAL)) <= 32) {
            Say(L"Could not open %s; start the game from the official launcher yourself.", exe.c_str());
        }
        Say(L"Start the game in the official launcher. Leave \"Launch with DirectX 11\" unticked: the game runs on Vulkan.");
    }
    FILETIME started;
    DWORD pid = 0;
    while (!(pid = FindProcess())) Sleep(250);
    GetSystemTimeAsFileTime(&started);
    Say(L"Endfield is running (pid %lu, elevated: %s). Press F10 in game to capture a frame.", pid, Elevated(pid));

    // One check that the loader took the layer, then wait for the game to exit.
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    const DWORD waited = process ? WaitForSingleObject(process, kLayerCheckMs) : (Sleep(kLayerCheckMs), WAIT_TIMEOUT);
    if (waited == WAIT_TIMEOUT) {
        if (LayerLogged(started)) {
            Say(L"The layer is running in the game (efmivulkan.log is being written).");
        } else {
            Say(L"The layer has NOT started in the game after %lu s: no new efmivulkan.log.", kLayerCheckMs / 1000);
        }
    }
    if (process) {
        WaitForSingleObject(process, INFINITE);
        CloseHandle(process);
    } else {
        while (FindProcess()) Sleep(1000);
    }
    // The game may restart itself once through its launcher: give it a moment.
    Sleep(5000);
    if (DWORD again = FindProcess()) {
        Say(L"Endfield restarted (pid %lu); waiting for it to exit.", again);
        while (FindProcess()) Sleep(1000);
    }
    Unregister();
    Say(L"Endfield exited; layer unregistered.");
    if (g_log) fclose(g_log);
    return 0;
}
