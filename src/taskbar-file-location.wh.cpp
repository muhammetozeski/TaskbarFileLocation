// ==WindhawkMod==
// @id              taskbar-file-location
// @name            Dosya Konumunu Aç
// @name:en-US      Open File Location
// @description     Opens the app executable's folder from its existing taskbar menu
// @version         1.3.0
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
- mouse5OpensFolder: true
  $name: Open file location with Mouse 5
  $name:tr-TR: Mouse 5 ile dosya konumunu aç
  $description: Click an app's taskbar icon with the second side button to open its executable folder directly.
  $description:tr-TR: Programın görev çubuğu simgesine farenin ikinci yan düğmesiyle tıklayınca EXE klasörünü doğrudan açar.
*/
// ==/WindhawkModSettings==

#include "../include/TaskbarMenuButton.hpp"
#include <propkey.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Xaml.Input.h>

namespace {
std::atomic<bool> MouseFiveEnabled{true};
std::atomic<bool> MouseHooksInstalled{};
constexpr GUID TaskbarViewImage{0x24534974, 0xe00e, 0x46a5,
    {0x8f, 0x69, 0xf6, 0x0d, 0x0c, 0xeb, 0x9b, 0xd9}};
constexpr GUID AppItemViewModelId{0x02695236, 0x2049, 0x5579,
    {0x9b, 0x5a, 0x79, 0x3a, 0xf7, 0x5b, 0x44, 0x1a}};
constexpr GUID TaskGroupId{0xa152e779, 0x93df, 0x5be2,
    {0xaf, 0x2c, 0xd3, 0x42, 0x45, 0x2b, 0x0c, 0xe0}};
constexpr GUID TaskItemId{0xb081d9d6, 0x9b45, 0x5363,
    {0x8a, 0x4d, 0x85, 0x4a, 0xdd, 0x6a, 0xbe, 0x7e}};
using PointerPressedFunction = HRESULT(WINAPI*)(void*, void*);
using ContainerModelFunction = void*(WINAPI*)(void**, const UIElement*);
PointerPressedFunction PointerPressedOriginal{};
ContainerModelFunction ReadGroupModel{};
ContainerModelFunction ReadWindowModel{};
decltype(&LoadLibraryExW) MouseLoadLibraryOriginal{};

/// Sends exactly one folder-open request to Explorer from the apartment worker.
void OpenExecutableFolder(const std::wstring& executable) {
    auto separator = executable.find_last_of(L"\\/");
    if (separator == std::wstring::npos) throw_hresult(E_INVALIDARG);
    std::wstring directory = executable.substr(0, separator == 2 ? 3 : separator);
    wchar_t windowsDirectory[MAX_PATH]{};
    if (!GetWindowsDirectoryW(windowsDirectory, ARRAYSIZE(windowsDirectory)))
        throw_last_error();
    std::wstring explorer = std::wstring(windowsDirectory) + L"\\explorer.exe";
    std::wstring arguments = L"\"" + directory + L"\"";
    SHELLEXECUTEINFOW execution{sizeof(execution)};
    execution.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_ASYNCOK;
    execution.lpFile = explorer.c_str();
    execution.lpParameters = arguments.c_str();
    execution.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execution)) throw_last_error();
    Wh_SetStringValue(L"LastOpenedDirectory", directory.c_str());
    Wh_SetIntValue(L"LastOpenSucceeded", 1);
    Wh_Log(L"Opened directory: %s", directory.c_str());
}

/// Reads an ABI getter after querying its exact interface.
/// Slots follow the installed WindowsUdk metadata or the checked Taskbar.View image.
template<typename T>
T ReadTaskProperty(IUnknown* object, unsigned slot) {
    T value{};
    auto table = *reinterpret_cast<void***>(object);
    using Getter = HRESULT(WINAPI*)(void*, T*);
    check_hresult(reinterpret_cast<Getter>(table[slot])(object, &value));
    return value;
}

