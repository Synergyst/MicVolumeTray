#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>

#include "resource.h"

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shell32.lib")

static const wchar_t* APP_CLASS_NAME = L"MicVolumeTrayWindowClass";
static const wchar_t* APP_TITLE = L"Microphone Volume Keeper";
static const wchar_t* INI_FILE_NAME = L"MicVolumeTray.ini";
static const wchar_t* INI_SECTION_MAIN = L"Main";

#define ID_COMBO_DEVICES         1001
#define ID_EDIT_VOLUME           1002
#define ID_SPIN_VOLUME           1003
#define ID_EDIT_INTERVAL         1004
#define ID_SPIN_INTERVAL         1005
#define ID_BUTTON_START          1006
#define ID_BUTTON_STOP           1007
#define ID_BUTTON_REFRESH        1008
#define ID_CHECK_MINTRAY         1009
#define ID_CHECK_CLOSETRAY       1010
#define ID_STATIC_STATUS         1011
#define ID_BUTTON_ADD_MIC        1012
#define ID_CHECK_EXACT_MATCH     1013
#define ID_LIST_SAVED_MICS       1014
#define ID_BUTTON_SAVE_MIC       1015
#define ID_BUTTON_REMOVE_MIC     1016
#define ID_STATIC_EDITING        1017
#define ID_BUTTON_CLEAR_ALL      1018

#define ID_TIMER_APPLY           2001

#define WM_TRAYICON              (WM_APP + 1)

#define ID_TRAY_RESTORE          3001
#define ID_TRAY_EXIT             3002

template <typename T>
class ComPtr
{
public:
    ComPtr() : ptr_(nullptr) {}
    ~ComPtr() { Reset(); }

    T* Get() const { return ptr_; }

    T** Put()
    {
        Reset();
        return &ptr_;
    }

    T* operator->() const { return ptr_; }
    operator bool() const { return ptr_ != nullptr; }

    void Reset()
    {
        if (ptr_)
        {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_;
};

struct CaptureDeviceInfo
{
    std::wstring id;
    std::wstring name;
};

struct SavedMicProfile
{
    std::wstring savedName;
    int volume = 50;
    bool exactMatch = false;
    bool present = false;
    bool selected = false;
};

static HINSTANCE g_hInst = nullptr;
static HWND g_hWnd = nullptr;

static HWND g_hComboDevices = nullptr;
static HWND g_hEditVolume = nullptr;
static HWND g_hSpinVolume = nullptr;
static HWND g_hEditInterval = nullptr;
static HWND g_hSpinInterval = nullptr;
static HWND g_hButtonStart = nullptr;
static HWND g_hButtonStop = nullptr;
static HWND g_hButtonRefresh = nullptr;
static HWND g_hButtonAddMic = nullptr;
static HWND g_hButtonSaveMic = nullptr;
static HWND g_hButtonRemoveMic = nullptr;
static HWND g_hButtonClearAll = nullptr;
static HWND g_hCheckMinTray = nullptr;
static HWND g_hCheckCloseTray = nullptr;
static HWND g_hCheckExactMatch = nullptr;
static HWND g_hStatus = nullptr;
static HWND g_hEditingLabel = nullptr;
static HWND g_hListSavedMics = nullptr;

static std::vector<CaptureDeviceInfo> g_devices;
static std::vector<SavedMicProfile> g_savedProfiles;
static int g_selectedSavedIndex = -1;
static bool g_timerRunning = false;
static bool g_isExiting = false;
static bool g_loadedStartRunning = false;
static NOTIFYICONDATAW g_nid = {};
static bool g_trayAdded = false;
static std::wstring g_iniPath;

static std::wstring HResultToString(HRESULT hr)
{
    wchar_t* msg = nullptr;
    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        hr,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&msg,
        0,
        nullptr
    );

    std::wstringstream ss;
    ss << L"0x" << std::hex << static_cast<unsigned long>(hr);

    if (msg)
    {
        ss << L" - " << msg;
        LocalFree(msg);
    }

    return ss.str();
}

static std::wstring ToLowerCopy(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch)
        {
            return (wchar_t)towlower(ch);
        });
    return text;
}

static void TrimInPlace(std::wstring& s)
{
    auto notSpace = [](wchar_t ch)
        {
            return !iswspace(ch);
        };

    while (!s.empty() && !notSpace(s.front()))
    {
        s.erase(s.begin());
    }

    while (!s.empty() && !notSpace(s.back()))
    {
        s.pop_back();
    }
}

static std::wstring NormalizeDeviceName(const std::wstring& name, bool exactMatch)
{
    std::wstring s = name;
    TrimInPlace(s);

    if (exactMatch)
    {
        return ToLowerCopy(s);
    }

    std::wstring lower = ToLowerCopy(s);

    if (lower.size() >= 4)
    {
        size_t p = lower.rfind(L" (");
        if (p != std::wstring::npos && lower.back() == L')')
        {
            bool digitsOnly = true;
            for (size_t i = p + 2; i + 1 < lower.size(); ++i)
            {
                if (!iswdigit(lower[i]))
                {
                    digitsOnly = false;
                    break;
                }
            }

            if (digitsOnly)
            {
                lower = lower.substr(0, p);
            }
        }
    }

    return lower;
}

static void SetStatus(const std::wstring& text)
{
    if (g_hStatus)
    {
        SetWindowTextW(g_hStatus, text.c_str());
    }
}

