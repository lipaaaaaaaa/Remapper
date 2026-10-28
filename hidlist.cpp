// Lists every HID interface of VID 342D / PID E40F with usage page, usage and report sizes.
// Build: cl /EHsc hidlist.cpp hid.lib setupapi.lib
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <cstdio>
#include <vector>

int main() {
    GUID g;
    HidD_GetHidGuid(&g);
    HDEVINFO set = SetupDiGetClassDevsW(&g, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA d{sizeof d};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &g, i, &d); i++) {
        DWORD sz = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &d, nullptr, 0, &sz, nullptr);
        std::vector<char> buf(sz);
        auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        det->cbSize = sizeof *det;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &d, det, sz, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES a{sizeof a};
        PHIDP_PREPARSED_DATA pp;
        if (HidD_GetAttributes(h, &a) && a.VendorID == 0x342D && a.ProductID == 0xE40F &&
            HidD_GetPreparsedData(h, &pp)) {
            HIDP_CAPS c;
            HidP_GetCaps(pp, &c);
            wprintf(L"%s\n  UsagePage=%04X Usage=%04X In=%u Out=%u Feature=%u\n", det->DevicePath,
                    c.UsagePage, c.Usage, c.InputReportByteLength, c.OutputReportByteLength,
                    c.FeatureReportByteLength);
            HidD_FreePreparsedData(pp);
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
}
