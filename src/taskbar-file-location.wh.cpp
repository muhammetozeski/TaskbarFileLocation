// ==WindhawkMod==
// @id              taskbar-file-location
// @name            Dosya Konumunu Aç
// @name:en-US      Open File Location
// @description     Opens the app executable's folder from its existing taskbar menu
// @version         1.0.0
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

#undef GetCurrentTime
#include <winstring.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

using namespace winrt;
using namespace Windows::UI::Xaml;

namespace {
constexpr wchar_t HostClass[] = L"TaskbarFileLocation.Host";
constexpr wchar_t UiClass[] = L"TaskbarFileLocation.Ui";
constexpr UINT CleanupMessage = WM_APP + 0x317;
constexpr ULONG_PTR OpenRequest = 0x54464C01;
constexpr ULONG_PTR PathReport = 0x54464C02;
constexpr GUID SupportedImage{0xadf87e58, 0x79a2, 0x571a,
    {0xc6, 0x56, 0x68, 0x50, 0x79, 0x8f, 0x0a, 0x6b}};
std::atomic<bool> Stopping{};
std::atomic<bool> English{};
std::atomic<bool> Hooked{};
std::atomic<HWND> HostWindow{};
HANDLE HostThread{};
bool IsExplorer{};
std::mutex UiWindowsMutex;
std::vector<HWND> UiWindows;
thread_local std::wstring TargetPath;
thread_local bool Updating{};

using InitializeFunction = void(WINAPI*)(void*, void*, void*);
using UpdateFunction = void(WINAPI*)(void*);
using VisibilityFunction = void(WINAPI*)(void*, void*, void*);
using PathFunction = HSTRING(WINAPI*)(void*);
using DismissFunction = void(WINAPI*)(void*, void*);
InitializeFunction InitializeOriginal{};
UpdateFunction UpdateOriginal{};
VisibilityFunction VisibilityOriginal{};
PathFunction ReadTargetPath{};
DismissFunction DismissView{};
decltype(&LoadLibraryExW) LoadLibraryOriginal{};

/// Returns this extension's module rather than the host executable.
HINSTANCE ModuleInstance() {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ModuleInstance), &module);
    return module;
}

/// Reads the language preference from Windhawk's existing portable settings.
void LoadSettings() {
    auto language = Wh_GetStringSetting(L"language");
    English = language && _wcsicmp(language, L"en") == 0;
    Wh_FreeStringSetting(language);
}

/// Resolves a desktop shortcut when Windows supplies a shortcut instead of an executable.
std::wstring ResolveExecutable(std::wstring path) {
    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"')
        path = path.substr(1, path.size() - 2);
    if (_wcsicmp(PathFindExtensionW(path.c_str()), L".lnk") == 0) {
        com_ptr<IShellLinkW> link;
        check_hresult(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_IShellLinkW, link.put_void()));
        check_hresult(link.as<IPersistFile>()->Load(path.c_str(), STGM_READ));
        wchar_t target[32768]{};
        check_hresult(link->GetPath(target, ARRAYSIZE(target), nullptr, SLGP_RAWPATH));
        path = target;
    }
    wchar_t expanded[32768]{};
    DWORD length = ExpandEnvironmentStringsW(path.c_str(), expanded, ARRAYSIZE(expanded));
    if (!length || length > ARRAYSIZE(expanded)) throw_hresult(E_INVALIDARG);
    path = expanded;
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
        throw_hresult(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    return path;
}

