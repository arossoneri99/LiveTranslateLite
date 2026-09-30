# LiveTranslate Lite

A minimal Windows-native live subtitle translator designed to avoid .NET, Electron, Python, WebView, databases, and third-party runtime dependencies.

## Architecture

Windows Live Captions -> Windows UI Automation -> Google Translate HTTP endpoint -> native Win32 overlay

## What it uses

- C++20 / Win32
- Windows UI Automation (`UIAutomationCore`)
- WinHTTP
- GDI overlay
- Windows Live Captions for on-device speech recognition

No .NET runtime is required by this app.

## Translation modes

Edit `config.ini`:

- `mode=auto`: Arabic-dominant text translates to English; otherwise translates to Arabic.
- `mode=ar`: always translate to Arabic.
- `mode=en`: always translate to English.

Hotkey: `Ctrl + Alt + D` cycles between the three modes.

## Hotkeys

- `Ctrl + Alt + T`: pause/resume translation
- `Ctrl + Alt + O`: hide/show the overlay
- `Ctrl + Alt + D`: cycle translation direction

## Build on Windows

Requirements for building only:

- Visual Studio 2022/2026 Build Tools with the Desktop development with C++ workload
- CMake
- Windows SDK

Commands from a Developer PowerShell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The resulting executable will be under:

`build/Release/LiveTranslateLite.exe`

Copy `config.ini` next to the EXE.

The CMake project uses the static MSVC runtime (`/MT`) in Release builds, so the target PC should not need the VC++ redistributable just to run this executable.

## First run

1. Windows 11 Live Captions must already work on the PC.
2. Configure Live Captions once and download the desired speech-recognition language pack.
3. The app tries to launch Live Captions with `Win + Ctrl + L` if the Live Captions window is not already present.
4. Keep `Position > Overlaid on screen` configured in Windows Live Captions, matching the upstream project's recommendation.
5. The app minimizes/hides the native Live Captions window by default and shows only its own translation overlay.

## Privacy / reliability note

This prototype uses the same style of unauthenticated Google Translate web endpoint used by some open-source translators. That endpoint is not the official Google Cloud Translation API and can be rate-limited or changed by Google. Text sent for translation leaves the PC and is sent to Google.

For a production/business build, replace the HTTP function with the official Google Cloud Translation API and use a protected API key / service account.

## Resource goal

The translator process itself should be small because it performs no local AI inference. The largest local workload remains Windows Live Captions, which performs speech recognition on-device. Exact RAM/CPU usage must be measured on the target Windows machine after compiling.
