// ==WindhawkMod==
// @id              taskbar-file-location
// @name            Dosya Konumunu Aç
// @name:en-US      Open File Location
// @description     Opens the app executable's folder from its existing taskbar menu
// @version         1.1.0
// @author          Portakal
// @license         MIT
// @include         explorer.exe
// @include         ShellExperienceHost.exe
// @architecture    x86-64
// @compilerOptions -lcomctl32 -lshell32 -lshlwapi -luuid -lole32 -loleaut32 -lruntimeobject
// ==/WindhawkMod==

// ==WindhawkModSettings==
/*
- language: tr
  $name: Language
  $name:tr-TR: Dil
  $options:
  - tr: Türkçe
  - en: English
*/
// ==/WindhawkModSettings==

#include "../include/TaskbarMenuButton.hpp"

namespace {
/// Opens the containing directory in File Explorer, using an explicit Explorer fallback.
void OpenExecutableFolder(const std::wstring& executable) {
    auto separator = executable.find_last_of(L"\\/");
    if (separator == std::wstring::npos) throw_hresult(E_INVALIDARG);
    std::wstring directory = executable.substr(0, separator == 2 ? 3 : separator);
    PIDLIST_ABSOLUTE folder{};
    HRESULT result = SHParseDisplayName(directory.c_str(), nullptr, &folder, 0, nullptr);
    if (SUCCEEDED(result)) {
        result = SHOpenFolderAndSelectItems(folder, 0, nullptr, 0);
        CoTaskMemFree(folder);
    }
    if (FAILED(result)) {
        Wh_Log(L"Shell folder open failed: %08X. Trying Explorer.", static_cast<UINT>(result));
        wchar_t windowsDirectory[MAX_PATH]{};
        if (!GetWindowsDirectoryW(windowsDirectory, ARRAYSIZE(windowsDirectory)))
            throw_last_error();
        std::wstring explorer = std::wstring(windowsDirectory) + L"\\explorer.exe";
        std::wstring arguments = L"\"" + directory + L"\"";
        SHELLEXECUTEINFOW execution{sizeof(execution)};
        execution.fMask = SEE_MASK_FLAG_NO_UI;
        execution.lpFile = explorer.c_str();
        execution.lpParameters = arguments.c_str();
        execution.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&execution)) throw_last_error();
    }
    Wh_SetStringValue(L"LastOpenedDirectory", directory.c_str());
    Wh_SetIntValue(L"LastOpenSucceeded", 1);
    Wh_Log(L"Opened directory: %s", directory.c_str());
}

/// Supplies localized text and the app-specific action in one registration call.
bool RegisterFolderButton() {
    auto language = Wh_GetStringSetting(L"language");
    std::wstring name = language && _wcsicmp(language, L"en") == 0
        ? L"Open File Location" : L"Dosya Konumunu Aç";
    Wh_FreeStringSetting(language);
    return AddTaskbarButton(std::move(name),
        [](const std::wstring& exePath, const std::wstring& programName) {
            OpenExecutableFolder(exePath);
            Wh_Log(L"Opened %s executable directory", programName.c_str());
        });
}
}
BOOL Wh_ModInit() { return RegisterFolderButton(); }
void Wh_ModSettingsChanged() { RegisterFolderButton(); }
