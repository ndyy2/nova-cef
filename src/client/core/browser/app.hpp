#pragma once

#include <windows.h>
#include "include/cef_app.h"

class DefaultCefApp : public CefApp {
public:
    DefaultCefApp() = default;

    static bool IsRunningUnderWine() noexcept
    {
        static const bool is_wine = []() {
            HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
            return ntdll && ::GetProcAddress(ntdll, "wine_get_version") != nullptr;
        }();
        return is_wine;
    }

    void OnBeforeCommandLineProcessing(const CefString& /*process_type*/, CefRefPtr<CefCommandLine> command_line) override 
    {
        command_line->AppendSwitchWithValue("force-device-scale-factor", "1.0");
        command_line->AppendSwitchWithValue("high-dpi-support", "1");
        command_line->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
        command_line->AppendSwitchWithValue("allow-browser-signin", "false");
        command_line->AppendSwitch("enable-begin-frame-scheduling");

        // Prevent Chromium from throttling JS timers/events in off-screen windowless browsers.
        command_line->AppendSwitch("disable-background-timer-throttling");
        command_line->AppendSwitch("disable-backgrounding-occluded-windows");
        command_line->AppendSwitch("disable-renderer-backgrounding");
        command_line->AppendSwitch("disable-gpu-sandbox");
        command_line->AppendSwitchWithValue("disable-features", "CalculateNativeWinOcclusion,HardwareMediaKeyHandling");

        if (IsRunningUnderWine())
        {
            // Under Wine (32-bit), ANGLE/DirectComposition/GPU subprocesses frequently crash or
            // produce black frames. Software OSR compositing is stable and avoids GPU readback stalls.
            command_line->AppendSwitch("disable-gpu");
            command_line->AppendSwitch("disable-gpu-compositing");
            command_line->AppendSwitch("disable-direct-composition");
            command_line->AppendSwitch("no-sandbox");
        }
    }

private:
    IMPLEMENT_REFCOUNTING(DefaultCefApp);
};
