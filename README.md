# Luminex Shaders

Luminex Shaders brings ReShade to Roblox. It includes a simple manager to, well, manage things and inject ReShade into a Roblox window.

**Luminex does not work with the normal Roblox client.** You need a Hyperion-emulating exploit such as Volt because Hyperion blocks the loading method Luminex Shaders uses.

We are not responsible for any bans, since we cannot control whether your exploit gets detected.

Luminex is NOT affiliated with Roblox or ReShade.

## Getting started

You need Windows x64, a Direct3D 11 capable GPU, and a compatible client environment. An internet connection is needed to download shader packs.

1. Download and open **LuminexSetup.exe**.
2. Choose your keyboard shortcuts and shader packs, then click **Install**. Uncheck **Create Start menu shortcut** if you do not want one. Check **Create desktop shortcut** if you also want a desktop icon.
3. Open **Luminex Shaders** from the Start menu, or click **Open manager** when setup finishes.
4. Start Roblox yourself. In the manager, click **Select Roblox window**, then click the Roblox window you want to use. You can also select its PID from the list.
5. Click **Inject Into Roblox**. Accept the Windows administrator prompt if one appears.
6. In Roblox, press **Home**, or the menu shortcut you chose, to open ReShade and enable effects.

The manager loads ReShade into the client you select. It does not start with Roblox or identify which account is signed in.

## Keyboard shortcuts

Setup lets you choose shortcuts for opening the ReShade menu and turning all effects on or off. Click a shortcut button and press the key you want. You can combine it with Ctrl, Shift, or Alt. Press Escape to cancel.

The menu shortcut defaults to **Home**. The effects toggle has no shortcut until you choose one.

To change them later, open **Keyboard shortcuts** in the manager. Close the client using Luminex before saving changes, since a running client can save its old settings over yours.

## Shader packs and presets

**Only Standard effects are selected by default.** Every other pack is optional.

You can add packs later through **Shader packs** in the manager.

The default preset has no effects enabled. Open the ReShade menu and choose the effects you want.

Shader packs download directly from their authors. Their licenses still apply, and the selector's **Author page** button opens the upstream repository. Third-party shaders and presets are not bundled in the installer.

## Updates and your files

Close any client using Luminex and run the new **LuminexSetup.exe**. Updates keep your settings, presets, shortcuts, and downloaded shader packs.

Your installed files are under `%LOCALAPPDATA%\LuminexShaders`. Settings, presets, shaders, and logs are in its `Runtime` folder. Click **Open configs folder** in the manager to get there.

The manager is installed under `%LOCALAPPDATA%\Programs\LuminexShaders`. You can delete the downloaded installer after setup; the manager includes its own maintenance files.

## Logs

You can turn **Save diagnostic logs on this PC** on or off in Setup, or through **Logs** in the manager. Use **About saved logs** in Setup to read what they contain. The setting takes effect after closing the client and loading Luminex into a new session.

Logs can include loading and shader errors, timestamps, process/thread IDs, graphics device details, shader names, and file paths that may include your Windows username. Luminex keeps them on your PC and does not upload them. You choose whether to share them for troubleshooting.

The files are `Runtime/Logs/Luminex.log` and `Runtime/ReShade.log`. Each is replaced when its component next starts with logging enabled. Turning logging off keeps existing logs; you can remove them through the uninstall window's log option. This setting does not disable saving your settings, presets, shader cache, or screenshots.

## Uninstalling

Close the client using Luminex, then choose **Uninstall** in the manager or uninstall **Luminex Shaders** through Windows Installed Apps.

Program files are removed. Your settings, presets, shader packs, and logs are kept by default. The uninstall window lets you choose which of those to delete, or remove the entire installation folder.

Logs are diagnostic records used for troubleshooting. Deleting them does not delete your presets or shaders. Deleted files do not go to the Recycle Bin, so back up anything you want to keep.

Close the uninstall window to finish removing the manager and its Start menu and desktop shortcuts.

## FAQ

**Is this vibe coded?**

yes

**Will this ever work without a Hyperion-emulating exploit?**

Most likely no, but you never know.

**Will this ever come to Linux?**

I don't have Linux, so not right now. You can always fork the project and add Linux support.

**Is this malware or a virus?**

No. You can look at the source code it's right there. Antivirus software may flag it as a virus, but it's a false positive.

**ReShade says some effects failed to load. How do I fix it?**

That may be a problem with the effects themselves. We don't maintain those effects, so we can't promise fixes for them.

**Does this work with DLSS 5?**

I haven't tested it, so I can't confirm that it works.

## Building from source

To build Luminex yourself, you need:

- Windows x64.
- Git for Windows, available in your terminal.
- The .NET 10 SDK.
- Python 3.10 or newer with pip, available as `python` in your terminal. Build dependencies are installed in an isolated Python environment.
- Visual Studio 2022 or 2026, or its Build Tools, with **Desktop development with C++**, an x64 MSVC toolset, a Windows SDK, and **C++ CMake tools for Windows** installed.
- An internet connection to download dependencies on the first build.

Download and extract the repository source, or clone it with Git. Open PowerShell in the folder containing `Build.ps1`, then run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Build.ps1
```

The script downloads the pinned ReShade source and its dependencies, applies the included patches, builds the native components and manager, and embeds the installer files and required licenses into **LuminexSetup.exe**. You do not need files from a previous release or any private build scripts.

The finished installer's full path and SHA-256 hash are printed when the build succeeds. By default, dependencies and build outputs are kept under `%LOCALAPPDATA%\LuminexBuild`, outside the source folder. You can choose a different output folder:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Build.ps1 -OutputDirectory "$env:USERPROFILE\Desktop\Luminex-built"
```

Run the same command again to rebuild after changing the source. `-WorkDirectory` selects a different dependency and build cache outside the source folder, and `-Jobs 2` reduces parallel compilation if memory is limited. Share only the finished installer with users, not the build cache.

## Source and release status

Luminex is an early alpha. Compatibility and in-game results can vary between machines and client environments.

Dependency licenses are included inside the program. Open **About > Third-party licenses** in the manager to read them.
