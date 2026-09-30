#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <UIAutomation.h>
#include <oleauto.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace {

constexpr wchar_t kWindowClass[] = L"LiveTranslateLiteOverlay";
constexpr wchar_t kAppTitle[] = L"LiveTranslate Lite";
constexpr UINT WM_APP_TRANSLATION = WM_APP + 1;
constexpr UINT WM_APP_STATUS = WM_APP + 2;
constexpr int HOTKEY_PAUSE = 1;
constexpr int HOTKEY_VISIBILITY = 2;
constexpr int HOTKEY_DIRECTION = 3;

struct Config {
    std::wstring mode = L"auto";
    int debounceMs = 350;
    int width = 1100;
    int height = 150;
    int bottomMargin = 90;
    int fontSize = 34;
    int opacity = 225;
    bool hideLiveCaptions = true;
};

Config g_config;
HWND g_hwnd = nullptr;
std::atomic_bool g_running{true};
std::atomic_bool g_paused{false};
std::atomic_bool g_overlayVisible{true};
std::wstring g_displayText = L"Starting Windows Live Captions…";
std::wstring g_currentMode = L"auto";
std::mutex g_captionMutex;
std::condition_variable g_captionCv;
std::wstring g_latestCaption;
std::chrono::steady_clock::time_point g_lastCaptionChange = std::chrono::steady_clock::now();
std::atomic_uint64_t g_captionVersion{0};

std::wstring GetExeDirectory() {
    std::vector<wchar_t> buffer(32768);
    DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0 || len >= buffer.size()) return L".";
    std::filesystem::path p(std::wstring(buffer.data(), len));
    return p.parent_path().wstring();
}

void LoadConfig() {
    const std::wstring path = GetExeDirectory() + L"\\config.ini";
    wchar_t mode[32]{};
    GetPrivateProfileStringW(L"translation", L"mode", L"auto", mode, 32, path.c_str());
    g_config.mode = mode;
    g_currentMode = g_config.mode;
    g_config.debounceMs = GetPrivateProfileIntW(L"translation", L"debounce_ms", 350, path.c_str());
    g_config.width = GetPrivateProfileIntW(L"overlay", L"width", 1100, path.c_str());
    g_config.height = GetPrivateProfileIntW(L"overlay", L"height", 150, path.c_str());
    g_config.bottomMargin = GetPrivateProfileIntW(L"overlay", L"bottom_margin", 90, path.c_str());
    g_config.fontSize = GetPrivateProfileIntW(L"overlay", L"font_size", 34, path.c_str());
    g_config.opacity = GetPrivateProfileIntW(L"overlay", L"opacity", 225, path.c_str());
    g_config.hideLiveCaptions = GetPrivateProfileIntW(L"overlay", L"hide_windows_live_captions", 1, path.c_str()) != 0;

    if (g_config.debounceMs < 100) g_config.debounceMs = 100;
    if (g_config.debounceMs > 3000) g_config.debounceMs = 3000;
    if (g_config.opacity < 40) g_config.opacity = 40;
    if (g_config.opacity > 255) g_config.opacity = 255;
}

std::string WideToUtf8(const std::wstring& input) {
    if (input.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& input) {
    if (input.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0) {
        size = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    }
    if (size <= 0) return {};
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), out.data(), size);
    return out;
}

bool IsUnreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

std::wstring UrlEncodeUtf8(const std::wstring& input) {
    const std::string utf8 = WideToUtf8(input);
    static const char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(utf8.size() * 3);
    for (unsigned char c : utf8) {
        if (IsUnreserved(c)) {
            encoded.push_back(static_cast<char>(c));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[(c >> 4) & 0x0F]);
            encoded.push_back(hex[c & 0x0F]);
        }
    }
    return Utf8ToWide(encoded);
}

std::wstring NormalizeWhitespace(std::wstring text) {
    std::wstring out;
    out.reserve(text.size());
    bool prevSpace = false;
    for (wchar_t ch : text) {
        bool space = iswspace(ch) != 0;
        if (space) {
            if (!prevSpace && !out.empty()) out.push_back(L' ');
        } else {
            out.push_back(ch);
        }
        prevSpace = space;
    }
    while (!out.empty() && iswspace(out.back())) out.pop_back();
    return out;
}

bool IsArabicLetter(wchar_t c) {
    return (c >= 0x0600 && c <= 0x06FF) ||
           (c >= 0x0750 && c <= 0x077F) ||
           (c >= 0x08A0 && c <= 0x08FF) ||
           (c >= 0xFB50 && c <= 0xFDFF) ||
           (c >= 0xFE70 && c <= 0xFEFF);
}

