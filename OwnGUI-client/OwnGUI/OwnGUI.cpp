#include <windows.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <fstream>
#include <random>
#include <algorithm>
#include "resource.h"

#pragma comment(lib, "dwmapi.lib")

// ------------------------------------------------------------
// Konfiguration
// ------------------------------------------------------------

constexpr wchar_t PROGRAM_PATH[] =
LR"(C:\Games\Old Of Sea\default\game\OldofSea.exe)";

constexpr wchar_t PROGRAM_DIRECTORY[] =
LR"(C:\Games\Old Of Sea\default\game)";

constexpr wchar_t WINDOW_TITLE[] =
L"Every Day iam NEYDISCH";

constexpr wchar_t EVERYDAY_DLL_NAME[] =
L"EveryDayiamNEYDISCH.dll";

constexpr UINT_PTR WINDOW_SEARCH_TIMER_ID = 1;
constexpr UINT_PTR STABILITY_DELAY_TIMER_ID = 2;
constexpr UINT_PTR GAME_SIZE_GUARD_TIMER_ID = 3;
constexpr UINT_PTR PROXY_ROTATION_TIMER_ID = 4;
constexpr UINT_PTR RESTART_WAIT_TIMER_ID = 5;

constexpr UINT WINDOW_SEARCH_INTERVAL = 50;
constexpr UINT STABILITY_DELAY_INTERVAL = 1000; // 1 Sekunde Warten nach Vollbild-Einnahme
constexpr UINT GAME_SIZE_GUARD_INTERVAL = 250;

// 400 × 50 ms = 20 Sekunden max. Suchen
constexpr int MAX_WINDOW_SEARCH_ATTEMPTS = 400;

// ------------------------------------------------------------
// Deklarationen
// ------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
BOOL CALLBACK FindWindowByProcessId(HWND hwnd, LPARAM lParam);

bool StartExternalProgram(HWND parentWindow);
bool PrepareExternalWindow(HWND parentWindow);
bool IsWindowInFullScreen(HWND hwndContainer, HWND hwndTarget);

bool InjectDLL(DWORD dwProcessId, const std::wstring& dllPath);
bool InjectAllDllsToRemoteProcess(HWND parentWindow);
void BeginGameRestart(HWND parentWindow);

void PositionExternalWindow(HWND parentWindow);
bool ExternalWindowNeedsPosition(HWND parentWindow);
void ResetExternalProcess();
void EnableDarkTitleBar(HWND hwnd);

void ShowWindowsError(HWND parentWindow, const wchar_t* title, DWORD errorCode);

// ------------------------------------------------------------
// Globale Variablen
// ------------------------------------------------------------

HWND hExternalWindow = nullptr;

PROCESS_INFORMATION externalProcess = {};
DWORD externalProcessId = 0;

HBRUSH hDarkBackground = nullptr;

int windowSearchAttempts = 0;
bool dllsInjected = false;
HANDLE singleInstanceMutex = nullptr;
HANDLE processJob = nullptr;
bool forceDirectX11 = false;
bool proxyEnabled = false;
bool restartPending = false;
ULONGLONG restartDeadline = 0;
std::wstring applicationDirectory;
std::wstring iniPath;
std::wstring proxyListPath;

// ------------------------------------------------------------
// Dark Mode für das Wrapper-Fenster
// ------------------------------------------------------------

void EnableDarkTitleBar(HWND hwnd)
{
    BOOL enabled = TRUE;

    HRESULT result = DwmSetWindowAttribute(
        hwnd,
        20,
        &enabled,
        sizeof(enabled)
    );

    if (FAILED(result)) {
        DwmSetWindowAttribute(
            hwnd,
            19,
            &enabled,
            sizeof(enabled)
        );
    }
}

// ------------------------------------------------------------
// Windows-Fehler anzeigen
// ------------------------------------------------------------

void ShowWindowsError(HWND parentWindow, const wchar_t* title, DWORD errorCode)
{
    wchar_t* systemMessage = nullptr;

    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&systemMessage),
        0,
        nullptr
    );

    wchar_t message[1024] = {};

    wsprintf(
        message,
        L"Die Aktion ist fehlgeschlagen.\n\n"
        L"Windows-Fehlercode: %lu\n\n"
        L"%s",
        errorCode,
        systemMessage ? systemMessage : L"Unbekannter Fehler"
    );

    MessageBox(
        parentWindow,
        message,
        title,
        MB_OK | MB_ICONERROR
    );

    if (systemMessage) {
        LocalFree(systemMessage);
    }
}