/// Opens the containing directory in File Explorer, using an explicit Explorer fallback.
void OpenExecutableFolder(const std::wstring& input) {
    auto executable = ResolveExecutable(input);
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

/// Receives bounded UTF-16 requests in Explorer, outside the shell UI's app sandbox.
LRESULT CALLBACK HostProcedure(HWND window, UINT message, WPARAM sender, LPARAM parameter) {
    if (message == WM_COPYDATA && !Stopping) {
        auto packet = reinterpret_cast<const COPYDATASTRUCT*>(parameter);
        wchar_t senderClass[64]{};
        if (!sender || !GetClassNameW(reinterpret_cast<HWND>(sender),
            senderClass, ARRAYSIZE(senderClass)) || wcscmp(senderClass, UiClass) != 0 ||
            !packet || !packet->lpData || packet->cbData < sizeof(wchar_t) ||
            packet->cbData > 65536 || packet->cbData % sizeof(wchar_t))
            return FALSE;
        auto text = static_cast<const wchar_t*>(packet->lpData);
        auto length = packet->cbData / sizeof(wchar_t);
        if (text[length - 1] != L'\0') return FALSE;
        if (packet->dwData == PathReport) {
            Wh_SetStringValue(L"LastTargetPath", text);
            return TRUE;
        }
        if (packet->dwData != OpenRequest) return FALSE;
        try {
            Wh_SetIntValue(L"LastOpenSucceeded", 0);
            OpenExecutableFolder(std::wstring(text, length - 1));
            return TRUE;
        } catch (const hresult_error& error) {
            Wh_Log(L"Opening the executable directory failed: %08X %s",
                static_cast<UINT>(error.code()), error.message().c_str());
            Wh_SetIntValue(L"LastOpenError", error.code());
        } catch (...) {
            Wh_Log(L"Opening the executable directory failed with an unexpected exception");
        }
        return FALSE;
    }
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, sender, parameter);
}

/// Hosts folder-opening requests on an apartment thread with no visible window.
DWORD WINAPI HostThreadProcedure(void* ready) {
    HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSW registration{};
    registration.hInstance = ModuleInstance();
    registration.lpszClassName = HostClass;
    registration.lpfnWndProc = HostProcedure;
    if (SUCCEEDED(apartment) && RegisterClassW(&registration)) {
        HostWindow = CreateWindowExW(0, HostClass, nullptr, 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, registration.hInstance, nullptr);
        if (HostWindow)
            ChangeWindowMessageFilterEx(HostWindow, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    }
    SetEvent(static_cast<HANDLE>(ready));
    if (HostWindow) {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
            DispatchMessageW(&message);
    }
    HostWindow = nullptr;
    UnregisterClassW(HostClass, registration.hInstance);
    if (SUCCEEDED(apartment)) CoUninitialize();
    return 0;
}

struct MenuRow {
    Controls::ListViewBase List{nullptr};
    Controls::Panel Parent{nullptr};
    Controls::StackPanel Wrapper{nullptr};
    Controls::Button Button{nullptr};
    Controls::TextBlock Label{nullptr};
    event_token ClickToken{};
};
struct UiState { HWND Window{}; std::vector<MenuRow> Rows; };
thread_local UiState* CurrentUi{};

/// Restores the list and releases all callbacks on the XAML thread before unloading.
void RemoveRows(UiState& state) {
    for (auto& row : state.Rows) {
        row.Button.Click(row.ClickToken);
        uint32_t index{};
        if (row.Parent.Children().IndexOf(row.Wrapper, index)) {
            row.Wrapper.Children().Clear();
            row.Parent.Children().RemoveAt(index);
            row.Parent.Children().InsertAt(index, row.List);
        }
    }
    state.Rows.clear();
}

/// Provides a cleanup endpoint on the thread which owns the menu controls.
LRESULT CALLBACK UiProcedure(HWND window, UINT message, WPARAM word, LPARAM parameter) {
    if (message == CleanupMessage) {
        if (CurrentUi) {
            try { RemoveRows(*CurrentUi); }
            catch (const hresult_error& error) { Wh_Log(L"Menu cleanup failed: %08X", static_cast<UINT>(error.code())); }
            delete CurrentUi;
            CurrentUi = nullptr;
        }
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, word, parameter);
}

/// Creates a thread-local message endpoint without driving or clicking the desktop UI.
UiState& GetUiState() {
    if (CurrentUi) return *CurrentUi;
    WNDCLASSW registration{};
    registration.hInstance = ModuleInstance();
    registration.lpszClassName = UiClass;
    registration.lpfnWndProc = UiProcedure;
    if (!RegisterClassW(&registration) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw_last_error();
    auto state = new UiState;
    state->Window = CreateWindowExW(0, UiClass, nullptr, 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, registration.hInstance, nullptr);
    if (!state->Window) { delete state; throw_last_error(); }
    CurrentUi = state;
    std::lock_guard lock(UiWindowsMutex);
    UiWindows.push_back(state->Window);
    return *state;
}

/// Sends a path to this extension's Explorer endpoint; the shell sandbox opens no process itself.
bool SendPath(HWND sender, ULONG_PTR request, const std::wstring& path) {
    auto receiver = FindWindowExW(HWND_MESSAGE, nullptr, HostClass, nullptr);
    if (!receiver || path.size() >= 32768) return false;
    COPYDATASTRUCT packet{request, static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)),
        const_cast<wchar_t*>(path.c_str())};
    return SendMessageW(receiver, WM_COPYDATA, reinterpret_cast<WPARAM>(sender),
        reinterpret_cast<LPARAM>(&packet)) != 0;
}