std::wstring ChooseTargetLanguage(const std::wstring& text) {
    if (_wcsicmp(g_currentMode.c_str(), L"ar") == 0) return L"ar";
    if (_wcsicmp(g_currentMode.c_str(), L"en") == 0) return L"en";

    int arabic = 0;
    int latin = 0;
    for (wchar_t c : text) {
        if (IsArabicLetter(c)) ++arabic;
        else if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')) ++latin;
    }
    // Auto pair: Arabic-dominant -> English, otherwise -> Arabic.
    return arabic > latin ? L"en" : L"ar";
}

int HexValue(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

std::wstring JsonUnescape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'\\' || i + 1 >= s.size()) {
            out.push_back(s[i]);
            continue;
        }
        wchar_t n = s[++i];
        switch (n) {
            case L'"': out.push_back(L'"'); break;
            case L'\\': out.push_back(L'\\'); break;
            case L'/': out.push_back(L'/'); break;
            case L'b': out.push_back(L'\b'); break;
            case L'f': out.push_back(L'\f'); break;
            case L'n': out.push_back(L'\n'); break;
            case L'r': out.push_back(L'\r'); break;
            case L't': out.push_back(L'\t'); break;
            case L'u': {
                if (i + 4 < s.size()) {
                    int value = 0;
                    bool ok = true;
                    for (int k = 0; k < 4; ++k) {
                        int h = HexValue(s[i + 1 + k]);
                        if (h < 0) { ok = false; break; }
                        value = (value << 4) | h;
                    }
                    if (ok) {
                        out.push_back(static_cast<wchar_t>(value));
                        i += 4;
                    } else {
                        out += L"\\u";
                    }
                } else {
                    out += L"\\u";
                }
                break;
            }
            default: out.push_back(n); break;
        }
    }
    return out;
}

std::wstring ExtractGoogleTranslation(const std::wstring& json) {
    const std::wstring key = L"\"trans\":";
    size_t pos = 0;
    std::wstring combined;

    while ((pos = json.find(key, pos)) != std::wstring::npos) {
        pos += key.size();
        while (pos < json.size() && iswspace(json[pos])) ++pos;
        if (pos >= json.size() || json[pos] != L'"') continue;
        ++pos;
        std::wstring raw;
        bool escaped = false;
        while (pos < json.size()) {
            wchar_t c = json[pos++];
            if (escaped) {
                raw.push_back(L'\\');
                raw.push_back(c);
                escaped = false;
                continue;
            }
            if (c == L'\\') {
                escaped = true;
                continue;
            }
            if (c == L'"') break;
            raw.push_back(c);
        }
        std::wstring part = JsonUnescape(raw);
        if (!part.empty()) {
            if (!combined.empty() && combined.back() != L' ') combined.push_back(L' ');
            combined += part;
        }
    }
    return NormalizeWhitespace(combined);
}

std::wstring TranslateWithGoogle(const std::wstring& text, const std::wstring& targetLang) {
    HINTERNET session = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/154.0 Safari/537.36",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) return L"";

    WinHttpSetTimeouts(session, 2500, 2500, 4000, 5000);

    HINTERNET connect = WinHttpConnect(session, L"clients5.google.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return L"";
    }

    std::wstring path = L"/translate_a/t?client=dict-chrome-ex&sl=auto&tl=" +
                        targetLang + L"&q=" + UrlEncodeUtf8(text);

    HINTERNET request = WinHttpOpenRequest(
        connect,
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return L"";
    }

    const wchar_t* headers = L"Accept: application/json\r\nAccept-Charset: utf-8\r\n";
    BOOL ok = WinHttpSendRequest(
        request,
        headers,
        static_cast<DWORD>(-1L),
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0);
    if (ok) ok = WinHttpReceiveResponse(request, nullptr);

    std::string bytes;
    if (ok) {
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX);
        if (status >= 200 && status < 300) {
            for (;;) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
                size_t oldSize = bytes.size();
                bytes.resize(oldSize + available);
                DWORD read = 0;
                if (!WinHttpReadData(request, bytes.data() + oldSize, available, &read)) {
                    bytes.resize(oldSize);
                    break;
                }
                bytes.resize(oldSize + read);
            }
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (bytes.empty()) return L"";
    return ExtractGoogleTranslation(Utf8ToWide(bytes));
}

HWND FindLiveCaptionsWindow() {
    struct State { HWND hwnd = nullptr; } state;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* state = reinterpret_cast<State*>(lp);
        wchar_t className[256]{};
        if (GetClassNameW(hwnd, className, 256) > 0 &&
            wcscmp(className, L"LiveCaptionsDesktopWindow") == 0) {
            state->hwnd = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&state));
    return state.hwnd;
}

