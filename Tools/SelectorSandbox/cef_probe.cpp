#include <windows.h>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"

#include <cstdio>
#include <string>

namespace {

class ProbeClient final : public CefClient, public CefLifeSpanHandler,
                          public CefRenderHandler, public CefLoadHandler,
                          public CefDisplayHandler {
public:
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override { browser_ = browser; }
    bool DoClose(CefRefPtr<CefBrowser>) override { return false; }
    void OnBeforeClose(CefRefPtr<CefBrowser>) override { browser_ = nullptr; closed_ = true; }
    bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                       const CefString&, const CefString&, WindowOpenDisposition,
                       bool, const CefPopupFeatures&, CefWindowInfo&, CefRefPtr<CefClient>&,
                       CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&,
                       bool*) override { return true; }

    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
        rect = CefRect(0, 0, 640, 600);
    }
    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&,
                 const void*, int, int) override { ++paint_count_; }
    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                   int) override {
        if (!frame->IsMain()) return;
        frame->ExecuteJavaScript(
            "setTimeout(()=>{document.title=document.querySelector('canvas.maplibregl-canvas')"
            "?'WEBGL_OK':'WEBGL_FAIL'},1500)", frame->GetURL(), 0);
    }
    void OnTitleChange(CefRefPtr<CefBrowser>, const CefString& title) override {
        const std::string value = title.ToString();
        if (value == "WEBGL_OK") webgl_ok_ = true;
        if (value == "WEBGL_FAIL") webgl_failed_ = true;
    }

    bool WebGlOk() const { return webgl_ok_; }
    bool WebGlFailed() const { return webgl_failed_; }
    int PaintCount() const { return paint_count_; }
    bool Closed() const { return closed_; }
    void Close() { if (browser_) browser_->GetHost()->CloseBrowser(true); }

private:
    CefRefPtr<CefBrowser> browser_;
    bool webgl_ok_ = false;
    bool webgl_failed_ = false;
    bool closed_ = false;
    int paint_count_ = 0;
    IMPLEMENT_REFCOUNTING(ProbeClient);
};

class ProbeApp final : public CefApp, public CefBrowserProcessHandler {
public:
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    void OnBeforeCommandLineProcessing(const CefString&, CefRefPtr<CefCommandLine> command) override {
        command->AppendSwitch("disable-background-networking");
        command->AppendSwitch("disable-component-update");
        command->AppendSwitch("disable-domain-reliability");
        command->AppendSwitch("disable-extensions");
        command->AppendSwitch("disable-sync");
    }
private:
    IMPLEMENT_REFCOUNTING(ProbeApp);
};

std::wstring CurrentDirectory() {
    wchar_t buffer[MAX_PATH]{};
    GetCurrentDirectoryW(MAX_PATH, buffer);
    return buffer;
}

int RunProbe(const std::wstring& repo, bool blank) {
    HMODULE module = GetModuleHandleW(nullptr);
    CefMainArgs args(module);
    CefRefPtr<ProbeApp> app(new ProbeApp);
    const int child_result = CefExecuteProcess(args, app, nullptr);
    if (child_result >= 0) return child_result;

    CefSettings settings;
    settings.no_sandbox = true;
    settings.windowless_rendering_enabled = true;
    HANDLE token = nullptr;
    DWORD is_container = 0;
    DWORD returned = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        GetTokenInformation(token, TokenIsAppContainer, &is_container,
                            sizeof(is_container), &returned);
        CloseHandle(token);
    }
    const std::wstring cache = repo + L"/test-results/p1/cef-probe-profile";
    if (!is_container) {
        CefString(&settings.root_cache_path) = cache;
        CefString(&settings.cache_path) = cache;
    }
    CefString(&settings.resources_dir_path) =
        L"C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/ThirdParty/CEF3/Win64/128.4.13+ge76af7e+chromium-128.0.6613.138+v2/Resources";
    CefString(&settings.locales_dir_path) =
        L"C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/ThirdParty/CEF3/Win64/128.4.13+ge76af7e+chromium-128.0.6613.138+v2/Resources/locales";
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    CefString(&settings.browser_subprocess_path) = executable;
    if (!CefInitialize(args, settings, app, nullptr)) return 50;

    std::wstring path = repo + L"/Content/P1Selector/index.html";
    for (wchar_t& c : path) if (c == L'\\') c = L'/';
    const std::wstring url = blank ? L"about:blank" :
        L"file:///" + path + L"?diagnostic=no-tiles";
    CefWindowInfo window;
    window.SetAsWindowless(nullptr);
    CefBrowserSettings browser_settings;
    CefRefPtr<ProbeClient> client(new ProbeClient);
    if (!CefBrowserHost::CreateBrowser(window, client, url, browser_settings, nullptr, nullptr)) {
        CefShutdown();
        return 51;
    }

    const ULONGLONG started = GetTickCount64();
    while (GetTickCount64() - started < 15000 && !client->WebGlOk() && !client->WebGlFailed()) {
        CefDoMessageLoopWork();
        Sleep(10);
    }
    const bool passed = client->WebGlOk() && client->PaintCount() > 0;
    std::printf("cef_webgl=%d cef_paints=%d cef_failed=%d\n",
        client->WebGlOk(), client->PaintCount(), client->WebGlFailed());
    client->Close();
    const ULONGLONG close_started = GetTickCount64();
    while (!client->Closed() && GetTickCount64() - close_started < 3000) {
        CefDoMessageLoopWork();
        Sleep(10);
    }
    CefShutdown();
    return passed && client->Closed() ? 0 : 52;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 41;
    return RunProbe(argv[1], argc >= 3 && wcscmp(argv[2], L"--blank") == 0);
}
