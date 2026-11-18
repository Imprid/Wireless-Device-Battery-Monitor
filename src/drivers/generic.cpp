// Fallbacks that work for any vendor:
//  * HID devices exposing a standard battery usage (Generic Device Controls "Battery Strength",
//    Battery System "Absolute State Of Charge") in a feature or input report.
//  * Bluetooth devices: Windows keeps the level it reads over HFP (classic) or the GATT Battery
//    Service (LE) in a device property, the same value Settings > Bluetooth & devices shows.
#include "drivers.h"

#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

// Live battery reads over GATT need bluetoothleapis.h (MinGW-w64 ships it). Without it the build still
// works, it just keeps using the level Windows has cached.
#if defined(__has_include)
#if __has_include(<bluetoothleapis.h>)
#include <bluetoothleapis.h>
#define HAVE_BLE_GATT 1
#endif
#endif

namespace {

bool IsBatteryUsage(USHORT page, USAGE usage) {
    return (page == 0x06 && usage == 0x20) || (page == 0x85 && usage == 0x65);
}

const DEVPROPKEY kBtBattery = { { 0x104EA319, 0x6EE2, 0x4701, { 0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5 } }, 2 };
const DEVPROPKEY kIsConnected = { { 0x83DA6326, 0x97A6, 0x4088, { 0x94, 0x53, 0xA1, 0x92, 0x3F, 0x57, 0x3B, 0x29 } }, 15 };
const DEVPROPKEY kClassOfDevice = { { 0x2BD67D8B, 0x8BEB, 0x48D5, { 0x87, 0xE0, 0x6C, 0xDA, 0x34, 0x28, 0x04, 0x0A } }, 10 };

DeviceKind KindFromClassOfDevice(UINT32 cod) {
    UINT32 major = (cod >> 8) & 0x1F, minor = (cod >> 2) & 0x3F;
    if (major == 0x04) return DeviceKind::Headset;
    if (major == 0x05) {
        if ((minor & 0x0F) == 0x01 || (minor & 0x0F) == 0x02) return DeviceKind::Gamepad;
        if ((minor >> 4) == 1) return DeviceKind::Keyboard;
        if ((minor >> 4) == 2) return DeviceKind::Mouse;
    }
    return DeviceKind::Other;
}

std::wstring StrProp(HDEVINFO di, SP_DEVINFO_DATA* dd, const DEVPROPKEY& key) {
    DEVPROPTYPE t;
    wchar_t buf[256] = {};
    if (SetupDiGetDevicePropertyW(di, dd, &key, &t, (PBYTE)buf, sizeof(buf) - sizeof(wchar_t), nullptr, 0) && t == DEVPROP_TYPE_STRING)
        return buf;
    return L"";
}

// Container properties like the category are string lists; only the first entry matters here.
std::wstring FirstStringProp(HDEVINFO di, SP_DEVINFO_DATA* dd, const DEVPROPKEY& key) {
    DEVPROPTYPE t;
    wchar_t buf[256] = {};
    if (SetupDiGetDevicePropertyW(di, dd, &key, &t, (PBYTE)buf, sizeof(buf) - sizeof(wchar_t), nullptr, 0) &&
        (t == DEVPROP_TYPE_STRING_LIST || t == DEVPROP_TYPE_STRING))
        return buf;
    return L"";
}

// Classic Bluetooth devices carry a Class of Device, but LE devices (BTHLE\DEV_...) usually do not,
// so KindFromClassOfDevice has nothing to work with and a LE keyboard or mouse ends up as "Other".
// Windows still files the device under a category for Settings > Bluetooth & devices; use it.
DeviceKind KindFromCategory(const std::wstring& c) {
    if (c == L"Input.Keyboard") return DeviceKind::Keyboard;
    if (c == L"Input.Mouse") return DeviceKind::Mouse;
    if (c == L"Input.Gaming") return DeviceKind::Gamepad;
    if (c.rfind(L"Communication.Headset", 0) == 0 || c == L"Audio.Headphones") return DeviceKind::Headset;
    return DeviceKind::Other;
}

// Bluetooth LE keyboards that also expose a mouse collection (for the knob or macros), so
// KindFromCollections would call them a mouse. Matched on the exact HID vid/pid seen on the LE link.
//  * 3151:4013 Attack Shark K86 (Rongyuan firmware) over Bluetooth, from the reporter's --dump in
//    krisgfx9/Wireless-Device-Battery-Monitor#168. Its USB ids are 3151:4011 / 4015 (OpenRGB issue 4074).
DeviceKind KindFromBleIds(USHORT vid, USHORT pid) {
    if (vid == 0x3151 && pid == 0x4013) return DeviceKind::Keyboard;
    return DeviceKind::Other;
}

std::wstring StripServiceSuffix(std::wstring n) {
    for (const wchar_t* suf : { L" Hands-Free AG", L" Hands-Free", L" Avrcp Transport", L" Stereo", L" Audio Gateway" }) {
        size_t l = wcslen(suf);
        if (n.size() > l && n.compare(n.size() - l, l, suf) == 0) return n.substr(0, n.size() - l);
    }
    return n;
}

// Container id ("{...}") of the physical device an HID interface belongs to, formatted like
// PollBluetooth's keys, or "" if unknown. Uses SetupAPI: CM_Get_Device_Interface_PropertyW is
// missing from older MinGW import libraries.
std::wstring ContainerOf(const std::wstring& path) {
    HDEVINFO di = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (di == INVALID_HANDLE_VALUE) return L"";
    std::wstring result;
    SP_DEVICE_INTERFACE_DATA ifd{ sizeof(ifd) };
    SP_DEVINFO_DATA dd{ sizeof(dd) };
    GUID g{};
    DEVPROPTYPE t;
    if (SetupDiOpenDeviceInterfaceW(di, path.c_str(), 0, &ifd) &&
        SetupDiGetDeviceInterfaceDetailW(di, &ifd, nullptr, 0, nullptr, &dd) == FALSE && dd.DevInst &&
        SetupDiGetDevicePropertyW(di, &dd, &DEVPKEY_Device_ContainerId, &t, (PBYTE)&g, sizeof(g), nullptr, 0) &&
        t == DEVPROP_TYPE_GUID) {
        wchar_t buf[64];
        StringFromGUID2(g, buf, 64);
        result = buf;
    }
    SetupDiDestroyDeviceInfoList(di);
    return result;
}
std::wstring HidContainer(const std::wstring& path) { return ContainerOf(path); }

// What PollBluetooth decided for each device with a battery level, and why; printed by --dump
// (DiagBleBattery below) so a missing or stale level can be traced without guessing.
std::wstring g_btDiag;
void BtDiag(const std::wstring& line) { g_btDiag += L"  " + line + L"\n"; }
const wchar_t* YesNo(bool b) { return b ? L"yes" : L"no"; }
std::wstring ShortId(const std::wstring& cid) {
    size_t b = (!cid.empty() && cid[0] == L'{') ? 1 : 0;
    return cid.substr(b, 8);
}

#ifdef HAVE_BLE_GATT
// ---- live Battery Level over GATT ------------------------------------------------------------------
// For a Bluetooth LE device Windows keeps the Battery Level it last read in a device property. Dual-mode
// headphones (a classic audio link plus an LE link that nobody keeps open) can leave that property hours
// out of date: the Audio-Technica ATH-M50xBT2 showed 50% in Settings while the headphones were at 65%.
// Where the device has the standard Battery Service (0x180F), ask it directly: reading the Battery Level
// characteristic (0x2A19) with FORCE_READ_FROM_DEVICE makes Windows connect and fetch the live value.
// Read-only, like every driver here.

// GUID_BLUETOOTH_GATT_SERVICE_DEVICE_INTERFACE (the interface of a BTHLEDEVICE\{service uuid} node) and
// GUID_BLUETOOTHLE_DEVICE_INTERFACE (the interface of a BTHLE\DEV_ node).
const GUID kGattServiceInterface = { 0x6E3BB679, 0x4372, 0x40C8, { 0x9E, 0xAA, 0x45, 0x09, 0xDF, 0x26, 0x0C, 0xD8 } };
const GUID kGattDeviceInterface = { 0x781AEE18, 0x7733, 0x4CE4, { 0xAD, 0xD0, 0x91, 0xF4, 0x1C, 0x67, 0xB5, 0x92 } };
const ULONG kForceReadFromDevice = 0x00000004;  // BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE

typedef HRESULT(WINAPI* GetServicesFn)(HANDLE, USHORT, PBTH_LE_GATT_SERVICE, USHORT*, ULONG);
typedef HRESULT(WINAPI* GetCharacteristicsFn)(HANDLE, PBTH_LE_GATT_SERVICE, USHORT, PBTH_LE_GATT_CHARACTERISTIC, USHORT*, ULONG);
typedef HRESULT(WINAPI* GetCharacteristicValueFn)(HANDLE, PBTH_LE_GATT_CHARACTERISTIC, ULONG, PBTH_LE_GATT_CHARACTERISTIC_VALUE, USHORT*, ULONG);

// Device interface path of the node `inst` for interface `guid`. Asks the configuration manager first; if
// that has nothing it builds the path, which is always "\\?\" + the instance id with \ turned into # + #
// + the interface guid. `how` says which of the two it was.
std::wstring InterfacePath(const wchar_t* inst, const GUID& guid, const wchar_t** how) {
    GUID g = guid;
    ULONG len = 0;
    if (CM_Get_Device_Interface_List_SizeW(&len, &g, const_cast<wchar_t*>(inst), CM_GET_DEVICE_INTERFACE_LIST_PRESENT) == CR_SUCCESS &&
        len > 1) {
        std::vector<wchar_t> buf(len + 1, 0);
        if (CM_Get_Device_Interface_ListW(&g, const_cast<wchar_t*>(inst), buf.data(), len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) ==
                CR_SUCCESS &&
            buf[0]) {
            *how = L"from the configuration manager";
            return buf.data();
        }
    }
    std::wstring p = L"\\\\?\\";
    for (const wchar_t* c = inst; *c; ++c) p += (*c == L'\\') ? L'#' : *c;
    wchar_t gs[64] = {};
    StringFromGUID2(g, gs, 64);
    p += L"#";
    p += gs;
    *how = L"built from the instance id";
    return p;
}

std::wstring Hex(HRESULT hr) {
    wchar_t b[16];
    swprintf(b, 16, L"0x%08lX", (unsigned long)hr);
    return b;
}

// GATT handles want read+write access; plain read is only a fallback. *err keeps the first failure.
HANDLE OpenGatt(const std::wstring& path, DWORD* err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    return h;
}

// Reads Battery Level (0x2A19) through an open handle. `svc` is the Battery Service when `h` is a device
// handle, and null when `h` is the service node's own handle. Returns 0..100, or -1 with the reason in `why`.
int ReadLevelFrom(HANDLE h, PBTH_LE_GATT_SERVICE svc, GetCharacteristicsFn getChars, GetCharacteristicValueFn getValue, std::wstring& why) {
    USHORT count = 0;
    getChars(h, svc, 0, nullptr, &count, 0);  // first call only reports how many there are
    if (count == 0 || count > 32) {
        why += L"no characteristics (" + std::to_wstring((unsigned)count) + L")";
        return -1;
    }
    std::vector<BTH_LE_GATT_CHARACTERISTIC> chars(count);
    USHORT got = 0;
    HRESULT hr = getChars(h, svc, count, chars.data(), &got, 0);
    if (FAILED(hr)) {
        why += L"characteristics " + Hex(hr);
        return -1;
    }
    for (USHORT i = 0; i < got; i++) {
        BTH_LE_GATT_CHARACTERISTIC& c = chars[i];
        if (!c.CharacteristicUuid.IsShortUuid || c.CharacteristicUuid.Value.ShortUuid != 0x2A19) continue;
        alignas(8) BYTE buf[64] = {};
        auto val = reinterpret_cast<PBTH_LE_GATT_CHARACTERISTIC_VALUE>(buf);
        USHORT need = 0;
        hr = getValue(h, &c, sizeof(buf), val, &need, kForceReadFromDevice);
        if (FAILED(hr)) {
            why += L"read " + Hex(hr);
            return -1;
        }
        if (val->DataSize < 1 || val->Data[0] > 100) {
            why += L"odd value";
            return -1;
        }
        return val->Data[0];
    }
    why += L"no battery level characteristic";
    return -1;
}

struct GattJob {
    std::wstring servicePath, devicePath;
    int level = -1;     // 0..100 once read
    std::wstring why;   // what went wrong, for --dump
    std::wstring route; // which of the two routes below worked
};
volatile LONG g_gattBusy = 0;  // 1 while a read thread is alive (one that times out can outlive its caller)
std::map<std::wstring, int> g_lastLive;  // container id -> last level read live, kept for when a read fails

// Two routes to the same characteristic: the Battery Service node's own handle, then the LE device's handle
// (which lists its services itself; this is the route Microsoft's GATT samples take).
int ReadGattBatteryLevel(GattJob& job, std::wstring& why) {
    HMODULE lib = LoadLibraryW(L"BluetoothApis.dll");
    if (!lib) { why = L"BluetoothApis.dll missing"; return -1; }
    auto getServices = (GetServicesFn)(void*)GetProcAddress(lib, "BluetoothGATTGetServices");
    auto getChars = (GetCharacteristicsFn)(void*)GetProcAddress(lib, "BluetoothGATTGetCharacteristics");
    auto getValue = (GetCharacteristicValueFn)(void*)GetProcAddress(lib, "BluetoothGATTGetCharacteristicValue");
    if (!getServices || !getChars || !getValue) { why = L"GATT API missing"; return -1; }

    if (!job.servicePath.empty()) {
        DWORD err = 0;
        HANDLE h = OpenGatt(job.servicePath, &err);
        if (h == INVALID_HANDLE_VALUE) {
            why += L"service node: open failed (" + std::to_wstring((unsigned long)err) + L"); ";
        } else {
            std::wstring sub;
            int level = ReadLevelFrom(h, nullptr, getChars, getValue, sub);
            CloseHandle(h);
            if (level >= 0) {
                job.route = L"the service node";
                return level;
            }
            why += L"service node: " + sub + L"; ";
        }
    }
    if (!job.devicePath.empty()) {
        DWORD err = 0;
        HANDLE h = OpenGatt(job.devicePath, &err);
        if (h == INVALID_HANDLE_VALUE) {
            why += L"device: open failed (" + std::to_wstring((unsigned long)err) + L")";
            return -1;
        }
        int level = -1;
        USHORT n = 0;
        getServices(h, 0, nullptr, &n, 0);
        if (n == 0 || n > 64) {
            why += L"device: no services (" + std::to_wstring((unsigned)n) + L")";
        } else {
            std::vector<BTH_LE_GATT_SERVICE> svcs(n);
            USHORT got = 0;
            HRESULT hr = getServices(h, n, svcs.data(), &got, 0);
            if (FAILED(hr)) {
                why += L"device: services " + Hex(hr);
            } else {
                bool found = false;
                for (USHORT i = 0; i < got && !found; i++) {
                    if (!svcs[i].ServiceUuid.IsShortUuid || svcs[i].ServiceUuid.Value.ShortUuid != 0x180F) continue;
                    found = true;
                    std::wstring sub;
                    level = ReadLevelFrom(h, &svcs[i], getChars, getValue, sub);
                    if (level < 0) why += L"device: " + sub;
                }
                if (!found) why += L"device: no battery service among " + std::to_wstring((unsigned)got);
            }
        }
        CloseHandle(h);
        if (level >= 0) job.route = L"the device handle";
        return level;
    }
    return -1;
}

DWORD WINAPI GattBatteryThread(LPVOID param) {
    auto job = static_cast<GattJob*>(param);
    job->level = ReadGattBatteryLevel(*job, job->why);
    InterlockedExchange(&g_gattBusy, 0);
    return 0;
}

// Runs the read on its own thread so an unreachable device cannot hold up the whole poll: after the timeout
// the pending call is cancelled. A read normally takes about a second. Returns -1 on failure and puts the
// reason in `why`; on success `route` says which route worked.
int LiveGattBattery(const std::wstring& servicePath, const std::wstring& devicePath, std::wstring& why, std::wstring& route) {
    if (InterlockedCompareExchange(&g_gattBusy, 1, 0) != 0) { why = L"previous read still pending"; return -1; }
    auto job = new GattJob{ servicePath, devicePath };
    HANDLE th = CreateThread(nullptr, 0, GattBatteryThread, job, 0, nullptr);
    if (!th) { InterlockedExchange(&g_gattBusy, 0); delete job; why = L"could not start thread"; return -1; }
    if (WaitForSingleObject(th, 4000) != WAIT_OBJECT_0) {
        CancelSynchronousIo(th);
        if (WaitForSingleObject(th, 500) != WAIT_OBJECT_0) {
            CloseHandle(th);  // the thread still owns `job`; it is left alone (tiny, rare leak)
            why = L"timed out";
            return -1;
        }
    }
    CloseHandle(th);
    int level = job->level;
    why = job->why;
    route = job->route;
    delete job;
    return level;
}
#endif  // HAVE_BLE_GATT

}  // namespace