void LoadClientSettings()
{
    wchar_t api[32] = {};
    GetPrivateProfileStringW(L"Graphics", L"Api", L"auto", api, 32, iniPath.c_str());
    forceDirectX11 = _wcsicmp(api, L"d3d11") == 0;
    proxyEnabled = GetPrivateProfileIntW(L"Proxy", L"Enabled", 0, iniPath.c_str()) != 0;
}

void SaveClientSettings()
{
    WritePrivateProfileStringW(L"Graphics", L"Api", forceDirectX11 ? L"d3d11" : L"auto", iniPath.c_str());
    WritePrivateProfileStringW(L"Proxy", L"Enabled", proxyEnabled ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"Proxy", L"MinHours", L"6", iniPath.c_str());
    WritePrivateProfileStringW(L"Proxy", L"MaxHours", L"8", iniPath.c_str());
}

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), count);
    return result;
}

bool SelectProxyForCurrentCycle(std::wstring& proxyUrl, unsigned& nextCycleIndex)
{
    std::ifstream input(proxyListPath);
    std::vector<std::string> entries;
    std::string line;
    while (std::getline(input, line)) {
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        if (!line.empty() && line[0] != '#' && std::count(line.begin(), line.end(), ':') >= 3)
            entries.push_back(line);
    }
    if (entries.empty()) return false;

    // Fester Zyklus: Proxy 1 -> Proxy 2 -> ... -> ohne Proxy -> Proxy 1.
    unsigned cycleIndex = GetPrivateProfileIntW(L"Proxy", L"CycleIndex", 0, iniPath.c_str());
    cycleIndex %= static_cast<unsigned>(entries.size() + 1);
    nextCycleIndex = (cycleIndex + 1) % static_cast<unsigned>(entries.size() + 1);
    if (cycleIndex == entries.size()) {
        proxyUrl.clear();
        return true;
    }

    const std::string& value = entries[cycleIndex];
    size_t c3 = value.rfind(':');
    size_t c2 = value.rfind(':', c3 - 1);
    size_t c1 = value.rfind(':', c2 - 1);
    proxyUrl = Utf8ToWide("http://" + value.substr(c2 + 1, c3 - c2 - 1) + ":" +
        value.substr(c3 + 1) + "@" + value.substr(0, c1) + ":" +
        value.substr(c1 + 1, c2 - c1 - 1));
    return true;
}

void ScheduleProxyRotation(HWND hwnd)
{
    KillTimer(hwnd, PROXY_ROTATION_TIMER_ID);
    if (!proxyEnabled) return;
    unsigned minHours = max(1, GetPrivateProfileIntW(L"Proxy", L"MinHours", 6, iniPath.c_str()));
    unsigned maxHours = max(minHours, static_cast<unsigned>(GetPrivateProfileIntW(L"Proxy", L"MaxHours", 8, iniPath.c_str())));
    std::mt19937 rng(std::random_device{}());
    const UINT minMilliseconds = minHours * 60u * 60u * 1000u;
    const UINT maxMilliseconds = maxHours * 60u * 60u * 1000u;
    std::uniform_int_distribution<UINT> pick(minMilliseconds, maxMilliseconds);
    SetTimer(hwnd, PROXY_ROTATION_TIMER_ID, pick(rng), nullptr);
}

void UpdateMenuChecks(HWND hwnd)
{
    HMENU menu = GetMenu(hwnd);
    CheckMenuRadioItem(menu, IDM_GRAPHICS_AUTO, IDM_GRAPHICS_D3D11,
        forceDirectX11 ? IDM_GRAPHICS_D3D11 : IDM_GRAPHICS_AUTO, MF_BYCOMMAND);
    CheckMenuItem(menu, IDM_PROXY_ENABLED, MF_BYCOMMAND | (proxyEnabled ? MF_CHECKED : MF_UNCHECKED));
}