static void SetEditingLabel(const std::wstring& text)
{
    if (g_hEditingLabel)
    {
        SetWindowTextW(g_hEditingLabel, text.c_str());
    }
}

static std::wstring GetWindowTextString(HWND hWnd)
{
    int len = GetWindowTextLengthW(hWnd);
    if (len <= 0)
    {
        return L"";
    }

    std::wstring text;
    text.resize(static_cast<size_t>(len));
    GetWindowTextW(hWnd, text.data(), len + 1);
    return text;
}

static int GetIntFromEdit(HWND hEdit, int fallbackValue)
{
    std::wstring text = GetWindowTextString(hEdit);
    if (text.empty())
    {
        return fallbackValue;
    }

    try
    {
        return std::stoi(text);
    }
    catch (...)
    {
        return fallbackValue;
    }
}

static HICON LoadAppIcon(int cx, int cy)
{
    HICON hIcon = (HICON)LoadImageW(
        g_hInst,
        MAKEINTRESOURCEW(IDI_APPICON),
        IMAGE_ICON,
        cx,
        cy,
        LR_DEFAULTCOLOR
    );

    if (hIcon)
    {
        return hIcon;
    }

    return LoadIconW(nullptr, IDI_INFORMATION);
}

static std::wstring GetIniPath()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);

    std::wstring exePath = path;
    size_t slashPos = exePath.find_last_of(L"\\/");
    if (slashPos == std::wstring::npos)
    {
        return INI_FILE_NAME;
    }

    return exePath.substr(0, slashPos + 1) + INI_FILE_NAME;
}

static void WriteIniString(const wchar_t* key, const std::wstring& value)
{
    WritePrivateProfileStringW(INI_SECTION_MAIN, key, value.c_str(), g_iniPath.c_str());
}

static void WriteIniInt(const wchar_t* key, int value)
{
    wchar_t buf[64] = {};
    wsprintfW(buf, L"%d", value);
    WritePrivateProfileStringW(INI_SECTION_MAIN, key, buf, g_iniPath.c_str());
}

static std::wstring ReadIniString(const wchar_t* key, const wchar_t* defaultValue)
{
    wchar_t buf[4096] = {};
    GetPrivateProfileStringW(
        INI_SECTION_MAIN,
        key,
        defaultValue,
        buf,
        static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])),
        g_iniPath.c_str()
    );
    return buf;
}

static int ReadIniInt(const wchar_t* key, int defaultValue)
{
    return (int)GetPrivateProfileIntW(INI_SECTION_MAIN, key, defaultValue, g_iniPath.c_str());
}

static void SaveWindowPlacement()
{
    if (!g_hWnd)
    {
        return;
    }

    WINDOWPLACEMENT wp = {};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(g_hWnd, &wp))
    {
        return;
    }

    RECT rc = wp.rcNormalPosition;
    WriteIniInt(L"WindowLeft", rc.left);
    WriteIniInt(L"WindowTop", rc.top);
    WriteIniInt(L"WindowRight", rc.right);
    WriteIniInt(L"WindowBottom", rc.bottom);
}

static void RestoreWindowPlacement()
{
    int left = ReadIniInt(L"WindowLeft", CW_USEDEFAULT);
    int top = ReadIniInt(L"WindowTop", CW_USEDEFAULT);
    int right = ReadIniInt(L"WindowRight", CW_USEDEFAULT);
    int bottom = ReadIniInt(L"WindowBottom", CW_USEDEFAULT);

    if (left == CW_USEDEFAULT || top == CW_USEDEFAULT || right == CW_USEDEFAULT || bottom == CW_USEDEFAULT)
    {
        return;
    }

    int width = right - left;
    int height = bottom - top;

    if (width < 500) width = 760;
    if (height < 350) height = 560;

    SetWindowPos(g_hWnd, nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

static std::wstring GetDeviceFriendlyName(IMMDevice* device)
{
    if (!device)
    {
        return L"(Unknown Device)";
    }

    ComPtr<IPropertyStore> props;
    HRESULT hr = device->OpenPropertyStore(STGM_READ, props.Put());
    if (FAILED(hr))
    {
        return L"(Unknown Device)";
    }

    PROPVARIANT varName;
    PropVariantInit(&varName);

    hr = props->GetValue(PKEY_Device_FriendlyName, &varName);
    if (FAILED(hr))
    {
        PropVariantClear(&varName);
        return L"(Unknown Device)";
    }

    std::wstring result =
        (varName.vt == VT_LPWSTR && varName.pwszVal)
        ? varName.pwszVal
        : L"(Unknown Device)";

    PropVariantClear(&varName);
    return result;
}

static bool EnumerateCaptureDevices(std::vector<CaptureDeviceInfo>& outDevices, std::wstring& error)
{
    outDevices.clear();
    error.clear();

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)enumerator.Put()
    );

    if (FAILED(hr))
    {
        error = L"Failed to create MMDeviceEnumerator: " + HResultToString(hr);
        return false;
    }

    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, collection.Put());
    if (FAILED(hr))
    {
        error = L"Failed to enumerate capture devices: " + HResultToString(hr);
        return false;
    }

    UINT count = 0;
    hr = collection->GetCount(&count);
    if (FAILED(hr))
    {
        error = L"Failed to get device count: " + HResultToString(hr);
        return false;
    }

    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> device;
        hr = collection->Item(i, device.Put());
        if (FAILED(hr))
        {
            continue;
        }

        LPWSTR id = nullptr;
        hr = device->GetId(&id);
        if (FAILED(hr) || !id)
        {
            continue;
        }

        CaptureDeviceInfo info;
        info.id = id;
        info.name = GetDeviceFriendlyName(device.Get());

        CoTaskMemFree(id);
        outDevices.push_back(std::move(info));
    }

    return true;
}