/// Finds the existing system-command list without replacing its items or data source.
Controls::ListViewBase FindSystemList(const DependencyObject& root, unsigned depth = 0) {
    if (!root || depth > 24) return nullptr;
    if (get_class_name(root) == L"JumpViewUI.SystemItemListView")
        return root.try_as<Controls::ListViewBase>();
    int count = Media::VisualTreeHelper::GetChildrenCount(root);
    for (int index = 0; index < count; ++index)
        if (auto result = FindSystemList(Media::VisualTreeHelper::GetChild(root, index), depth + 1))
            return result;
    return nullptr;
}

/// Adds one native button beside the existing list and refreshes its target on reuse.
bool EnsureMenuRow(void* rawFrame) {
    if (Stopping || Updating) return false;
    struct UpdateGuard {
        UpdateGuard() { Updating = true; }
        ~UpdateGuard() { Updating = false; }
    } guard;
    try {
        Windows::Foundation::IInspectable object{nullptr};
        copy_from_abi(object, rawFrame);
        auto frame = object.as<FrameworkElement>();
        if (auto control = frame.try_as<Controls::Control>()) control.ApplyTemplate();
        auto list = FindSystemList(frame);
        if (!list) return false;
        auto& state = GetUiState();
        auto found = std::find_if(state.Rows.begin(), state.Rows.end(),
            [&](const MenuRow& row) { return row.List == list; });
        if (found != state.Rows.end()) {
            found->Button.Tag(box_value(hstring(TargetPath)));
            found->Button.IsEnabled(!TargetPath.empty());
            found->Label.Text(English ? L"Open File Location" : L"Dosya Konumunu Aç");
            return false;
        }
        auto parent = Media::VisualTreeHelper::GetParent(list).try_as<Controls::Panel>();
        if (!parent) return false;
        uint32_t index{};
        if (!parent.Children().IndexOf(list, index)) return false;
        MenuRow row;
        row.List = list;
        row.Parent = parent;
        row.Button = Controls::Button();
        row.Label = Controls::TextBlock();
        row.Label.Text(English ? L"Open File Location" : L"Dosya Konumunu Aç");
        row.Label.FontSize(14);
        row.Label.VerticalAlignment(VerticalAlignment::Center);
        Controls::FontIcon icon;
        icon.FontFamily(Media::FontFamily(L"Segoe Fluent Icons"));
        icon.Glyph(L"\xE838");
        icon.FontSize(16);
        Controls::StackPanel content;
        content.Orientation(Controls::Orientation::Horizontal);
        content.Spacing(12);
        content.Children().Append(icon);
        content.Children().Append(row.Label);
        row.Button.Content(content);
        row.Button.MinHeight(40);
        row.Button.HorizontalAlignment(HorizontalAlignment::Stretch);
        row.Button.HorizontalContentAlignment(HorizontalAlignment::Left);
        row.Button.Padding({12, 8, 12, 8});
        row.Button.BorderThickness({0, 0, 0, 0});
        row.Button.Background(Media::SolidColorBrush(Windows::UI::Colors::Transparent()));
        row.Button.Tag(box_value(hstring(TargetPath)));
        row.Button.IsEnabled(!TargetPath.empty());
        auto weakFrame = make_weak(frame);
        HWND endpoint = state.Window;
        row.ClickToken = row.Button.Click([weakFrame, rawFrame, endpoint](auto&& sender, auto&&) {
            if (Stopping) return;
            try {
                auto button = sender.template as<Controls::Button>();
                std::wstring path(unbox_value<hstring>(button.Tag()));
                if (SendPath(endpoint, OpenRequest, path) && weakFrame.get())
                    DismissView(rawFrame, nullptr);
            } catch (const hresult_error& error) {
                Wh_Log(L"Menu action failed: %08X", static_cast<UINT>(error.code()));
            } catch (...) { Wh_Log(L"Menu action failed with an unexpected exception"); }
        });
        row.Wrapper = Controls::StackPanel();
        Controls::Grid::SetRow(row.Wrapper, Controls::Grid::GetRow(list));
        Controls::Grid::SetColumn(row.Wrapper, Controls::Grid::GetColumn(list));
        Controls::Grid::SetRowSpan(row.Wrapper, Controls::Grid::GetRowSpan(list));
        Controls::Grid::SetColumnSpan(row.Wrapper, Controls::Grid::GetColumnSpan(list));
        parent.Children().RemoveAt(index);
        try {
            row.Wrapper.Children().Append(row.Button);
            row.Wrapper.Children().Append(list);
            parent.Children().InsertAt(index, row.Wrapper);
        } catch (...) {
            row.Button.Click(row.ClickToken);
            row.Wrapper.Children().Clear();
            parent.Children().InsertAt(index, list);
            throw;
        }
        state.Rows.push_back(std::move(row));
        SendPath(endpoint, PathReport, TargetPath);
        Wh_Log(L"Inserted the file-location command");
        return true;
    } catch (const hresult_error& error) {
        Wh_Log(L"Adding the menu row failed: %08X %s", static_cast<UINT>(error.code()), error.message().c_str());
    } catch (...) { Wh_Log(L"Adding the menu row failed with an unexpected exception"); }
    return false;
}