/// Obtains the native app model for either a grouped icon or an individual window.
com_ptr<IUnknown> GetClickedTaskGroup(const UIElement& element) {
    com_ptr<IUnknown> model;
    ReadGroupModel(model.put_void(), &element);
    if (!model) ReadWindowModel(model.put_void(), &element);
    if (!model) throw_hresult(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    com_ptr<IUnknown> appModel;
    check_hresult(model->QueryInterface(AppItemViewModelId, appModel.put_void()));
    com_ptr<IUnknown> group;
    group.attach(ReadTaskProperty<IUnknown*>(appModel.get(), 31));
    if (!group) throw_hresult(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    com_ptr<IUnknown> taskGroup;
    check_hresult(group->QueryInterface(TaskGroupId, taskGroup.put_void()));
    return taskGroup;
}

/// Resolves a running app through its taskbar window, including combined windows.
std::wstring GetTaskGroupExecutable(IUnknown* group, const std::wstring& appId) {
    com_ptr<IUnknown> items;
    items.attach(ReadTaskProperty<IUnknown*>(group, 20));
    if (items) {
        auto count = ReadTaskProperty<uint32_t>(items.get(), 7);
        for (uint32_t index = 0; index < count; ++index) {
            auto table = *reinterpret_cast<void***>(items.get());
            using GetAt = HRESULT(WINAPI*)(void*, uint32_t, void**);
            com_ptr<IUnknown> rawItem;
            check_hresult(reinterpret_cast<GetAt>(table[6])(items.get(), index,
                rawItem.put_void()));
            com_ptr<IUnknown> item;
            check_hresult(rawItem->QueryInterface(TaskItemId, item.put_void()));
            auto windowId = ReadTaskProperty<uint64_t>(item.get(), 8);
            DWORD processId{};
            GetWindowThreadProcessId(reinterpret_cast<HWND>(windowId), &processId);
            if (!processId) continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
            if (!process) continue;
            std::wstring executable(32768, L'\0');
            DWORD length = static_cast<DWORD>(executable.size());
            bool resolved = QueryFullProcessImageNameW(process, 0, executable.data(), &length);
            CloseHandle(process);
            if (resolved) {
                executable.resize(length);
                return TaskbarMenu::Detail::ResolveExecutable(std::move(executable));
            }
        }
    }
    if (appId.find(L'!') != std::wstring::npos)
        return TaskbarMenu::Detail::ResolveExecutable(L"app-id:" + appId);

    // Pinned desktop apps can have no running window. Ask the shell for their target.
    com_ptr<IShellItem2> application;
    check_hresult(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, 0, appId.c_str(),
        IID_PPV_ARGS(application.put())));
    wchar_t* target{};
    check_hresult(application->GetString(PKEY_Link_TargetParsingPath, &target));
    std::wstring path(target ? target : L"");
    CoTaskMemFree(target);
    return TaskbarMenu::Detail::ResolveExecutable(std::move(path));
}

/// Handles only Mouse 5 on app icons; other input goes to Windows unchanged.
HRESULT WINAPI PointerPressedHook(void* sender, void* arguments) {
    if (!MouseFiveEnabled || TaskbarMenu::Detail::Stopping)
        return PointerPressedOriginal(sender, arguments);
    try {
        UIElement element{nullptr};
        check_hresult(static_cast<IUnknown*>(sender)->QueryInterface(
            guid_of<UIElement>(), put_abi(element)));
        Input::PointerRoutedEventArgs args{nullptr};
        check_hresult(static_cast<IUnknown*>(arguments)->QueryInterface(
            guid_of<Input::PointerRoutedEventArgs>(), put_abi(args)));
        auto point = args.GetCurrentPoint(element);
        if (point.Properties().PointerUpdateKind() !=
                Windows::UI::Input::PointerUpdateKind::XButton2Pressed)
            return PointerPressedOriginal(sender, arguments);

        args.Handled(true);
        Wh_SetStringValue(L"MouseFiveStage", L"pressed");
        auto group = GetClickedTaskGroup(element);
        hstring appId{ReadTaskProperty<HSTRING>(group.get(), 6), take_ownership_from_abi};
        hstring programName{ReadTaskProperty<HSTRING>(group.get(), 8), take_ownership_from_abi};
        Wh_SetStringValue(L"LastMouseFiveApplicationId", appId.c_str());
        auto started = GetTickCount64();
        auto executable = GetTaskGroupExecutable(group.get(), std::wstring(appId));
        Wh_SetIntValue(L"LastMouseFiveResolveMs", GetTickCount64() - started);
        if (!TaskbarMenu::Detail::QueueAction(executable, std::wstring(programName)))
            throw_hresult(HRESULT_FROM_WIN32(ERROR_NOT_READY));
        Wh_SetStringValue(L"LastMouseFiveExecutable", executable.c_str());
        Wh_SetStringValue(L"LastMouseFiveProgramName", programName.c_str());
        Wh_SetStringValue(L"MouseFiveStage", L"queued");
        Wh_SetIntValue(L"LastMouseFiveError", 0);
        Wh_Log(L"Mouse 5 queued %s executable directory: %s",
            programName.c_str(), executable.c_str());
        return S_OK;
    } catch (const hresult_error& error) {
        Wh_SetIntValue(L"LastMouseFiveError", error.code());
        Wh_SetStringValue(L"MouseFiveStage", error.message().c_str());
        Wh_Log(L"Mouse 5 folder action failed: %08X %s",
            static_cast<UINT>(error.code()), error.message().c_str());
    } catch (...) { Wh_Log(L"Mouse 5 folder action failed with an unexpected exception"); }
    return S_OK;
}

/// Checks the exact image whose pointer callback and model accessor were inspected.
bool IsSupportedTaskbarView(HMODULE module) {
    auto base = reinterpret_cast<const BYTE*>(module);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE) return false;
    auto directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    auto entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + directory.VirtualAddress);
    for (DWORD index = 0; index < directory.Size / sizeof(*entries); ++index) {
        if (entries[index].Type != IMAGE_DEBUG_TYPE_CODEVIEW || entries[index].SizeOfData < 24)
            continue;
        auto record = base + entries[index].AddressOfRawData;
        if (memcmp(record, "RSDS", 4) == 0 &&
                memcmp(record + 4, &TaskbarViewImage, sizeof(GUID)) == 0 &&
                *reinterpret_cast<const DWORD*>(record + 20) == 1)
            return true;
    }
    return false;
}