static bool SetCaptureDeviceVolumeById(const std::wstring& deviceId, int volumePercent, std::wstring& error)
{
    error.clear();

    if (volumePercent < 0) volumePercent = 0;
    if (volumePercent > 100) volumePercent = 100;

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)enumerator.Put()
    );

    if (FAILED(hr))
    {
        error = L"Failed to create MMDeviceEnumerator: " + HResultToString(hr);
        return false;
    }

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDevice(deviceId.c_str(), device.Put());
    if (FAILED(hr))
    {
        error = L"Failed to get device: " + HResultToString(hr);
        return false;
    }

    ComPtr<IAudioEndpointVolume> endpointVolume;
    hr = device->Activate(
        __uuidof(IAudioEndpointVolume),
        CLSCTX_ALL,
        nullptr,
        (void**)endpointVolume.Put()
    );

    if (FAILED(hr))
    {
        error = L"Failed to activate IAudioEndpointVolume: " + HResultToString(hr);
        return false;
    }

    float scalar = static_cast<float>(volumePercent) / 100.0f;
    hr = endpointVolume->SetMasterVolumeLevelScalar(scalar, nullptr);
    if (FAILED(hr))
    {
        error = L"Failed to set microphone volume: " + HResultToString(hr);
        return false;
    }

    return true;
}

static bool SetCaptureDeviceVolumeByNameMatch(const std::wstring& wantedName, int volumePercent, bool exactMatch, std::wstring& error)
{
    error.clear();

    std::vector<CaptureDeviceInfo> currentDevices;
    std::wstring enumError;
    if (!EnumerateCaptureDevices(currentDevices, enumError))
    {
        error = enumError;
        return false;
    }

    std::wstring wantedNorm = NormalizeDeviceName(wantedName, exactMatch);
    bool appliedAny = false;
    bool foundAny = false;
    std::vector<std::wstring> failures;

    for (const auto& dev : currentDevices)
    {
        std::wstring devNorm = NormalizeDeviceName(dev.name, exactMatch);
        if (devNorm != wantedNorm)
        {
            continue;
        }

        foundAny = true;

        std::wstring applyError;
        if (SetCaptureDeviceVolumeById(dev.id, volumePercent, applyError))
        {
            appliedAny = true;
        }
        else
        {
            failures.push_back(dev.name + L": " + applyError);
        }
    }

    if (appliedAny)
    {
        return true;
    }

    if (!foundAny)
    {
        error = L"No matching microphone currently present.";
        return false;
    }

    if (!failures.empty())
    {
        error = failures.front();
        return false;
    }

    error = L"Unable to apply microphone volume.";
    return false;
}

static bool MicIsPresentByName(const std::wstring& wantedName, bool exactMatch)
{
    std::vector<CaptureDeviceInfo> currentDevices;
    std::wstring enumError;
    if (!EnumerateCaptureDevices(currentDevices, enumError))
    {
        return false;
    }

    std::wstring wantedNorm = NormalizeDeviceName(wantedName, exactMatch);

    for (const auto& dev : currentDevices)
    {
        if (NormalizeDeviceName(dev.name, exactMatch) == wantedNorm)
        {
            return true;
        }
    }

    return false;
}

static void LoadSavedProfiles()
{
    g_savedProfiles.clear();

    std::wstring raw = ReadIniString(L"SavedMics", L"");
    if (raw.empty())
    {
        return;
    }

    size_t pos = 0;
    while (pos < raw.size())
    {
        size_t next = raw.find(L'|', pos);
        std::wstring token = raw.substr(pos, next == std::wstring::npos ? std::wstring::npos : next - pos);
        if (!token.empty())
        {
            size_t sep1 = token.find(L'\x1f');
            size_t sep2 = token.find(L'\x1e');
            size_t sep = (sep1 != std::wstring::npos) ? sep1 : sep2;

            if (sep != std::wstring::npos)
            {
                std::wstring name = token.substr(0, sep);
                std::wstring rest = token.substr(sep + 1);
                TrimInPlace(name);
                TrimInPlace(rest);

                int vol = 50;
                int exact = 0;
                size_t sepVolExact = rest.find(L'\x1e');
                if (sepVolExact != std::wstring::npos)
                {
                    std::wstring volStr = rest.substr(0, sepVolExact);
                    std::wstring exactStr = rest.substr(sepVolExact + 1);
                    try { vol = std::stoi(volStr); }
                    catch (...) { vol = 50; }
                    try { exact = std::stoi(exactStr); }
                    catch (...) { exact = 0; }
                }
                else
                {
                    try { vol = std::stoi(rest); }
                    catch (...) { vol = 50; }
                }

                if (vol < 0) vol = 0;
                if (vol > 100) vol = 100;

                SavedMicProfile p;
                p.savedName = name;
                p.volume = vol;
                p.exactMatch = (exact != 0);
                p.present = MicIsPresentByName(p.savedName, p.exactMatch);
                g_savedProfiles.push_back(std::move(p));
            }
        }

        if (next == std::wstring::npos)
        {
            break;
        }
        pos = next + 1;
    }
}