HMENU CreateClientMenu()
{
    HMENU bar = CreateMenu();
    HMENU graphics = CreatePopupMenu();
    AppendMenuW(graphics, MF_STRING, IDM_GRAPHICS_AUTO, L"Automatisch");
    AppendMenuW(graphics, MF_STRING, IDM_GRAPHICS_D3D11, L"DirectX 11 erzwingen");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(graphics), L"Grafik");
    HMENU proxy = CreatePopupMenu();
    AppendMenuW(proxy, MF_STRING, IDM_PROXY_ENABLED, L"HTTP(S)-Proxy verwenden");
    AppendMenuW(proxy, MF_STRING, IDM_PROXY_OPEN_LIST, L"Proxy-Liste öffnen...");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(proxy), L"Proxy");
    AppendMenuW(bar, MF_STRING, IDM_RESTART_GAME, L"Spiel neu starten");
    return bar;
}

// ------------------------------------------------------------
// Prüfen, ob das Ziel-Fenster bereits vollflächig anliegt
// ------------------------------------------------------------

bool IsWindowInFullScreen(HWND hwndContainer, HWND hwndTarget)
{
    if (!hwndContainer || !hwndTarget || !IsWindow(hwndTarget)) return false;

    RECT rcContainer = {};
    RECT rcTarget = {};

    GetClientRect(hwndContainer, &rcContainer);
    GetWindowRect(hwndTarget, &rcTarget);

    POINT ptTargetTopLeft = { rcTarget.left, rcTarget.top };
    ScreenToClient(hwndContainer, &ptTargetTopLeft);

    int targetWidth = rcTarget.right - rcTarget.left;
    int targetHeight = rcTarget.bottom - rcTarget.top;

    int containerWidth = rcContainer.right - rcContainer.left;
    int containerHeight = rcContainer.bottom - rcContainer.top;

    // Mindestens 95% der Zielgröße müssen erreicht sein
    return (targetWidth >= containerWidth * 0.95) && (targetHeight >= containerHeight * 0.95);
}

// ------------------------------------------------------------
// Remote Process DLL Injection
// ------------------------------------------------------------

