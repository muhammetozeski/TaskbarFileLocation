# TaskbarFileLocation

![Windows](https://img.shields.io/badge/Windows-11_x64-0078D4)
![C++](https://img.shields.io/badge/C%2B%2B-23-00599C)
![License](https://img.shields.io/badge/license-MIT-green)

Open an application's executable folder directly from its taskbar menu.

Right-click an application on the taskbar and select **Dosya Konumunu Aç**. The English label is **Open File Location**. Existing application tasks, recent items and window commands are preserved.

Click an application's taskbar icon with **Mouse 4** (the first side button) to open its executable folder directly. **Open file location with Mouse 4** is enabled by default in Windhawk settings. Turning it off takes effect when the settings are saved.

The extension reads the application path from the Windows jump-list session. File Explorer opens the executable's containing directory; shortcuts are resolved to their targets. Packaged applications are matched by application identity to a running process, and Windows supplies the executable path.

## Requirements

- Windows 11, 64-bit.
- Portable [Windhawk](https://windhawk.net/), already running.
- This first version targets **JumpViewUI.dll 10.0.26100.9549**, symbol identifier **ADF87E5879A2571AC6566850798F0A6B1**.
- The Mouse 4 callback targets **Taskbar.View.dll 2608.26001.200.0**, symbol identifier **24534974E00E46A58F69F60D0CEB9BD91**.

The extension checks that identity before attaching its native hooks. It does not download symbols or packages.

## Installation

Extract the release package and run the following command from its directory:

```powershell
pwsh -File .\Install.ps1
```

For a Windhawk directory that is not available through PATH:

```powershell
pwsh -File .\Install.ps1 -WindhawkDirectory "C:\Path\To\Windhawk"
```

The installer registers **Dosya Konumunu Aç** in Windhawk, keeps the existing language and Mouse 4 preferences and grants the shell host read access to the required Windhawk files. Settings and logs use Windhawk's existing storage.

Select `tr` or `en` in the extension's Windhawk settings to change the label. Disable the extension in Windhawk to remove the added command.

## Source build

The reusable menu implementation is contained in one file, `include\TaskbarMenuButton.hpp`, relative to this repository. Register a button from `Wh_ModInit` with `AddTaskbarButton(buttonName, callback)`. The callback receives only the resolved executable path and the application's display name from the existing menu. The header supplies Windhawk's cleanup and late-initialization exports.

The file-location mod uses this API for its menu command. Its Mouse 4 callback reads the clicked taskbar model and shares the same executable resolver and folder-opening action. Installation embeds the header into Windhawk's source copy so that copy remains standalone.

Windhawk's bundled C++ compiler builds the module. From the repository directory:

```powershell
pwsh -File .\Build.ps1
pwsh -File .\Install.ps1
```

The compiled module is placed in `publish\AppData\TaskbarFileLocation.dll`, relative to the repository directory.

## Verified behavior

The taskbar command opened the executable directories for Windhawk, WinRAR and ChatGPT (the Codex desktop package). The command appeared after enabling the module without restarting Explorer. Directory access used normal user privileges.