void PollGenericHid(PollContext& ctx) {
    // Drawing tablets and pen displays (any collection on the Digitizers page 0D) are never read:
    // polling one for a battery report flipped the Y axis of an XP-Pen Deco Fun S (issue #232).
    std::vector<std::pair<USHORT, USHORT>> digitizers;
    for (const auto& c : ctx.colls)
        if (c.page() == 0x0D) digitizers.push_back({ c.vid, c.pid });
    for (const auto& c : ctx.colls) {
        if (ctx.IsClaimed(c.vid, c.pid)) continue;
        if (c.page() == 0x84 || c.page() == 0x85) continue;  // UPS / power supplies
        if (std::find(digitizers.begin(), digitizers.end(), std::make_pair(c.vid, c.pid)) != digitizers.end()) continue;
        if (!c.caps.NumberFeatureValueCaps && !c.caps.NumberInputValueCaps) continue;
        // Same physical device already shown with Windows' Bluetooth level (its HID field can read 0).
        if (!ctx.btContainers.empty() && ctx.btContainers.count(ContainerOf(c.path))) continue;
        HANDLE h = CreateFileW(c.path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        PHIDP_PREPARSED_DATA pp;
        if (!HidD_GetPreparsedData(h, &pp)) { CloseHandle(h); continue; }

        int pct = -1;
        for (HIDP_REPORT_TYPE type : { HidP_Feature, HidP_Input }) {
            USHORT n = type == HidP_Feature ? c.caps.NumberFeatureValueCaps : c.caps.NumberInputValueCaps;
            USHORT len = type == HidP_Feature ? c.featLen() : c.inLen();
            if (!n || !len) continue;
            std::vector<HIDP_VALUE_CAPS> vc(n);
            if (HidP_GetValueCaps(type, vc.data(), &n, pp) != HIDP_STATUS_SUCCESS) continue;
            for (USHORT i = 0; i < n && pct < 0; i++) {
                const auto& v = vc[i];
                USAGE u = v.IsRange ? v.Range.UsageMin : v.NotRange.Usage;
                if (!IsBatteryUsage(v.UsagePage, u)) continue;
                std::vector<BYTE> buf(len, 0);
                buf[0] = v.ReportID;
                BOOL got = type == HidP_Feature ? HidD_GetFeature(h, buf.data(), len) : HidD_GetInputReport(h, buf.data(), len);
                if (!got) continue;
                ULONG val = 0;
                if (HidP_GetUsageValue(type, v.UsagePage, v.LinkCollection, u, &val, pp, (PCHAR)buf.data(), len) != HIDP_STATUS_SUCCESS)
                    continue;
                // Apple devices (Magic Trackpad / Mouse / Keyboard) report a plain percentage while
                // declaring a 0-255 range; Linux handles them with HID_BATTERY_QUIRK_PERCENT.
                bool percentQuirk = c.vid == 0x05AC && val <= 100;
                if (percentQuirk) pct = (int)val;
                else if (v.LogicalMax > v.LogicalMin) pct = Clamp100((int)(((LONG)val - v.LogicalMin) * 100 / (v.LogicalMax - v.LogicalMin)));
                else if (val <= 100) pct = (int)val;
            }
            if (pct >= 0) break;
        }
        HidD_FreePreparsedData(pp);
        CloseHandle(h);
        if (pct < 0) continue;
        DeviceStatus s;
        s.name = HidProduct(c);
        if (s.name.empty()) s.name = L"Wireless device";
        s.key = L"hid:" + Hex4(c.vid) + L":" + Hex4(c.pid);
        s.via = L"HID";
        s.kind = KindFromCollections(ctx.colls, c.vid, c.pid);
        if (s.kind == DeviceKind::Other) s.kind = GuessKind(s.name);
        s.percent = pct;
        ctx.Add(s, c.vid, c.pid);
    }
}

void PollBluetooth(PollContext& ctx) {
    g_btDiag.clear();
    HDEVINFO di = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return;

    struct Found { int pct; std::wstring name; bool connected; };
    std::map<std::wstring, std::wstring> containerName;  // container id -> name of the main BT device node
    std::map<std::wstring, UINT32> containerCod;         // container id -> class of device
    std::map<std::wstring, std::wstring> containerCat;   // container id -> device container category
    std::map<std::wstring, bool> containerConn;          // container id -> main BT device node connected
    std::map<std::wstring, Found> battery;
    std::set<std::wstring> classicConn;                  // containers whose classic (BTHENUM) node is connected
    std::map<std::wstring, bool> batteryFromLe;          // container id -> the level above sits on an LE node
    std::map<std::wstring, std::wstring> gattBatteryPath;  // container id -> interface path of its 0x180F node
    std::map<std::wstring, std::wstring> gattDevicePath;   // container id -> interface path of its BTHLE\DEV_ node
    SP_DEVINFO_DATA dd{ sizeof(dd) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); i++) {
        wchar_t inst[512];
        if (!SetupDiGetDeviceInstanceIdW(di, &dd, inst, 512, nullptr)) continue;
        if (wcsncmp(inst, L"BTHENUM\\", 8) && wcsncmp(inst, L"BTHLE\\", 6) && wcsncmp(inst, L"BTHLEDEVICE\\", 12)) continue;

        GUID container{};
        DEVPROPTYPE t;
        std::wstring cid = inst;
        if (SetupDiGetDevicePropertyW(di, &dd, &DEVPKEY_Device_ContainerId, &t, (PBYTE)&container, sizeof(container), nullptr, 0)) {
            wchar_t g[64];
            StringFromGUID2(container, g, 64);
            cid = g;
        }
        std::wstring name = StrProp(di, &dd, DEVPKEY_Device_FriendlyName);
        if (name.empty()) name = StrProp(di, &dd, DEVPKEY_Device_DeviceDesc);
        if (!wcsncmp(inst, L"BTHENUM\\DEV_", 12) || !wcsncmp(inst, L"BTHLE\\DEV_", 10)) {
            containerName[cid] = name;
            UINT32 cod = 0;
            if (SetupDiGetDevicePropertyW(di, &dd, &kClassOfDevice, &t, (PBYTE)&cod, sizeof(cod), nullptr, 0) && t == DEVPROP_TYPE_UINT32)
                containerCod[cid] = cod;
            std::wstring cat = FirstStringProp(di, &dd, DEVPKEY_DeviceContainer_Category);
            if (!cat.empty()) containerCat[cid] = cat;
            DEVPROP_BOOLEAN conn;
            if (SetupDiGetDevicePropertyW(di, &dd, &kIsConnected, &t, (PBYTE)&conn, sizeof(conn), nullptr, 0) &&
                t == DEVPROP_TYPE_BOOLEAN) {
                // A dual-mode device (most Bluetooth headphones) has a classic BTHENUM node and an LE BTHLE
                // node in the same container, and the LE one can read "disconnected" while audio plays.
                // The device is connected when either node is, whichever one is enumerated last.
                bool& c = containerConn[cid];
                c = c || conn != DEVPROP_FALSE;
                if (!wcsncmp(inst, L"BTHENUM\\DEV_", 12) && conn != DEVPROP_FALSE) classicConn.insert(cid);
            }
        }
#ifdef HAVE_BLE_GATT
        {
            static const wchar_t kBatteryService[] = L"BTHLEDEVICE\\{0000180F-";
            const wchar_t* how = L"";
            if (!_wcsnicmp(inst, kBatteryService, wcslen(kBatteryService))) {
                gattBatteryPath[cid] = InterfacePath(inst, kGattServiceInterface, &how);
                BtDiag(L"container " + ShortId(cid) + L": Battery Service node found, its interface path was " + how);
            } else if (!wcsncmp(inst, L"BTHLE\\DEV_", 10)) {
                gattDevicePath[cid] = InterfacePath(inst, kGattDeviceInterface, &how);
            }
        }
#endif

        BYTE pct = 0;
        if (!SetupDiGetDevicePropertyW(di, &dd, &kBtBattery, &t, &pct, sizeof(pct), nullptr, 0) || t != DEVPROP_TYPE_BYTE) continue;
        // The service node's own connected flag is only a fallback: headphones keep their level on the
        // Hands-Free node, which reads as disconnected while audio runs over A2DP. The main device node
        // decides below when there is one.
        DEVPROP_BOOLEAN conn = DEVPROP_TRUE;
        if (!SetupDiGetDevicePropertyW(di, &dd, &kIsConnected, &t, (PBYTE)&conn, sizeof(conn), nullptr, 0) ||
            t != DEVPROP_TYPE_BOOLEAN)
            conn = DEVPROP_TRUE;
        ULONG st = 0, prob = 0;
        if (CM_Get_DevNode_Status(&st, &prob, dd.DevInst, 0) == CR_SUCCESS && (st & DN_HAS_PROBLEM)) continue;
        if (pct > 100) continue;
        auto prev = battery.find(cid);
        if (prev != battery.end() && prev->second.connected && conn == DEVPROP_FALSE) continue;  // prefer a live node
        battery[cid] = { pct, StripServiceSuffix(name), conn != DEVPROP_FALSE };
        batteryFromLe[cid] = !wcsncmp(inst, L"BTHLE", 5);  // BTHLE\\ and BTHLEDEVICE\\ nodes
    }
    SetupDiDestroyDeviceInfoList(di);

    // Physical devices a driver above already reported over a HID interface on the Bluetooth link
    // itself (e.g. Logitech HID++ names the LIFT "Logitech LIFT VERTICAL ERGONOMIC MOUSE" while
    // Bluetooth calls it "LIFT", so the name check below misses it).
    std::set<std::wstring> reported;
    std::map<std::wstring, std::pair<USHORT, USHORT>> hidIds;  // container id -> HID vid/pid on that device
    for (const auto& c : ctx.colls) {
        std::wstring cont = HidContainer(c.path);
        hidIds[cont] = { c.vid, c.pid };
        if (ctx.IsClaimed(c.vid, c.pid)) reported.insert(cont);
    }

    if (battery.empty()) BtDiag(L"no Bluetooth node carries a battery level");
    for (auto& [cid, f] : battery) {
        const std::wstring tag = L"container " + ShortId(cid) + L" '" + f.name + L"' (Windows level " + std::to_wstring(f.pct) + L"%)";
        if (reported.count(cid)) { BtDiag(tag + L": skipped, a vendor driver already reported it"); continue; }
        auto mc = containerConn.find(cid);
        if (mc != containerConn.end() ? !mc->second : !f.connected) { BtDiag(tag + L": skipped, not connected"); continue; }
        DeviceStatus s;
        s.key = L"bt:" + cid;
        auto it = containerName.find(cid);
        s.name = (it != containerName.end() && !it->second.empty()) ? it->second : f.name;
        if (s.name.empty()) s.name = L"Bluetooth device";
        // Dual-mode headphones advertise over LE as "LE_<name>" (Sony, Audio-Technica ...); show the real name.
        if (s.name.size() > 3 && s.name.compare(0, 3, L"LE_") == 0) s.name.erase(0, 3);
        // Skip devices a vendor driver already reported over HID++ / Bragi on the same link.
        bool dup = false;
        for (auto& o : ctx.out) dup |= o.name == s.name;
        if (dup) { BtDiag(tag + L": skipped, a device with the same name is already listed"); continue; }
        s.via = L"Bluetooth";
        s.percent = f.pct;
#ifdef HAVE_BLE_GATT
        // Dual-mode headphones: classic audio link up, level taken from an LE node Windows may not have
        // refreshed for hours. Ask the Battery Service itself (see LiveGattBattery).
        {
            auto gp = gattBatteryPath.find(cid);
            auto dp = gattDevicePath.find(cid);
            auto le = batteryFromLe.find(cid);
            const bool leLevel = le != batteryFromLe.end() && le->second;
            const bool classic = classicConn.count(cid) != 0;
            const bool haveNode = gp != gattBatteryPath.end();
            BtDiag(tag + L": level from an LE node " + YesNo(leLevel) + L", classic node connected " + YesNo(classic) +
                   L", Battery Service node " + (haveNode ? L"found" : L"missing"));
            if (haveNode && leLevel && classic) {
                std::wstring why, route;
                const ULONGLONG t0 = GetTickCount64();
                int live = LiveGattBattery(gp->second, dp != gattDevicePath.end() ? dp->second : std::wstring(), why, route);
                const std::wstring took = std::to_wstring((unsigned long long)(GetTickCount64() - t0)) + L" ms";
                if (live >= 0) {
                    s.percent = live;
                    g_lastLive[cid] = live;
                    BtDiag(L"live read OK: " + std::to_wstring(live) + L"% via " + route + L" after " + took);
                } else {
                    // Keep the last live reading rather than falling back to Windows' older cached level.
                    auto last = g_lastLive.find(cid);
                    if (last != g_lastLive.end()) s.percent = last->second;
                    s.detail = last != g_lastLive.end() ? L"last known level" : L"level cached by Windows";
                    BtDiag(L"live read FAILED: " + why + L" after " + took);
                }
            } else {
                BtDiag(L"live read not attempted");
            }
        }
#else
        BtDiag(tag + L": this build has no GATT support (bluetoothleapis.h missing), showing Windows' level");
#endif
        auto cod = containerCod.find(cid);
        s.kind = cod != containerCod.end() ? KindFromClassOfDevice(cod->second) : DeviceKind::Other;
        if (s.kind == DeviceKind::Other) {
            auto cat = containerCat.find(cid);
            if (cat != containerCat.end()) s.kind = KindFromCategory(cat->second);
        }
        // Bluetooth LE devices have no class of device; their HID collections tell a mouse from a keyboard.
        auto ids = hidIds.find(cid);
        if (s.kind == DeviceKind::Other && ids != hidIds.end()) s.kind = KindFromBleIds(ids->second.first, ids->second.second);
        if (s.kind == DeviceKind::Other && ids != hidIds.end()) s.kind = KindFromCollections(ctx.colls, ids->second.first, ids->second.second);
        if (s.kind == DeviceKind::Other) s.kind = GuessKind(s.name);
        ctx.out.push_back(s);
        ctx.btContainers.insert(cid);
    }
}

// For --dump: runs the Bluetooth fallback once and returns what it decided and why.
std::wstring DiagBleBattery() {
    PollContext ctx;
    ctx.colls = EnumHid();
    PollBluetooth(ctx);
    std::wstring out = L"\nBluetooth fallback (PollBluetooth) decisions:\n" + g_btDiag;
    for (const auto& d : ctx.out)
        out += L"  -> listed: " + d.name + L" " + std::to_wstring(d.percent) + L"%" + (d.detail.empty() ? L"" : L" (" + d.detail + L")") + L"\n";
    if (ctx.out.empty()) out += L"  -> nothing listed\n";
    return out;
}