bool InjectDLL(DWORD dwProcessId, const std::wstring& dllPath) {
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, dwProcessId);
    if (!hProcess) return false;

    size_t dllPathSize = (dllPath.length() + 1) * sizeof(wchar_t);
    LPVOID pRemoteMemory = VirtualAllocEx(hProcess, nullptr, dllPathSize, MEM_COMMIT, PAGE_READWRITE);
    if (!pRemoteMemory) {
        CloseHandle(hProcess);
        return false;
    }

    if (!WriteProcessMemory(hProcess, pRemoteMemory, dllPath.c_str(), dllPathSize, nullptr)) {
        VirtualFreeEx(hProcess, pRemoteMemory, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    LPVOID pLoadLibraryW = (LPVOID)GetProcAddress(hKernel32, "LoadLibraryW");
    if (!pLoadLibraryW) {
        VirtualFreeEx(hProcess, pRemoteMemory, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(
        hProcess,
        nullptr,
        0,
        (LPTHREAD_START_ROUTINE)pLoadLibraryW,
        pRemoteMemory,
        0,
        nullptr
    );

    if (!hThread) {
        VirtualFreeEx(hProcess, pRemoteMemory, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    WaitForSingleObject(hThread, INFINITE);
    VirtualFreeEx(hProcess, pRemoteMemory, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProcess);

    return true;
}

bool InjectAllDllsToRemoteProcess(HWND parentWindow)
{
    if (dllsInjected) return true;

    if (externalProcessId == 0) {
        MessageBox(parentWindow, L"OldofSea PID konnte nicht ermittelt werden!", L"Fehler", MB_OK | MB_ICONERROR);
        return false;
    }

    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring exePath = buffer;
    size_t pos = exePath.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        exePath = exePath.substr(0, pos);
    }

    std::wstring dllPath = exePath + L"\\" + EVERYDAY_DLL_NAME;
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(parentWindow, (L"DLL nicht gefunden:\n\n" + dllPath).c_str(),
            L"DLL fehlt", MB_OK | MB_ICONERROR);
        return false;
    }

    // MinHook ist im aktuellen MovementMod-Projekt bereits statisch gelinkt.
    // Deshalb darf hier nur die Haupt-DLL geladen werden.
    if (!InjectDLL(externalProcessId, dllPath)) {
        ShowWindowsError(parentWindow, L"Fehler beim Injizieren von EveryDayiamNEYDISCH.dll", GetLastError());
        return false;
    }

    dllsInjected = true;

    MessageBox(
        parentWindow,
        L"Das Spiel ist stabil im Vollbild.\nDie Haupt-DLL wurde erfolgreich geladen!",
        L"DLL Injection Erfolgreich",
        MB_OK | MB_ICONINFORMATION
    );

    return true;
}

// ------------------------------------------------------------
// Hauptfenster von OldofSea suchen
// ------------------------------------------------------------

BOOL CALLBACK FindWindowByProcessId(HWND hwnd, LPARAM lParam)
{
    DWORD windowProcessId = 0;
    GetWindowThreadProcessId(hwnd, &windowProcessId);

    DWORD wantedProcessId = static_cast<DWORD>(lParam);

    if (windowProcessId != wantedProcessId) {
        return TRUE;
    }

    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }

    if (GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }

    hExternalWindow = hwnd;
    return FALSE;
}

// ------------------------------------------------------------
// OldofSea starten
// ------------------------------------------------------------

bool StartExternalProgram(HWND parentWindow)
{
    if (externalProcessId != 0) {
        return false;
    }

    DWORD attributes = GetFileAttributes(PROGRAM_PATH);

    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        MessageBox(
            parentWindow,
            L"Die EXE wurde nicht gefunden:\n\nC:\\Games\\Old Of Sea\\default\\game\\OldofSea.exe",
            L"EXE nicht gefunden",
            MB_OK | MB_ICONERROR
        );
        return false;
    }

    std::wstring commandLine = L"\"" + std::wstring(PROGRAM_PATH) + L"\"";
    if (forceDirectX11) commandLine += L" -force-d3d11";
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    std::wstring selectedProxy;
    unsigned nextProxyCycleIndex = 0;
    std::vector<std::pair<std::wstring, std::wstring>> previousProxyValues;
    if (proxyEnabled) {
        if (!SelectProxyForCurrentCycle(selectedProxy, nextProxyCycleIndex)) {
            MessageBoxW(parentWindow,
                L"Proxy ist aktiviert, aber proxies.txt enthält keinen gültigen Eintrag.\n\nFormat: host:port:benutzer:passwort",
                L"Proxy-Konfiguration", MB_OK | MB_ICONERROR);
            return false;
        }
        const wchar_t* variableNames[] = { L"HTTP_PROXY", L"HTTPS_PROXY", L"http_proxy", L"https_proxy" };
        for (const wchar_t* name : variableNames) {
            wchar_t oldValue[32767] = {};
            DWORD oldLength = GetEnvironmentVariableW(name, oldValue, 32767);
            previousProxyValues.emplace_back(name, oldLength ? std::wstring(oldValue, oldLength) : std::wstring());
            // Ein leerer Wert ist die feste "ohne Proxy"-Phase des Zyklus.
            SetEnvironmentVariableW(name, selectedProxy.empty() ? nullptr : selectedProxy.c_str());
        }
    }

    STARTUPINFO startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);

    PROCESS_INFORMATION processInfo = {};

    BOOL success = CreateProcess(
        PROGRAM_PATH,
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED,
        nullptr,
        PROGRAM_DIRECTORY,
        &startupInfo,
        &processInfo
    );

    for (const auto& previous : previousProxyValues)
        SetEnvironmentVariableW(previous.first.c_str(), previous.second.empty() ? nullptr : previous.second.c_str());

    if (!success) {
        ShowWindowsError(parentWindow, L"OldofSea.exe konnte nicht gestartet werden", GetLastError());
        return false;
    }

    if (!AssignProcessToJobObject(processJob, processInfo.hProcess)) {
        DWORD error = GetLastError();
        TerminateProcess(processInfo.hProcess, 1);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        ShowWindowsError(parentWindow, L"Spielprozess konnte nicht isoliert werden", error);
        return false;
    }

    if (proxyEnabled) {
        WritePrivateProfileStringW(L"Proxy", L"CycleIndex",
            std::to_wstring(nextProxyCycleIndex).c_str(), iniPath.c_str());
    }

    ResumeThread(processInfo.hThread);

    externalProcess = processInfo;
    externalProcessId = processInfo.dwProcessId;

    windowSearchAttempts = 0;
    dllsInjected = false;

    if (externalProcess.hThread) {
        CloseHandle(externalProcess.hThread);
        externalProcess.hThread = nullptr;
    }

    AllowSetForegroundWindow(externalProcessId);

    SetTimer(parentWindow, WINDOW_SEARCH_TIMER_ID, WINDOW_SEARCH_INTERVAL, nullptr);
    ScheduleProxyRotation(parentWindow);

    return true;
}