/// Obtains the path from the same Windows session which populates the clicked app's menu.
void WINAPI InitializeHook(void* model, void* list, void* session) {
    InitializeOriginal(model, list, session);
    TargetPath.clear();
    if (Stopping) return;
    try {
        hstring path{ReadTargetPath(session), take_ownership_from_abi};
        TargetPath = path;
        Wh_Log(L"Menu target: %s", TargetPath.c_str());
    } catch (...) { Wh_Log(L"Reading the menu target failed"); }
}

void WINAPI UpdateHook(void* frame) {
    EnsureMenuRow(frame);
    UpdateOriginal(frame);
}

void WINAPI VisibilityHook(void* frame, void* sender, void* arguments) {
    VisibilityOriginal(frame, sender, arguments);
    if (EnsureMenuRow(frame)) UpdateOriginal(frame);
}

/// Accepts only the exact symbol identity inspected during development; no symbol downloads occur.
bool SupportedModule(HMODULE module) {
    auto base = reinterpret_cast<BYTE*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto headers = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE) return false;
    auto directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!directory.VirtualAddress || !directory.Size) return false;
    auto entries = reinterpret_cast<IMAGE_DEBUG_DIRECTORY*>(base + directory.VirtualAddress);
    for (DWORD index = 0; index < directory.Size / sizeof(*entries); ++index) {
        if (entries[index].Type != IMAGE_DEBUG_TYPE_CODEVIEW || entries[index].SizeOfData < 24)
            continue;
        auto record = base + entries[index].AddressOfRawData;
        if (memcmp(record, "RSDS", 4) == 0 &&
            memcmp(record + 4, &SupportedImage, sizeof(GUID)) == 0 &&
            *reinterpret_cast<const DWORD*>(record + 20) == 1)
            return true;
    }
    return false;
}