static void SaveSavedProfiles()
{
    std::wstring raw;
    for (size_t i = 0; i < g_savedProfiles.size(); ++i)
    {
        if (i)
        {
            raw += L"|";
        }

        raw += g_savedProfiles[i].savedName;
        raw += L"\x1f";
        raw += std::to_wstring(g_savedProfiles[i].volume);
        raw += L"\x1e";
        raw += g_savedProfiles[i].exactMatch ? L"1" : L"0";
    }

    WriteIniString(L"SavedMics", raw);
    WriteIniInt(L"SavedMicCount", (int)g_savedProfiles.size());
}

static void SaveSettings()
{
    if (!g_hWnd)
    {
        return;
    }

    int volume = GetIntFromEdit(g_hEditVolume, 50);
    int interval = GetIntFromEdit(g_hEditInterval, 5);
    int minimizeToTray = (SendMessageW(g_hCheckMinTray, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    int closeToTray = (SendMessageW(g_hCheckCloseTray, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    int running = g_timerRunning ? 1 : 0;
    int exactMatch = (SendMessageW(g_hCheckExactMatch, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;

    WriteIniInt(L"Volume", volume);
    WriteIniInt(L"Interval", interval);
    WriteIniInt(L"MinimizeToTray", minimizeToTray);
    WriteIniInt(L"CloseToTray", closeToTray);
    WriteIniInt(L"TimerRunning", running);
    WriteIniInt(L"ExactNameMatchOnly", exactMatch);

    SaveSavedProfiles();
    SaveWindowPlacement();
}

static void ApplyLoadedSettingsToControls()
{
    int volume = ReadIniInt(L"Volume", 50);
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    int interval = ReadIniInt(L"Interval", 5);
    if (interval < 1) interval = 1;
    if (interval > 86400) interval = 86400;

    wchar_t buf[64] = {};
    wsprintfW(buf, L"%d", volume);
    SetWindowTextW(g_hEditVolume, buf);

    wsprintfW(buf, L"%d", interval);
    SetWindowTextW(g_hEditInterval, buf);

    SendMessageW(
        g_hCheckMinTray,
        BM_SETCHECK,
        ReadIniInt(L"MinimizeToTray", 1) ? BST_CHECKED : BST_UNCHECKED,
        0
    );

    SendMessageW(
        g_hCheckCloseTray,
        BM_SETCHECK,
        ReadIniInt(L"CloseToTray", 1) ? BST_CHECKED : BST_UNCHECKED,
        0
    );

    SendMessageW(
        g_hCheckExactMatch,
        BM_SETCHECK,
        ReadIniInt(L"ExactNameMatchOnly", 0) ? BST_CHECKED : BST_UNCHECKED,
        0
    );

    g_loadedStartRunning = ReadIniInt(L"TimerRunning", 0) ? true : false;
}

static void UpdateSavedMicPresence()
{
    bool anyPresent = false;
    for (auto& p : g_savedProfiles)
    {
        p.present = MicIsPresentByName(p.savedName, p.exactMatch);
        if (p.present)
        {
            anyPresent = true;
        }
    }

    EnableWindow(g_hButtonAddMic, anyPresent ? TRUE : FALSE);
}

static std::wstring GetStatusStringForSavedMic(const SavedMicProfile& p)
{
    std::wstringstream ss;
    ss << p.savedName << L" | ";
    ss << L"Vol " << p.volume << L"% | ";
    ss << (p.exactMatch ? L"Exact" : L"Loose") << L" | ";
    ss << (p.present ? L"Present" : L"Unplugged");
    return ss.str();
}

static void RefreshSavedMicList()
{
    ListView_DeleteAllItems(g_hListSavedMics);

    for (size_t i = 0; i < g_savedProfiles.size(); ++i)
    {
        const auto& p = g_savedProfiles[i];

        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = (int)i;
        item.iSubItem = 0;
        item.lParam = (LPARAM)i;
        std::wstring name = p.savedName;
        item.pszText = name.data();
        ListView_InsertItem(g_hListSavedMics, &item);

        std::wstring vol = std::to_wstring(p.volume) + L"%";
        ListView_SetItemText(g_hListSavedMics, (int)i, 1, vol.data());
        ListView_SetItemText(g_hListSavedMics, (int)i, 2, (LPWSTR)(p.exactMatch ? L"Exact" : L"Loose"));
        ListView_SetItemText(g_hListSavedMics, (int)i, 3, (LPWSTR)(p.present ? L"Present" : L"Unplugged"));
    }

    if (g_savedProfiles.empty())
    {
        SetStatus(L"No saved microphones yet. Select a microphone and click 'Add microphone'.");
    }

    UpdateSavedMicPresence();
}

static void SelectSavedMicByIndex(int index)
{
    if (index < 0 || index >= (int)g_savedProfiles.size())
    {
        g_selectedSavedIndex = -1;
        SetEditingLabel(L"Editing: New unsaved microphone");
        return;
    }

    g_selectedSavedIndex = index;

    const auto& p = g_savedProfiles[(size_t)index];

    wchar_t buf[64] = {};
    wsprintfW(buf, L"%d", p.volume);
    SetWindowTextW(g_hEditVolume, buf);

    SendMessageW(
        g_hCheckExactMatch,
        BM_SETCHECK,
        p.exactMatch ? BST_CHECKED : BST_UNCHECKED,
        0
    );

    std::wstringstream ss;
    ss << L"Editing: " << p.savedName;
    SetEditingLabel(ss.str());

    ListView_SetItemState(g_hListSavedMics, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(g_hListSavedMics, index, FALSE);
}

static void ClearEditingToNewUnsaved()
{
    g_selectedSavedIndex = -1;
    SetEditingLabel(L"Editing: New unsaved microphone");
    SendMessageW(g_hComboDevices, CB_SETCURSEL, 0, 0);
}

static bool GetSelectedDeviceName(std::wstring& outName)
{
    outName.clear();

    int sel = (int)SendMessageW(g_hComboDevices, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR || sel < 0 || sel >= (int)g_devices.size())
    {
        return false;
    }

    outName = g_devices[static_cast<size_t>(sel)].name;
    return true;
}

static void SyncCurrentControlsToSelectedProfileOrNew()
{
    int sel = (int)SendMessageW(g_hListSavedMics, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (sel >= 0 && sel < (int)g_savedProfiles.size())
    {
        SelectSavedMicByIndex(sel);
        return;
    }

    ClearEditingToNewUnsaved();
}

static void RefreshDeviceList()
{
    SendMessageW(g_hComboDevices, CB_RESETCONTENT, 0, 0);
    g_devices.clear();

    std::wstring error;
    if (!EnumerateCaptureDevices(g_devices, error))
    {
        SetStatus(L"Device enumeration failed: " + error);
        return;
    }

    for (const auto& dev : g_devices)
    {
        SendMessageW(g_hComboDevices, CB_ADDSTRING, 0, (LPARAM)dev.name.c_str());
    }

    if (!g_devices.empty())
    {
        SendMessageW(g_hComboDevices, CB_SETCURSEL, 0, 0);
        SetStatus(L"Ready.");
    }
    else
    {
        SetStatus(L"No active capture devices found.");
    }

    UpdateSavedMicPresence();
    RefreshSavedMicList();
}

static bool ApplySavedProfile(const SavedMicProfile& prof)
{
    std::wstring error;
    if (SetCaptureDeviceVolumeByNameMatch(prof.savedName, prof.volume, prof.exactMatch, error))
    {
        return true;
    }
    return false;
}

static void ApplyAllSavedProfiles()
{
    if (g_savedProfiles.empty())
    {
        SetStatus(L"No saved microphones configured.");
        return;
    }

    int applied = 0;
    int missing = 0;
    int failed = 0;

    UpdateSavedMicPresence();

    for (const auto& prof : g_savedProfiles)
    {
        if (!prof.present)
        {
            ++missing;
            continue;
        }

        if (ApplySavedProfile(prof))
        {
            ++applied;
        }
        else
        {
            ++failed;
        }
    }

    std::wstringstream ss;
    ss << L"Applied " << applied << L" microphone(s)";
    if (missing > 0)
    {
        ss << L", " << missing << L" unplugged";
    }
    if (failed > 0)
    {
        ss << L", " << failed << L" failed";
    }
    ss << L".";
    SetStatus(ss.str());

    RefreshSavedMicList();
}

static void AddOrUpdateCurrentMicFromControls()
{
    std::wstring selectedName;
    if (!GetSelectedDeviceName(selectedName))
    {
        SetStatus(L"No microphone selected.");
        return;
    }

    int volume = GetIntFromEdit(g_hEditVolume, 50);
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    bool exactMatch = (SendMessageW(g_hCheckExactMatch, BM_GETCHECK, 0, 0) == BST_CHECKED);

    if (g_selectedSavedIndex >= 0 && g_selectedSavedIndex < (int)g_savedProfiles.size())
    {
        auto& p = g_savedProfiles[(size_t)g_selectedSavedIndex];
        p.savedName = selectedName;
        p.volume = volume;
        p.exactMatch = exactMatch;
        p.present = MicIsPresentByName(p.savedName, p.exactMatch);
        SaveSavedProfiles();
        RefreshSavedMicList();

        std::wstringstream ss;
        ss << L"Updated saved microphone: " << selectedName;
        SetStatus(ss.str());
        SelectSavedMicByIndex(g_selectedSavedIndex);
        return;
    }

    for (const auto& p : g_savedProfiles)
    {
        if (NormalizeDeviceName(p.savedName, p.exactMatch) == NormalizeDeviceName(selectedName, exactMatch))
        {
            SetStatus(L"That microphone is already saved.");
            return;
        }
    }

    SavedMicProfile p;
    p.savedName = selectedName;
    p.volume = volume;
    p.exactMatch = exactMatch;
    p.present = MicIsPresentByName(p.savedName, p.exactMatch);
    g_savedProfiles.push_back(std::move(p));
    g_selectedSavedIndex = (int)g_savedProfiles.size() - 1;

    SaveSavedProfiles();
    RefreshSavedMicList();

    std::wstringstream ss;
    ss << L"Saved microphone: " << selectedName;
    SetStatus(ss.str());
    SelectSavedMicByIndex(g_selectedSavedIndex);
}

static void RemoveSelectedSavedMic()
{
    if (g_selectedSavedIndex < 0 || g_selectedSavedIndex >= (int)g_savedProfiles.size())
    {
        SetStatus(L"No saved microphone selected.");
        return;
    }

    g_savedProfiles.erase(g_savedProfiles.begin() + g_selectedSavedIndex);
    if (g_savedProfiles.empty())
    {
        g_selectedSavedIndex = -1;
        ClearEditingToNewUnsaved();
    }
    else if (g_selectedSavedIndex >= (int)g_savedProfiles.size())
    {
        g_selectedSavedIndex = (int)g_savedProfiles.size() - 1;
    }

    SaveSavedProfiles();
    RefreshSavedMicList();

    if (g_selectedSavedIndex >= 0)
    {
        SelectSavedMicByIndex(g_selectedSavedIndex);
    }
    else
    {
        SetEditingLabel(L"Editing: New unsaved microphone");
    }

    SetStatus(L"Selected microphone removed.");
}

static void ClearAllSavedMics()
{
    g_savedProfiles.clear();
    g_selectedSavedIndex = -1;
    SaveSavedProfiles();
    RefreshSavedMicList();
    SetEditingLabel(L"Editing: New unsaved microphone");
    SetStatus(L"All saved microphones removed.");
}

static void StartTimerApply()
{
    int interval = GetIntFromEdit(g_hEditInterval, 5);
    if (interval < 1) interval = 1;
    if (interval > 86400) interval = 86400;

    wchar_t buf[32] = {};
    wsprintfW(buf, L"%d", interval);
    SetWindowTextW(g_hEditInterval, buf);

    KillTimer(g_hWnd, ID_TIMER_APPLY);
    SetTimer(g_hWnd, ID_TIMER_APPLY, static_cast<UINT>(interval * 1000), nullptr);
    g_timerRunning = true;

    ApplyAllSavedProfiles();
    SaveSettings();
}

static void StopTimerApply()
{
    KillTimer(g_hWnd, ID_TIMER_APPLY);
    g_timerRunning = false;
    SetStatus(L"Stopped.");
    SaveSettings();
}

static bool AddTrayIcon(HWND hWnd)
{
    if (g_trayAdded)
    {
        return true;
    }

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadAppIcon(16, 16);
    (void)lstrcpynW(g_nid.szTip, APP_TITLE, static_cast<int>(sizeof(g_nid.szTip) / sizeof(g_nid.szTip[0])));

    if (Shell_NotifyIconW(NIM_ADD, &g_nid))
    {
        g_trayAdded = true;
        return true;
    }

    if (g_nid.hIcon)
    {
        DestroyIcon(g_nid.hIcon);
        g_nid.hIcon = nullptr;
    }

    return false;
}

static void RemoveTrayIcon()
{
    if (g_trayAdded)
    {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayAdded = false;
    }

    if (g_nid.hIcon)
    {
        DestroyIcon(g_nid.hIcon);
        g_nid.hIcon = nullptr;
    }
}

static void RestoreFromTray(HWND hWnd)
{
    ShowWindow(hWnd, SW_SHOW);
    ShowWindow(hWnd, SW_RESTORE);
    SetForegroundWindow(hWnd);
}

static void MinimizeToTray(HWND hWnd)
{
    AddTrayIcon(hWnd);
    ShowWindow(hWnd, SW_HIDE);
}

static void ShowTrayMenu(HWND hWnd)
{
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu)
    {
        return;
    }

    AppendMenuW(hMenu, MF_STRING, ID_TRAY_RESTORE, L"Restore");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");

    POINT pt = {};
    GetCursorPos(&pt);

    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, nullptr);
    DestroyMenu(hMenu);
}

static void CreateListViewColumns(HWND hList)
{
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.pszText = (LPWSTR)L"Microphone";
    col.cx = 250;
    ListView_InsertColumn(hList, 0, &col);

    col.pszText = (LPWSTR)L"Volume";
    col.cx = 70;
    ListView_InsertColumn(hList, 1, &col);

    col.pszText = (LPWSTR)L"Match";
    col.cx = 80;
    ListView_InsertColumn(hList, 2, &col);

    col.pszText = (LPWSTR)L"Status";
    col.cx = 90;
    ListView_InsertColumn(hList, 3, &col);
}

static void CreateControls(HWND hWnd)
{
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    auto SetControlFont = [&](HWND c)
        {
            SendMessageW(c, WM_SETFONT, (WPARAM)hFont, TRUE);
        };

    HWND hLabelMic = CreateWindowW(L"STATIC", L"Detected microphone:",
        WS_CHILD | WS_VISIBLE, 20, 16, 150, 20, hWnd, nullptr, g_hInst, nullptr);
    SetControlFont(hLabelMic);

    g_hComboDevices = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        170, 12, 380, 300, hWnd, (HMENU)ID_COMBO_DEVICES, g_hInst, nullptr);
    SetControlFont(g_hComboDevices);

    HWND hLabelVolume = CreateWindowW(L"STATIC", L"Volume (%):",
        WS_CHILD | WS_VISIBLE, 20, 52, 150, 20, hWnd, nullptr, g_hInst, nullptr);
    SetControlFont(hLabelVolume);

    g_hEditVolume = CreateWindowW(L"EDIT", L"50",
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_NUMBER,
        170, 48, 80, 24, hWnd, (HMENU)ID_EDIT_VOLUME, g_hInst, nullptr);
    SetControlFont(g_hEditVolume);

    g_hSpinVolume = CreateWindowW(UPDOWN_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS,
        0, 0, 0, 0, hWnd, (HMENU)ID_SPIN_VOLUME, g_hInst, nullptr);
    SetControlFont(g_hSpinVolume);
    SendMessageW(g_hSpinVolume, UDM_SETBUDDY, (WPARAM)g_hEditVolume, 0);
    SendMessageW(g_hSpinVolume, UDM_SETRANGE32, 0, 100);

    HWND hLabelInterval = CreateWindowW(L"STATIC", L"Interval (s):",
        WS_CHILD | WS_VISIBLE, 20, 84, 150, 20, hWnd, nullptr, g_hInst, nullptr);
    SetControlFont(hLabelInterval);

    g_hEditInterval = CreateWindowW(L"EDIT", L"5",
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_NUMBER,
        170, 80, 80, 24, hWnd, (HMENU)ID_EDIT_INTERVAL, g_hInst, nullptr);
    SetControlFont(g_hEditInterval);

    g_hSpinInterval = CreateWindowW(UPDOWN_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS,
        0, 0, 0, 0, hWnd, (HMENU)ID_SPIN_INTERVAL, g_hInst, nullptr);
    SetControlFont(g_hSpinInterval);
    SendMessageW(g_hSpinInterval, UDM_SETBUDDY, (WPARAM)g_hEditInterval, 0);
    SendMessageW(g_hSpinInterval, UDM_SETRANGE32, 1, 86400);

    g_hCheckExactMatch = CreateWindowW(L"BUTTON", L"Apply ONLY to exact name match",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        20, 116, 260, 22, hWnd, (HMENU)ID_CHECK_EXACT_MATCH, g_hInst, nullptr);
    SetControlFont(g_hCheckExactMatch);

    g_hButtonAddMic = CreateWindowW(L"BUTTON", L"Add microphone",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        20, 148, 120, 30, hWnd, (HMENU)ID_BUTTON_ADD_MIC, g_hInst, nullptr);
    SetControlFont(g_hButtonAddMic);
    EnableWindow(g_hButtonAddMic, FALSE);

    g_hButtonSaveMic = CreateWindowW(L"BUTTON", L"Save changes",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        150, 148, 110, 30, hWnd, (HMENU)ID_BUTTON_SAVE_MIC, g_hInst, nullptr);
    SetControlFont(g_hButtonSaveMic);

    g_hButtonRemoveMic = CreateWindowW(L"BUTTON", L"Remove selected",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        270, 148, 120, 30, hWnd, (HMENU)ID_BUTTON_REMOVE_MIC, g_hInst, nullptr);
    SetControlFont(g_hButtonRemoveMic);

    g_hButtonClearAll = CreateWindowW(L"BUTTON", L"Clear all",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        400, 148, 90, 30, hWnd, (HMENU)ID_BUTTON_CLEAR_ALL, g_hInst, nullptr);
    SetControlFont(g_hButtonClearAll);

    g_hButtonStart = CreateWindowW(L"BUTTON", L"Start",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        20, 188, 90, 30, hWnd, (HMENU)ID_BUTTON_START, g_hInst, nullptr);
    SetControlFont(g_hButtonStart);

    g_hButtonStop = CreateWindowW(L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        120, 188, 90, 30, hWnd, (HMENU)ID_BUTTON_STOP, g_hInst, nullptr);
    SetControlFont(g_hButtonStop);

    g_hButtonRefresh = CreateWindowW(L"BUTTON", L"Refresh devices",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        220, 188, 130, 30, hWnd, (HMENU)ID_BUTTON_REFRESH, g_hInst, nullptr);
    SetControlFont(g_hButtonRefresh);

    g_hCheckMinTray = CreateWindowW(L"BUTTON", L"Minimize to tray",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        20, 228, 180, 22, hWnd, (HMENU)ID_CHECK_MINTRAY, g_hInst, nullptr);
    SetControlFont(g_hCheckMinTray);

    g_hCheckCloseTray = CreateWindowW(L"BUTTON", L"Close to tray",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        20, 252, 180, 22, hWnd, (HMENU)ID_CHECK_CLOSETRAY, g_hInst, nullptr);
    SetControlFont(g_hCheckCloseTray);

    g_hEditingLabel = CreateWindowW(L"STATIC", L"Editing: New unsaved microphone",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 282, 500, 20, hWnd, (HMENU)ID_STATIC_EDITING, g_hInst, nullptr);
    SetControlFont(g_hEditingLabel);

    HWND hLabelSaved = CreateWindowW(L"STATIC", L"Saved microphones:",
        WS_CHILD | WS_VISIBLE,
        20, 310, 150, 20, hWnd, nullptr, g_hInst, nullptr);
    SetControlFont(hLabelSaved);

    g_hListSavedMics = CreateWindowW(WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL,
        20, 334, 710, 150, hWnd, (HMENU)ID_LIST_SAVED_MICS, g_hInst, nullptr);
    SetControlFont(g_hListSavedMics);

    ListView_SetExtendedListViewStyle(g_hListSavedMics, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    CreateListViewColumns(g_hListSavedMics);

    g_hStatus = CreateWindowW(L"STATIC", L"Ready.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 494, 710, 42, hWnd, (HMENU)ID_STATIC_STATUS, g_hInst, nullptr);
    SetControlFont(g_hStatus);
}

static void ResizeControls(HWND hWnd, int width, int height)
{
    int margin = 16;
    int left = margin;
    int right = width - margin;
    int top = 12;

    MoveWindow(g_hComboDevices, 170, 12, max(250, width - 190), 300, TRUE);
    MoveWindow(g_hListSavedMics, 20, 334, width - 40, max(120, height - 380), TRUE);
    MoveWindow(g_hStatus, 20, height - 42, width - 40, 24, TRUE);

    (void)hWnd;
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        CreateControls(hWnd);
        ApplyLoadedSettingsToControls();
        LoadSavedProfiles();
        RefreshSavedMicList();
        RefreshDeviceList();
        AddTrayIcon(hWnd);
        RestoreWindowPlacement();

        if (g_loadedStartRunning)
        {
            StartTimerApply();
        }
        else
        {
            SetEditingLabel(L"Editing: New unsaved microphone");
        }
        return 0;

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
        {
            SaveWindowPlacement();

            BOOL minimizeToTray = (SendMessageW(g_hCheckMinTray, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (minimizeToTray)
            {
                MinimizeToTray(hWnd);
                return 0;
            }
        }
        else
        {
            RECT rc = {};
            GetClientRect(hWnd, &rc);
            ResizeControls(hWnd, rc.right - rc.left, rc.bottom - rc.top);
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case ID_BUTTON_START:
            StartTimerApply();
            return 0;

        case ID_BUTTON_STOP:
            StopTimerApply();
            return 0;

        case ID_BUTTON_REFRESH:
            RefreshDeviceList();
            SaveSettings();
            return 0;

        case ID_BUTTON_ADD_MIC:
            AddOrUpdateCurrentMicFromControls();
            SaveSettings();
            return 0;

        case ID_BUTTON_SAVE_MIC:
            AddOrUpdateCurrentMicFromControls();
            SaveSettings();
            return 0;

        case ID_BUTTON_REMOVE_MIC:
            RemoveSelectedSavedMic();
            SaveSettings();
            return 0;

        case ID_BUTTON_CLEAR_ALL:
            ClearAllSavedMics();
            SaveSettings();
            return 0;

        case ID_TRAY_RESTORE:
            RestoreFromTray(hWnd);
            return 0;

        case ID_TRAY_EXIT:
            g_isExiting = true;
            SaveSettings();
            DestroyWindow(hWnd);
            return 0;

        case ID_CHECK_MINTRAY:
        case ID_CHECK_CLOSETRAY:
        case ID_CHECK_EXACT_MATCH:
            if (HIWORD(wParam) == BN_CLICKED)
            {
                SaveSettings();
            }
            return 0;

        case ID_COMBO_DEVICES:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                g_selectedSavedIndex = -1;
                ListView_SetItemState(g_hListSavedMics, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
                SetEditingLabel(L"Editing: New unsaved microphone");
                SaveSettings();
            }
            return 0;

        case ID_EDIT_VOLUME:
        case ID_EDIT_INTERVAL:
            if (HIWORD(wParam) == EN_CHANGE)
            {
                SaveSettings();
            }
            return 0;
        }
        break;

    case WM_NOTIFY:
        if (((LPNMHDR)lParam)->hwndFrom == g_hListSavedMics)
        {
            LPNMLISTVIEW nmlv = (LPNMLISTVIEW)lParam;
            if (nmlv->hdr.code == LVN_ITEMCHANGED)
            {
                if ((nmlv->uChanged & LVIF_STATE) && (nmlv->uNewState & LVIS_SELECTED))
                {
                    int sel = ListView_GetNextItem(g_hListSavedMics, -1, LVNI_SELECTED);
                    if (sel >= 0 && sel < (int)g_savedProfiles.size())
                    {
                        SelectSavedMicByIndex(sel);
                    }
                }
            }
        }
        break;

    case WM_TIMER:
        if (wParam == ID_TIMER_APPLY)
        {
            ApplyAllSavedProfiles();
            return 0;
        }
        break;

    case WM_MOVE:
    case WM_MOVING:
    case WM_EXITSIZEMOVE:
        SaveWindowPlacement();
        break;

    case WM_CLOSE:
        SaveSettings();

        if (!g_isExiting)
        {
            BOOL closeToTray = (SendMessageW(g_hCheckCloseTray, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (closeToTray)
            {
                MinimizeToTray(hWnd);
                return 0;
            }
        }

        DestroyWindow(hWnd);
        return 0;

    case WM_TRAYICON:
        switch ((UINT)lParam)
        {
        case WM_LBUTTONDBLCLK:
            RestoreFromTray(hWnd);
            return 0;

        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu(hWnd);
            return 0;
        }
        break;

    case WM_DESTROY:
        SaveSettings();
        KillTimer(hWnd, ID_TIMER_APPLY);
        g_timerRunning = false;
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)pCmdLine;

    g_hInst = hInstance;
    g_iniPath = GetIniPath();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
    {
        MessageBoxW(nullptr, L"COM initialization failed.", APP_TITLE, MB_ICONERROR);
        return 1;
    }

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_UPDOWN_CLASS | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = APP_CLASS_NAME;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadAppIcon(32, 32);
    wc.hIconSm = LoadAppIcon(16, 16);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassExW(&wc))
    {
        MessageBoxW(nullptr, L"Failed to register window class.", APP_TITLE, MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    g_hWnd = CreateWindowExW(
        0,
        APP_CLASS_NAME,
        APP_TITLE,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_SIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        760, 560,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!g_hWnd)
    {
        MessageBoxW(nullptr, L"Failed to create main window.", APP_TITLE, MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}