// ------------------------------------------------------------
// OldofSea-Fenster erzwingen & positionieren
// ------------------------------------------------------------

void PositionExternalWindow(HWND parentWindow)
{
    if (!hExternalWindow || !IsWindow(hExternalWindow)) {
        return;
    }

    if (IsIconic(parentWindow)) {
        return;
    }

    RECT clientRect = {};
    GetClientRect(parentWindow, &clientRect);

    POINT topLeft = { clientRect.left, clientRect.top };
    ClientToScreen(parentWindow, &topLeft);

    int width = clientRect.right - clientRect.left;
    int height = clientRect.bottom - clientRect.top;

    if (width <= 0 || height <= 0) {
        return;
    }

    // Das Spielfenster bleibt ein eigenständiges Popup, damit Unity seine
    // normalen Maus- und Fokusereignisse erhält.
    SetWindowPos(
        hExternalWindow,
        HWND_TOP,
        topLeft.x,
        topLeft.y,
        width,
        height,
        SWP_SHOWWINDOW | SWP_NOOWNERZORDER | SWP_NOACTIVATE
    );

    // Falls Unity das Child zwischenzeitlich selbst verkleinert hat, bleiben
    // sonst alte Bildpixel in der freigelegten Parent-Fläche stehen.
    RedrawWindow(
        parentWindow,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN
    );

}

bool ExternalWindowNeedsPosition(HWND parentWindow)
{
    if (!parentWindow || !hExternalWindow || !IsWindow(hExternalWindow) ||
        IsIconic(parentWindow))
        return false;

    RECT parentClient{};
    RECT gameRect{};
    if (!GetClientRect(parentWindow, &parentClient) ||
        !GetWindowRect(hExternalWindow, &gameRect))
        return false;

    POINT expectedTopLeft{ parentClient.left, parentClient.top };
    if (!ClientToScreen(parentWindow, &expectedTopLeft))
        return false;

    const int expectedWidth = parentClient.right - parentClient.left;
    const int expectedHeight = parentClient.bottom - parentClient.top;
    const int actualWidth = gameRect.right - gameRect.left;
    const int actualHeight = gameRect.bottom - gameRect.top;

    constexpr int tolerance = 2;
    return
        gameRect.left < expectedTopLeft.x - tolerance ||
        gameRect.left > expectedTopLeft.x + tolerance ||
        gameRect.top < expectedTopLeft.y - tolerance ||
        gameRect.top > expectedTopLeft.y + tolerance ||
        actualWidth < expectedWidth - tolerance ||
        actualWidth > expectedWidth + tolerance ||
        actualHeight < expectedHeight - tolerance ||
        actualHeight > expectedHeight + tolerance;
}

// ------------------------------------------------------------
// OldofSea-Fenster rahmenlos für Vollbild vorbereiten
// ------------------------------------------------------------