/// Uses addresses derived from the approved, matching Microsoft symbols.
bool HookJumpView(HMODULE module) {
    if (Hooked || !module) return true;
    if (!SupportedModule(module)) {
        Wh_Log(L"This JumpViewUI symbol identity has not been verified");
        return false;
    }
    auto base = reinterpret_cast<BYTE*>(module);
    ReadTargetPath = reinterpret_cast<PathFunction>(base + 0xb7b80);
    DismissView = reinterpret_cast<DismissFunction>(base + 0x663c0);
    if (!Wh_SetFunctionHook(base + 0xf08a0, reinterpret_cast<void*>(InitializeHook),
            reinterpret_cast<void**>(&InitializeOriginal)) ||
        !Wh_SetFunctionHook(base + 0x695f0, reinterpret_cast<void*>(UpdateHook),
            reinterpret_cast<void**>(&UpdateOriginal)) ||
        !Wh_SetFunctionHook(base + 0x67db0, reinterpret_cast<void*>(VisibilityHook),
            reinterpret_cast<void**>(&VisibilityOriginal)))
        return false;
    Hooked = true;
    return true;
}

HMODULE WINAPI LoadLibraryHook(LPCWSTR name, HANDLE file, DWORD flags) {
    auto module = LoadLibraryOriginal(name, file, flags);
    if (!Stopping && !Hooked && module && module == GetModuleHandleW(L"JumpViewUI.dll"))
        if (HookJumpView(module)) Wh_ApplyHookOperations();
    return module;
}
} // namespace

BOOL Wh_ModInit() {
    LoadSettings();
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    IsExplorer = _wcsicmp(PathFindFileNameW(executable), L"explorer.exe") == 0;
    if (IsExplorer) {
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ready) return FALSE;
        HostThread = CreateThread(nullptr, 0, HostThreadProcedure, ready, 0, nullptr);
        if (HostThread) WaitForSingleObject(ready, INFINITE);
        CloseHandle(ready);
        Wh_SetStringValue(L"HostStatus", HostWindow ? L"ready" : L"failed");
        return HostWindow != nullptr;
    }
    auto loader = GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "LoadLibraryExW");
    if (!loader || !Wh_SetFunctionHook(reinterpret_cast<void*>(loader),
            reinterpret_cast<void*>(LoadLibraryHook), reinterpret_cast<void**>(&LoadLibraryOriginal)))
        return FALSE;
    return HookJumpView(GetModuleHandleW(L"JumpViewUI.dll"));
}

void Wh_ModBeforeUninit() {
    Stopping = true;
    if (!IsExplorer) {
        std::vector<HWND> endpoints;
        { std::lock_guard lock(UiWindowsMutex); endpoints = UiWindows; }
        for (HWND window : endpoints)
            if (IsWindow(window)) SendMessageW(window, CleanupMessage, 0, 0);
        UnregisterClassW(UiClass, ModuleInstance());
    }
}

void Wh_ModUninit() {
    if (HostWindow) SendMessageW(HostWindow, WM_CLOSE, 0, 0);
    if (HostThread) {
        WaitForSingleObject(HostThread, INFINITE);
        CloseHandle(HostThread);
        HostThread = nullptr;
    }
}

void Wh_ModSettingsChanged() { LoadSettings(); }