/// Hooks the app button callback without downloading symbols at runtime.
bool HookMouseFive(HMODULE module) {
    if (MouseHooksInstalled || !module) return true;
    if (!IsSupportedTaskbarView(module)) {
        Wh_SetStringValue(L"MouseFiveHookStatus", L"unsupported-taskbar-image");
        Wh_Log(L"Mouse 5 Taskbar.View symbol identity has not been verified");
        return false;
    }
    auto base = reinterpret_cast<BYTE*>(module);
    ReadGroupModel = reinterpret_cast<ContainerModelFunction>(base + 0x26230);
    ReadWindowModel = reinterpret_cast<ContainerModelFunction>(base + 0x83bc0);
    if (!Wh_SetFunctionHook(base + 0x46eb0, reinterpret_cast<void*>(PointerPressedHook),
            reinterpret_cast<void**>(&PointerPressedOriginal)))
        return false;
    MouseHooksInstalled = true;
    Wh_SetStringValue(L"MouseFiveHookStatus", L"installed");
    return true;
}

/// Attaches after the taskbar loads during Explorer startup.
HMODULE WINAPI MouseLoadLibraryHook(LPCWSTR name, HANDLE file, DWORD flags) {
    auto module = MouseLoadLibraryOriginal(name, file, flags);
    if (!TaskbarMenu::Detail::Stopping && !MouseHooksInstalled && module &&
            module == GetModuleHandleW(L"Taskbar.View.dll"))
        if (HookMouseFive(module)) Wh_ApplyHookOperations();
    return module;
}

/// Registers Mouse 5 in Explorer; the menu host keeps its existing registration.
bool InitializeMouseFive() {
    if (!TaskbarMenu::Detail::IsExplorer) return true;
    if (auto module = GetModuleHandleW(L"Taskbar.View.dll")) return HookMouseFive(module);
    auto loader = GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "LoadLibraryExW");
    return loader && Wh_SetFunctionHook(reinterpret_cast<void*>(loader),
        reinterpret_cast<void*>(MouseLoadLibraryHook),
        reinterpret_cast<void**>(&MouseLoadLibraryOriginal));
}

/// Supplies localized text and the app-specific action in one registration call.
bool RegisterFolderButton() {
    MouseFiveEnabled = Wh_GetIntSetting(L"mouse5OpensFolder") != 0;
    Wh_SetIntValue(L"MouseFiveEnabled", MouseFiveEnabled ? 1 : 0);
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
BOOL Wh_ModInit() { return RegisterFolderButton() && InitializeMouseFive(); }
void Wh_ModSettingsChanged() { RegisterFolderButton(); }
