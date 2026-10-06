#pragma once
/*
Taskbar menu button for native C++ Windhawk mods.

Required metadata:
// @include explorer.exe
// @include ShellExperienceHost.exe
// @architecture x86-64
// @compilerOptions -lshell32 -lshlwapi -luuid -lole32 -loleaut32 -lruntimeobject

Usage:
    #include "TaskbarMenuButton.hpp"
    BOOL Wh_ModInit() {
        return AddTaskbarButton(L"My button",
            [](const std::wstring& exePath, const std::wstring& programName) {
                // Use the resolved executable path and name shown in the menu.
            });
    }

The callback runs in Explorer's private apartment thread with normal user privileges.
This file supplies Windhawk's AfterInit, BeforeUninit and Uninit exports.
No additional source files or downloads are required.
Supported JumpViewUI.dll: 10.0.26100.9549, ADF87E5879A2571AC6566850798F0A6B1.
*/
#include <windhawk_api.h>
#include <functional>
#include <string>
#include <memory>
namespace TaskbarMenu {
using Callback = std::function<void(const std::wstring&, const std::wstring&)>;
}

#undef GetCurrentTime
#include <winstring.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <appmodel.h>
#include <tlhelp32.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Data.h>
#include <winrt/Windows.UI.Xaml.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

using namespace winrt;
using namespace Windows::UI::Xaml;