void SendLiveCaptionsHotkey() {
    INPUT inputs[6]{};
    inputs[0].type = INPUT_KEYBOARD; inputs[0].ki.wVk = VK_LWIN;
    inputs[1].type = INPUT_KEYBOARD; inputs[1].ki.wVk = VK_CONTROL;
    inputs[2].type = INPUT_KEYBOARD; inputs[2].ki.wVk = 'L';
    inputs[3].type = INPUT_KEYBOARD; inputs[3].ki.wVk = 'L'; inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[4].type = INPUT_KEYBOARD; inputs[4].ki.wVk = VK_CONTROL; inputs[4].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[5].type = INPUT_KEYBOARD; inputs[5].ki.wVk = VK_LWIN; inputs[5].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(6, inputs, sizeof(INPUT));
}

HWND EnsureLiveCaptionsWindow() {
    HWND hwnd = FindLiveCaptionsWindow();
    if (hwnd) return hwnd;

    SendLiveCaptionsHotkey();
    for (int i = 0; i < 80 && g_running; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        hwnd = FindLiveCaptionsWindow();
        if (hwnd) return hwnd;
    }
    return nullptr;
}

void HideLiveCaptionsWindow(HWND hwnd) {
    if (!hwnd) return;
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle | WS_EX_TOOLWINDOW);
    ShowWindow(hwnd, SW_MINIMIZE);
}

