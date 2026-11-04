// config.ini next to the program or DLL: the folders a user's install has. The starter asks
// for them once, with a folder picker, and the DLL reads the same file.
//
//   [paths]
//   mods = C:\...\XXMI Launcher\EFMI\Mods
//   launcher = C:\...\GRYPHLINK
#pragma once
#include <windows.h>
#include <shobjidl.h>

#include <string>

namespace settings {

// The folder of the module (exe or DLL) this code is in.
inline std::wstring ModuleDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dir = path;
    return dir.substr(0, dir.find_last_of(L'\\'));
}

inline std::wstring File() { return ModuleDir() + L"\\config.ini"; }

inline std::wstring Get(const wchar_t* key) {
    wchar_t value[MAX_PATH * 2]{};
    GetPrivateProfileStringW(L"paths", key, L"", value, MAX_PATH * 2, File().c_str());
    return value;
}

inline void Set(const wchar_t* key, const std::wstring& value) { WritePrivateProfileStringW(L"paths", key, value.c_str(), File().c_str()); }

inline bool IsDir(const std::wstring& path) {
    const DWORD a = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool IsFile(const std::wstring& path) {
    const DWORD a = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// A folder picker; empty when cancelled.
inline std::wstring PickFolder(const wchar_t* title) {
    std::wstring result;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->SetTitle(title);
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->Show(nullptr)) && SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
        dialog->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
}

// The saved folder for key when check(folder) holds; else the first candidate that does;
// else asked for (until check holds or the user cancels). Saved once found.
template <typename Check>
std::wstring Folder(const wchar_t* key, const wchar_t* title, std::initializer_list<std::wstring> candidates, Check check) {
    std::wstring dir = Get(key);
    if (check(dir)) return dir;
    for (const std::wstring& c : candidates) {
        if (check(c)) {
            Set(key, c);
            return c;
        }
    }
    for (;;) {
        dir = PickFolder(title);
        if (dir.empty()) return {};
        if (check(dir)) {
            Set(key, dir);
            return dir;
        }
        MessageBoxW(nullptr, (dir + L"\n\nis not the right folder. Please pick it again.").c_str(), title, MB_OK | MB_ICONWARNING);
    }
}

inline std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(buf) : std::wstring();
}

}  // namespace settings