bool PrepareExternalWindow(HWND parentWindow)
{
    hExternalWindow = nullptr;

    EnumWindows(FindWindowByProcessId, static_cast<LPARAM>(externalProcessId));

    if (!hExternalWindow) {
        return false;
    }

    // Rahmenloses Popup beibehalten. Unity verarbeitet Maus und Fokus in
    // dieser Betriebsart korrekt; die Größe wird separat nachgeführt.
    LONG_PTR style = GetWindowLongPtr(hExternalWindow, GWL_STYLE);
    style &= ~static_cast<LONG_PTR>(WS_CHILD | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_DISABLED);
    style |= static_cast<LONG_PTR>(WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
    SetWindowLongPtr(hExternalWindow, GWL_STYLE, style);

    LONG_PTR extendedStyle = GetWindowLongPtr(hExternalWindow, GWL_EXSTYLE);
    extendedStyle &= ~static_cast<LONG_PTR>(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT);
    extendedStyle |= WS_EX_TOOLWINDOW;
    SetWindowLongPtr(hExternalWindow, GWL_EXSTYLE, extendedStyle);

    SetWindowLongPtr(hExternalWindow, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(parentWindow));

    EnableWindow(hExternalWindow, TRUE);

    // Apply the changed borderless styles immediately. The old order first
    // positioned the game correctly and then used SW_SHOWMAXIMIZED, which
    // overwrote that size. Unity only corrected its UI/backbuffer after the
    // user manually resized the wrapper.
    SetWindowPos(
        hExternalWindow,
        nullptr,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
        SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    // Restore first, then make the wrapper client area the final authoritative
    // game size. SetWindowPos inside PositionExternalWindow emits the resize
    // Unity needs without requiring a manual minimize/maximize cycle.
    ShowWindow(hExternalWindow, SW_RESTORE);
    PositionExternalWindow(parentWindow);

    UpdateWindow(hExternalWindow);
    SetForegroundWindow(hExternalWindow);
    SetFocus(hExternalWindow);

    return true;
}

// ------------------------------------------------------------
// Prozessdaten freigeben
// ------------------------------------------------------------

void ResetExternalProcess()
{
    if (externalProcess.hThread) {
        CloseHandle(externalProcess.hThread);
        externalProcess.hThread = nullptr;
    }

    if (externalProcess.hProcess) {
        CloseHandle(externalProcess.hProcess);
        externalProcess.hProcess = nullptr;
    }

    externalProcessId = 0;
    hExternalWindow = nullptr;

    windowSearchAttempts = 0;
    dllsInjected = false;
}

// ------------------------------------------------------------
// Programmeinstieg
// ------------------------------------------------------------

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    singleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\NEYDISCH_OwnGUI_SingleInstance");
    if (!singleInstanceMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Der Client läuft bereits.", WINDOW_TITLE, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    applicationDirectory.resize(MAX_PATH);
    DWORD pathLength = GetModuleFileNameW(nullptr, applicationDirectory.data(), MAX_PATH);
    applicationDirectory.resize(pathLength);
    size_t pathSlash = applicationDirectory.find_last_of(L"\\/");
    if (pathSlash != std::wstring::npos) applicationDirectory.resize(pathSlash);
    iniPath = applicationDirectory + L"\\client.ini";
    proxyListPath = applicationDirectory + L"\\proxies.txt";
    LoadClientSettings();

    processJob = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!processJob || !SetInformationJobObject(processJob, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        ShowWindowsError(nullptr, L"Prozess-Isolierung konnte nicht initialisiert werden", GetLastError());
        return 1;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    hDarkBackground = CreateSolidBrush(RGB(18, 18, 18));

    HICON largeIcon = reinterpret_cast<HICON>(
        LoadImage(hInstance, MAKEINTRESOURCE(IDI_OWNGUI), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR)
        );

    HICON smallIcon = reinterpret_cast<HICON>(
        LoadImage(hInstance, MAKEINTRESOURCE(IDI_OWNGUI), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR)
        );

    if (!smallIcon) {
        smallIcon = largeIcon;
    }

    WNDCLASSEX windowClass = {};
    windowClass.cbSize = sizeof(WNDCLASSEX);
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = hInstance;
    windowClass.lpszClassName = L"MyExternalWindowContainer";
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = hDarkBackground;
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.hIcon = largeIcon;
    windowClass.hIconSm = smallIcon;

    if (!RegisterClassEx(&windowClass)) {
        MessageBox(nullptr, L"Die Fensterklasse konnte nicht registriert werden.", L"Fehler", MB_OK | MB_ICONERROR);
        return 0;
    }

    HWND hwnd = CreateWindow(
        windowClass.lpszClassName,
        WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700,
        nullptr, CreateClientMenu(), hInstance, nullptr
    );

    if (!hwnd) {
        MessageBox(nullptr, L"Das Hauptfenster konnte nicht erstellt werden.", L"Fehler", MB_OK | MB_ICONERROR);
        return 0;
    }

    SendMessage(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(largeIcon));
    SendMessage(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
    SendMessage(hwnd, WM_SETICON, ICON_SMALL2, reinterpret_cast<LPARAM>(smallIcon));

    EnableDarkTitleBar(hwnd);
    ShowWindow(hwnd, SW_MAXIMIZE);
    UpdateWindow(hwnd);

    // Erst starten, wenn das Wrapper-Fenster bereits seine endgültige
    // maximierte Client-Größe besitzt. So bekommt das Spiel beim Start
    // nicht nacheinander mehrere widersprüchliche Größen.
    if (!StartExternalProgram(hwnd)) {
        DestroyWindow(hwnd);
        return 1;
    }

    MSG message = {};
    while (GetMessage(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    if (smallIcon && smallIcon != largeIcon) {
        DestroyIcon(smallIcon);
    }

    if (largeIcon) {
        DestroyIcon(largeIcon);
    }

    if (hDarkBackground) {
        DeleteObject(hDarkBackground);
        hDarkBackground = nullptr;
    }

    if (processJob) CloseHandle(processJob);
    if (singleInstanceMutex) CloseHandle(singleInstanceMutex);

    return static_cast<int>(message.wParam);
}

// ------------------------------------------------------------
// Fensterprozedur
// ------------------------------------------------------------

void BeginGameRestart(HWND hwnd)
{
    if (restartPending || externalProcessId == 0) return;
    restartPending = true;
    restartDeadline = GetTickCount64() + 15000;
    KillTimer(hwnd, PROXY_ROTATION_TIMER_ID);
    if (hExternalWindow && IsWindow(hExternalWindow))
        PostMessageW(hExternalWindow, WM_CLOSE, 0, 0);
    SetTimer(hwnd, RESTART_WAIT_TIMER_ID, 500, nullptr);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
    {
        EnableDarkTitleBar(hwnd);
        UpdateMenuChecks(hwnd);
        return 0;
    }

    case WM_COMMAND:
    {
        switch (LOWORD(wParam)) {
        case IDM_GRAPHICS_AUTO:
            forceDirectX11 = false; SaveClientSettings(); UpdateMenuChecks(hwnd); BeginGameRestart(hwnd); return 0;
        case IDM_GRAPHICS_D3D11:
            forceDirectX11 = true; SaveClientSettings(); UpdateMenuChecks(hwnd); BeginGameRestart(hwnd); return 0;
        case IDM_PROXY_ENABLED:
            proxyEnabled = !proxyEnabled; SaveClientSettings(); UpdateMenuChecks(hwnd); BeginGameRestart(hwnd); return 0;
        case IDM_PROXY_OPEN_LIST:
            if (GetFileAttributesW(proxyListPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                std::ofstream output(proxyListPath);
                output << "# Eine Zeile je HTTP(S)-Proxy: host:port:benutzer:passwort\n";
            }
            ShellExecuteW(hwnd, L"open", proxyListPath.c_str(), nullptr, applicationDirectory.c_str(), SW_SHOWNORMAL);
            return 0;
        case IDM_RESTART_GAME:
            BeginGameRestart(hwnd); return 0;
        }
        break;
    }

    case WM_ERASEBKGND:
    {
        RECT clientRect = {};
        GetClientRect(hwnd, &clientRect);

        FillRect(reinterpret_cast<HDC>(wParam), &clientRect, hDarkBackground);
        return 1;
    }

    case WM_TIMER:
    {
        if (wParam == WINDOW_SEARCH_TIMER_ID) {
            ++windowSearchAttempts;

            if (externalProcess.hProcess &&
                WaitForSingleObject(externalProcess.hProcess, 0) == WAIT_OBJECT_0) {

                KillTimer(hwnd, WINDOW_SEARCH_TIMER_ID);

                MessageBox(
                    hwnd,
                    L"OldofSea.exe wurde beendet, bevor sein Hauptfenster gefunden wurde.",
                    L"Fenster nicht gefunden",
                    MB_OK | MB_ICONERROR
                );

                ResetExternalProcess();
                return 0;
            }

            // 1. Spielfenster suchen und auf Vollbild stretchen
            if (PrepareExternalWindow(hwnd)) {
                KillTimer(hwnd, WINDOW_SEARCH_TIMER_ID);

                // Startet den Stabilisierungs-Timer (wartet, bis Auflösung fix ist)
                SetTimer(hwnd, STABILITY_DELAY_TIMER_ID, STABILITY_DELAY_INTERVAL, nullptr);
                return 0;
            }

            if (windowSearchAttempts >= MAX_WINDOW_SEARCH_ATTEMPTS) {
                KillTimer(hwnd, WINDOW_SEARCH_TIMER_ID);

                MessageBox(
                    hwnd,
                    L"OldofSea.exe läuft, aber sein Hauptfenster konnte nicht gefunden werden.",
                    L"Fenster nicht gefunden",
                    MB_OK | MB_ICONWARNING
                );

                return 0;
            }

            return 0;
        }

        // 2. Timer für die Vollbild-Prüfung und finale Injection
        if (wParam == STABILITY_DELAY_TIMER_ID) {
            PositionExternalWindow(hwnd);

            // Prüfen, ob das Spielfenster tatsächlich in Vollbildgröße anliegt
            if (IsWindowInFullScreen(hwnd, hExternalWindow)) {
                KillTimer(hwnd, STABILITY_DELAY_TIMER_ID);

                // Erst injizieren, wenn Vollbild stabil steht
                if (InjectAllDllsToRemoteProcess(hwnd)) {
                    SetTimer(hwnd, GAME_SIZE_GUARD_TIMER_ID, GAME_SIZE_GUARD_INTERVAL, nullptr);
                }
            }

            return 0;
        }

        if (wParam == GAME_SIZE_GUARD_TIMER_ID) {
            if (!externalProcess.hProcess ||
                WaitForSingleObject(externalProcess.hProcess, 0) == WAIT_OBJECT_0) {
                KillTimer(hwnd, GAME_SIZE_GUARD_TIMER_ID);
                ResetExternalProcess();
                return 0;
            }

            if (ExternalWindowNeedsPosition(hwnd)) {
                PositionExternalWindow(hwnd);
            }

            return 0;
        }

        if (wParam == PROXY_ROTATION_TIMER_ID) {
            BeginGameRestart(hwnd);
            return 0;
        }

        if (wParam == RESTART_WAIT_TIMER_ID) {
            bool exited = !externalProcess.hProcess ||
                WaitForSingleObject(externalProcess.hProcess, 0) == WAIT_OBJECT_0;
            if (!exited && GetTickCount64() >= restartDeadline) {
                TerminateProcess(externalProcess.hProcess, 0);
                WaitForSingleObject(externalProcess.hProcess, 2000);
                exited = true;
            }
            if (exited) {
                KillTimer(hwnd, RESTART_WAIT_TIMER_ID);
                ResetExternalProcess();
                restartPending = false;
                StartExternalProgram(hwnd);
            }
            return 0;
        }

        return 0;
    }

    case WM_MOVE:
    {
        PositionExternalWindow(hwnd);
        return 0;
    }

    case WM_SIZE:
    {
        if (wParam == SIZE_MINIMIZED) {
            return 0;
        }

        if (hExternalWindow && IsWindow(hExternalWindow)) {
            ShowWindow(hExternalWindow, SW_SHOW);
            PositionExternalWindow(hwnd);
        }

        return 0;
    }

    case WM_EXITSIZEMOVE:
    {
        PositionExternalWindow(hwnd);
        return 0;
    }

    case WM_ACTIVATE:
    {
        if (LOWORD(wParam) != WA_INACTIVE && hExternalWindow && IsWindow(hExternalWindow)) {
            PositionExternalWindow(hwnd);

            SetWindowPos(
                hExternalWindow,
                HWND_TOP,
                0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER | SWP_NOACTIVATE
            );
        }

        return 0;
    }

    case WM_DWMCOMPOSITIONCHANGED:
    {
        return 0;
    }

    case WM_CLOSE:
    {
        KillTimer(hwnd, WINDOW_SEARCH_TIMER_ID);
        KillTimer(hwnd, STABILITY_DELAY_TIMER_ID);
        KillTimer(hwnd, GAME_SIZE_GUARD_TIMER_ID);
        KillTimer(hwnd, PROXY_ROTATION_TIMER_ID);
        KillTimer(hwnd, RESTART_WAIT_TIMER_ID);

        if (hExternalWindow && IsWindow(hExternalWindow)) {
            PostMessage(hExternalWindow, WM_CLOSE, 0, 0);
            hExternalWindow = nullptr;
        }

        DestroyWindow(hwnd);
        return 0;
    }

    case WM_DESTROY:
    {
        KillTimer(hwnd, WINDOW_SEARCH_TIMER_ID);
        KillTimer(hwnd, STABILITY_DELAY_TIMER_ID);
        KillTimer(hwnd, GAME_SIZE_GUARD_TIMER_ID);
        KillTimer(hwnd, PROXY_ROTATION_TIMER_ID);
        KillTimer(hwnd, RESTART_WAIT_TIMER_ID);

        ResetExternalProcess();

        PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProc(hwnd, message, wParam, lParam);
}