void PostStatus(const std::wstring& text) {
    if (!g_hwnd) return;
    auto* copy = new std::wstring(text);
    if (!PostMessageW(g_hwnd, WM_APP_STATUS, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
}

void PostTranslation(const std::wstring& text) {
    if (!g_hwnd) return;
    auto* copy = new std::wstring(text);
    if (!PostMessageW(g_hwnd, WM_APP_TRANSLATION, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
}

void CaptionPollingThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IUIAutomation* automation = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_CUIAutomation,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&automation));
    if (FAILED(hr) || !automation) {
        PostStatus(L"Could not initialize Windows UI Automation.");
        CoUninitialize();
        return;
    }

    HWND liveHwnd = EnsureLiveCaptionsWindow();
    if (!liveHwnd) {
        PostStatus(L"Windows Live Captions was not found. Press Win + Ctrl + L, then restart the app.");
        automation->Release();
        CoUninitialize();
        return;
    }

    if (g_config.hideLiveCaptions) HideLiveCaptionsWindow(liveHwnd);

    IUIAutomationElement* root = nullptr;
    IUIAutomationElement* captionElement = nullptr;

    auto connectCaptionElement = [&]() -> bool {
        if (captionElement) { captionElement->Release(); captionElement = nullptr; }
        if (root) { root->Release(); root = nullptr; }

        liveHwnd = FindLiveCaptionsWindow();
        if (!liveHwnd) return false;
        if (FAILED(automation->ElementFromHandle(liveHwnd, &root)) || !root) return false;

        VARIANT value;
        VariantInit(&value);
        value.vt = VT_BSTR;
        value.bstrVal = SysAllocString(L"CaptionsTextBlock");
        IUIAutomationCondition* condition = nullptr;
        HRESULT chr = automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, value, &condition);
        VariantClear(&value);
        if (FAILED(chr) || !condition) return false;

        HRESULT fhr = root->FindFirst(TreeScope_Descendants, condition, &captionElement);
        condition->Release();
        return SUCCEEDED(fhr) && captionElement;
    };

    PostStatus(L"Listening…");

    std::wstring lastSeen;
    while (g_running) {
        if (g_paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            continue;
        }

        if (!captionElement && !connectCaptionElement()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        BSTR name = nullptr;
        HRESULT nhr = captionElement->get_CurrentName(&name);
        if (FAILED(nhr)) {
            captionElement->Release();
            captionElement = nullptr;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        std::wstring current = name ? std::wstring(name, SysStringLen(name)) : L"";
        if (name) SysFreeString(name);
        current = NormalizeWhitespace(current);

        if (!current.empty() && current != lastSeen) {
            lastSeen = current;
            {
                std::lock_guard<std::mutex> lock(g_captionMutex);
                g_latestCaption = current;
                g_lastCaptionChange = std::chrono::steady_clock::now();
                ++g_captionVersion;
            }
            g_captionCv.notify_one();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (captionElement) captionElement->Release();
    if (root) root->Release();
    automation->Release();
    CoUninitialize();
}

void TranslationThread() {
    std::wstring lastTranslated;

    while (g_running) {
        std::unique_lock<std::mutex> lock(g_captionMutex);
        g_captionCv.wait_for(lock, std::chrono::milliseconds(250), [] {
            return !g_running || (!g_latestCaption.empty() && g_latestCaption != L"");
        });
        if (!g_running) break;
        if (g_paused || g_latestCaption.empty()) {
            lock.unlock();
            continue;
        }

        auto sinceChange = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_lastCaptionChange).count();
        if (sinceChange < g_config.debounceMs) {
            int waitMs = g_config.debounceMs - static_cast<int>(sinceChange);
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
            continue;
        }

        std::wstring text = g_latestCaption;
        uint64_t version = g_captionVersion.load();
        lock.unlock();

        if (text == lastTranslated) {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            continue;
        }
        lastTranslated = text;

        std::wstring target = ChooseTargetLanguage(text);
        std::wstring translated = TranslateWithGoogle(text, target);
        if (translated.empty()) {
            PostStatus(L"Google translation failed. Retrying on the next caption…");
            continue;
        }

        // Discard stale output when a newer caption arrived while the HTTP request was in flight.
        if (version != g_captionVersion.load()) continue;
        PostTranslation(translated);
    }
}

void PositionOverlay(HWND hwnd) {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int screenWidth = work.right - work.left;
    int x = work.left + (screenWidth - g_config.width) / 2;
    int y = work.bottom - g_config.bottomMargin - g_config.height;
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, g_config.width, g_config.height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void CycleDirection() {
    if (_wcsicmp(g_currentMode.c_str(), L"auto") == 0) g_currentMode = L"ar";
    else if (_wcsicmp(g_currentMode.c_str(), L"ar") == 0) g_currentMode = L"en";
    else g_currentMode = L"auto";

    std::wstring label = L"Mode: ";
    if (g_currentMode == L"auto") label += L"Auto Arabic ↔ English";
    else if (g_currentMode == L"ar") label += L"Always Arabic";
    else label += L"Always English";
    PostStatus(label);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(g_config.opacity), LWA_ALPHA);
            RegisterHotKey(hwnd, HOTKEY_PAUSE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'T');
            RegisterHotKey(hwnd, HOTKEY_VISIBILITY, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'O');
            RegisterHotKey(hwnd, HOTKEY_DIRECTION, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'D');
            return 0;

        case WM_HOTKEY:
            if (wParam == HOTKEY_PAUSE) {
                bool paused = !g_paused.load();
                g_paused = paused;
                g_displayText = paused ? L"Translation paused — Ctrl+Alt+T to resume" : L"Listening…";
                InvalidateRect(hwnd, nullptr, TRUE);
            } else if (wParam == HOTKEY_VISIBILITY) {
                bool visible = !g_overlayVisible.load();
                g_overlayVisible = visible;
                ShowWindow(hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
            } else if (wParam == HOTKEY_DIRECTION) {
                CycleDirection();
            }
            return 0;

        case WM_APP_TRANSLATION:
        case WM_APP_STATUS: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
            if (text) {
                g_displayText = *text;
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);

            HBRUSH bg = CreateSolidBrush(RGB(12, 12, 12));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));

            LOGFONTW lf{};
            lf.lfHeight = -MulDiv(g_config.fontSize, GetDeviceCaps(hdc, LOGPIXELSY), 72);
            lf.lfWeight = FW_SEMIBOLD;
            lf.lfQuality = CLEARTYPE_QUALITY;
            wcscpy_s(lf.lfFaceName, L"Segoe UI");
            HFONT font = CreateFontIndirectW(&lf);
            HFONT old = static_cast<HFONT>(SelectObject(hdc, font));

            RECT textRc = rc;
            InflateRect(&textRc, -28, -16);
            DrawTextW(hdc, g_displayText.c_str(), -1, &textRc,
                      DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);

            SelectObject(hdc, old);
            DeleteObject(font);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            UnregisterHotKey(hwnd, HOTKEY_PAUSE);
            UnregisterHotKey(hwnd, HOTKEY_VISIBILITY);
            UnregisterHotKey(hwnd, HOTKEY_DIRECTION);
            g_running = false;
            g_captionCv.notify_all();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    LoadConfig();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = hInstance;
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc)) return 1;

    g_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        kWindowClass,
        kAppTitle,
        WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, g_config.width, g_config.height,
        nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd) return 2;

    PositionOverlay(g_hwnd);
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(g_hwnd);

    std::thread captionThread(CaptionPollingThread);
    std::thread translationThread(TranslationThread);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_running = false;
    g_captionCv.notify_all();
    if (captionThread.joinable()) captionThread.join();
    if (translationThread.joinable()) translationThread.join();

    return 0;
}