namespace TaskbarMenu::Detail {
constexpr wchar_t HostClass[] = L"TaskbarMenuButton.Host." WH_MOD_ID;
constexpr wchar_t UiClass[] = L"TaskbarMenuButton.Ui." WH_MOD_ID;
constexpr UINT CleanupMessage = WM_APP + 0x317;
constexpr ULONG_PTR OpenRequest = 0x54464C01;
constexpr ULONG_PTR PathReport = 0x54464C02;
constexpr GUID SupportedImage{0xadf87e58, 0x79a2, 0x571a,
    {0xc6, 0x56, 0x68, 0x50, 0x79, 0x8f, 0x0a, 0x6b}};
std::atomic<bool> Stopping{};
std::mutex RegistrationMutex;
std::wstring ButtonName;
TaskbarMenu::Callback Action;
std::atomic<bool> Initialized{};
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
PathFunction ReadTargetAppId{};
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

/// Returns registered text without sharing mutable storage with the UI thread.
std::wstring GetButtonName() {
    std::lock_guard lock(RegistrationMutex);
    return ButtonName;
}
/// Copies the action so it executes outside the registration lock.
TaskbarMenu::Callback GetAction() {
    std::lock_guard lock(RegistrationMutex);
    return Action;
}

/// Resolves a desktop shortcut when Windows supplies a shortcut instead of an executable.
std::wstring ResolveExecutable(std::wstring path) {
    constexpr std::wstring_view applicationPrefix = L"app-id:";
    if (path.starts_with(applicationPrefix)) {
        std::wstring applicationId = path.substr(applicationPrefix.size());
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) throw_last_error();
        PROCESSENTRY32W entry{sizeof(entry)};
        std::wstring executable;
        if (Process32FirstW(snapshot, &entry)) {
            do {
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
                if (!process) continue;
                UINT32 size = 0;
                if (GetApplicationUserModelId(process, &size, nullptr) == ERROR_INSUFFICIENT_BUFFER) {
                    std::wstring processApplicationId(size, L'\0');
                    if (GetApplicationUserModelId(process, &size, processApplicationId.data()) == ERROR_SUCCESS &&
                        _wcsicmp(processApplicationId.c_str(), applicationId.c_str()) == 0) {
                        executable.resize(32768);
                        DWORD length = static_cast<DWORD>(executable.size());
                        if (QueryFullProcessImageNameW(process, 0, executable.data(), &length))
                            executable.resize(length);
                        else executable.clear();
                    }
                }
                CloseHandle(process);
                if (!executable.empty()) break;
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        if (executable.empty()) throw_hresult(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
        path = executable;
    }
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
    if (attributes == INVALID_FILE_ATTRIBUTES) throw_last_error();
    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        throw_hresult(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    return path;
}


/// Receives bounded UTF-16 requests in Explorer, outside the shell UI's app sandbox.
LRESULT CALLBACK HostProcedure(HWND window, UINT message, WPARAM sender, LPARAM parameter) {
    if (message == WM_COPYDATA && !Stopping) {
        auto packet = reinterpret_cast<const COPYDATASTRUCT*>(parameter);
        wchar_t senderClass[256]{};
        if (!sender || !GetClassNameW(reinterpret_cast<HWND>(sender),
            senderClass, ARRAYSIZE(senderClass)) || wcscmp(senderClass, UiClass) != 0 ||
            !packet || !packet->lpData || packet->cbData < sizeof(wchar_t) ||
            packet->cbData > 131072 || packet->cbData % sizeof(wchar_t))
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
            Wh_SetStringValue(L"LastRequestedPath", text);
            Wh_SetIntValue(L"LastOpenSucceeded", 0);
            auto separator = std::find(text, text + length, L'\0');
            if (separator == text + length || separator + 1 >= text + length)
                return FALSE;
            std::wstring programName(separator + 1, text + length - 1);
            auto executable = ResolveExecutable(std::wstring(text, separator));
            auto callback = GetAction();
            callback(executable, programName);
            Wh_SetStringValue(L"LastActionExecutable", executable.c_str());
            Wh_SetStringValue(L"LastActionProgramName", programName.c_str());
            return TRUE;
        } catch (const hresult_error& error) {
            Wh_Log(L"Executing the taskbar callback failed: %08X %s",
                static_cast<UINT>(error.code()), error.message().c_str());
            Wh_SetIntValue(L"LastOpenError", error.code());
        } catch (...) {
            Wh_Log(L"Executing the taskbar callback failed with an unexpected exception");
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
    FrameworkElement Section{nullptr};
    Controls::Panel Parent{nullptr};
    Controls::StackPanel Wrapper{nullptr};
    Controls::Button Button{nullptr};
    Controls::TextBlock Label{nullptr};
    std::shared_ptr<std::wstring> ProgramName;
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
            row.Parent.Children().InsertAt(index, row.Section);
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

/// Reads the application's own display label from the existing system-command list.
std::wstring ReadVisibleProgramName(const Controls::ListViewBase& list) {
    if (!list.Items().Size()) throw_hresult(E_UNEXPECTED);
    auto item = list.Items().GetAt(0);
    auto provider = item.as<Data::ICustomPropertyProvider>();
    auto property = provider.GetCustomProperty(L"DisplayName");
    if (!property) throw_hresult(E_NOINTERFACE);
    auto name = unbox_value<hstring>(property.GetValue(item));
    if (name.empty()) throw_hresult(E_UNEXPECTED);
    return std::wstring(name);
}
/// Adds one native button beside the existing list and refreshes its target on reuse.
bool EnsureMenuRow(void* rawFrame) {
    if (Stopping || Updating) return false;
    struct UpdateGuard {
        UpdateGuard() { Updating = true; }
        ~UpdateGuard() { Updating = false; }
    } guard;
    try {
        Wh_SetStringValue(L"MenuStage", L"enter");
        auto window = Window::Current();
        if (!window || !window.Content()) {
            Wh_SetStringValue(L"MenuStage", L"no-current-window");
            return false;
        }
        auto frame = window.Content().try_as<FrameworkElement>();
        if (!frame) return false;
        if (auto control = frame.try_as<Controls::Control>()) control.ApplyTemplate();
        auto list = FindSystemList(frame);
        if (!list) {
            for (auto popup : Media::VisualTreeHelper::GetOpenPopups(window)) {
                list = FindSystemList(popup.Child());
                if (list) break;
            }
        }
        if (!list) {
            Wh_SetStringValue(L"MenuStage", L"system-list-not-found");
            Wh_SetStringValue(L"WindowContentClass", get_class_name(frame).c_str());
            return false;
        }
        auto& state = GetUiState();
        auto programName = ReadVisibleProgramName(list);
        auto found = std::find_if(state.Rows.begin(), state.Rows.end(),
            [&](const MenuRow& row) { return row.List == list; });
        if (found != state.Rows.end()) {
            *found->ProgramName = programName;
            found->Button.Tag(box_value(hstring(TargetPath)));
            found->Button.IsEnabled(!TargetPath.empty());
            found->Label.Text(GetButtonName());
            return false;
        }
        FrameworkElement section = list;
        auto parentObject = Media::VisualTreeHelper::GetParent(section);
        Controls::Panel parent{nullptr};
        for (unsigned depth = 0; parentObject && depth < 12; ++depth) {
            parent = parentObject.try_as<Controls::Panel>();
            if (parent) break;
            auto ancestor = parentObject.try_as<FrameworkElement>();
            if (!ancestor) break;
            section = ancestor;
            parentObject = Media::VisualTreeHelper::GetParent(section);
        }
        if (!parent) {
            Wh_SetStringValue(L"MenuStage", L"list-parent-is-not-panel");
            return false;
        }
        uint32_t index{};
        if (!parent.Children().IndexOf(section, index)) return false;
        MenuRow row;
        row.ProgramName = std::make_shared<std::wstring>(programName);
        row.List = list;
        row.Section = section;
        row.Parent = parent;
        row.Button = Controls::Button();
        row.Label = Controls::TextBlock();
        row.Label.Text(GetButtonName());
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
        row.ClickToken = row.Button.Click([weakFrame, rawFrame, endpoint, programName = row.ProgramName](auto&& sender, auto&&) {
            if (Stopping) return;
            try {
                auto button = sender.template as<Controls::Button>();
                std::wstring path(unbox_value<hstring>(button.Tag()));
                if (SendPath(endpoint, OpenRequest, path + L'\0' + *programName) && weakFrame.get())
                    DismissView(rawFrame, nullptr);
            } catch (const hresult_error& error) {
                Wh_Log(L"Menu action failed: %08X", static_cast<UINT>(error.code()));
            } catch (...) { Wh_Log(L"Menu action failed with an unexpected exception"); }
        });
        row.Wrapper = Controls::StackPanel();
        Controls::Grid::SetRow(row.Wrapper, Controls::Grid::GetRow(section));
        Controls::Grid::SetColumn(row.Wrapper, Controls::Grid::GetColumn(section));
        Controls::Grid::SetRowSpan(row.Wrapper, Controls::Grid::GetRowSpan(section));
        Controls::Grid::SetColumnSpan(row.Wrapper, Controls::Grid::GetColumnSpan(section));
        parent.Children().RemoveAt(index);
        try {
            row.Wrapper.Children().Append(row.Button);
            row.Wrapper.Children().Append(section);
            parent.Children().InsertAt(index, row.Wrapper);
        } catch (...) {
            row.Button.Click(row.ClickToken);
            row.Wrapper.Children().Clear();
            parent.Children().InsertAt(index, section);
            throw;
        }
        state.Rows.push_back(std::move(row));
        SendPath(endpoint, PathReport, TargetPath);
        Wh_Log(L"Inserted the file-location command");
        Wh_SetStringValue(L"MenuStage", L"inserted");
        return true;
    } catch (const hresult_error& error) {
        Wh_SetStringValue(L"MenuStage", error.message().c_str());
        Wh_Log(L"Adding the menu row failed: %08X %s", static_cast<UINT>(error.code()), error.message().c_str());
    } catch (...) { Wh_Log(L"Adding the menu row failed with an unexpected exception"); }
    return false;
}

/// Obtains the path from the same Windows session which populates the clicked app's menu.
void WINAPI InitializeHook(void* model, void* list, void* session) {
    Wh_SetStringValue(L"InitializeHook", L"called");
    InitializeOriginal(model, list, session);
    TargetPath.clear();
    if (Stopping) return;
    try {
        hstring path{ReadTargetPath(session), take_ownership_from_abi};
        TargetPath = path;
        hstring applicationId{ReadTargetAppId(session), take_ownership_from_abi};
        if (std::wstring_view(applicationId).find(L'!') != std::wstring_view::npos)
            TargetPath = L"app-id:" + std::wstring(applicationId);
        Wh_Log(L"Menu target: %s", TargetPath.c_str());
    } catch (...) { Wh_Log(L"Reading the menu target failed"); }
}

void WINAPI UpdateHook(void* frame) {
    Wh_SetStringValue(L"UpdateHook", L"called");
    EnsureMenuRow(frame);
    UpdateOriginal(frame);
}

void WINAPI VisibilityHook(void* frame, void* sender, void* arguments) {
    Wh_SetStringValue(L"VisibilityHook", L"called");
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
    ReadTargetAppId = reinterpret_cast<PathFunction>(base + 0xb7b20);
    DismissView = reinterpret_cast<DismissFunction>(base + 0x663c0);
    if (!Wh_SetFunctionHook(base + 0xf08a0, reinterpret_cast<void*>(InitializeHook),
            reinterpret_cast<void**>(&InitializeOriginal)) ||
        !Wh_SetFunctionHook(base + 0x695f0, reinterpret_cast<void*>(UpdateHook),
            reinterpret_cast<void**>(&UpdateOriginal)) ||
        !Wh_SetFunctionHook(base + 0x67db0, reinterpret_cast<void*>(VisibilityHook),
            reinterpret_cast<void**>(&VisibilityOriginal)))
        return false;
    Hooked = true;
    Wh_SetStringValue(L"JumpViewHooks", L"installed");
    return true;
}

HMODULE WINAPI LoadLibraryHook(LPCWSTR name, HANDLE file, DWORD flags) {
    auto module = LoadLibraryOriginal(name, file, flags);
    if (!Stopping && !Hooked && module && module == GetModuleHandleW(L"JumpViewUI.dll"))
        if (HookJumpView(module)) Wh_ApplyHookOperations();
    return module;
}


bool Initialize() {
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

void BeforeUninitialize() {
    Stopping = true;
    if (!IsExplorer) {
        std::vector<HWND> endpoints;
        { std::lock_guard lock(UiWindowsMutex); endpoints = UiWindows; }
        for (HWND window : endpoints)
            if (IsWindow(window)) SendMessageW(window, CleanupMessage, 0, 0);
        UnregisterClassW(UiClass, ModuleInstance());
    }
}

void Uninitialize() {
    if (HostWindow) SendMessageW(HostWindow, WM_CLOSE, 0, 0);
    if (HostThread) {
        WaitForSingleObject(HostThread, INFINITE);
        CloseHandle(HostThread);
        HostThread = nullptr;
    }
}


void AfterInitialize() {
    if (!IsExplorer && !Hooked)
        if (auto module = GetModuleHandleW(L"JumpViewUI.dll"))
            if (HookJumpView(module)) Wh_ApplyHookOperations();
}
} // namespace TaskbarMenu::Detail

/// Registers one button; a repeated call updates its text and callback.
/// @param buttonName Text displayed in the native taskbar app menu.
/// @param callback Receives the resolved executable path and the app's visible name.
/// @return True if initialization succeeded; false if the shell version is unsupported.
inline bool AddTaskbarButton(std::wstring buttonName, TaskbarMenu::Callback callback) {
    using namespace TaskbarMenu::Detail;
    if (buttonName.empty() || !callback) return false;
    {
        std::lock_guard lock(RegistrationMutex);
        ButtonName = std::move(buttonName);
        Action = std::move(callback);
    }
    if (Initialized.exchange(true)) return true;
    if (Initialize()) return true;
    Initialized = false;
    return false;
}

void Wh_ModAfterInit() { TaskbarMenu::Detail::AfterInitialize(); }
void Wh_ModBeforeUninit() { TaskbarMenu::Detail::BeforeUninitialize(); }
void Wh_ModUninit() { TaskbarMenu::Detail::Uninitialize(); }
