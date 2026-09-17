#include <windows.h>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <string>
#include <chrono>
#include <thread>
#include <iostream>
 
static volatile std::sig_atomic_t g_stopRequested = 0;
 
static BOOL WINAPI consoleHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_stopRequested = 1;
        return TRUE;
    }
    return FALSE;
}
 
static void enableDpiAwareness()
{
    using SetCtxFn = BOOL(WINAPI*)(HANDLE);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto setCtx = reinterpret_cast<SetCtxFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
        if (setCtx && setCtx(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4))))
            return;
    }
    SetProcessDPIAware();
}
 

struct ScreenCapture {
    HDC     screenDC = nullptr;
    HDC     memDC    = nullptr;
    HBITMAP dib      = nullptr;
    HGDIOBJ oldBmp   = nullptr;
    void*   pixels   = nullptr;   // BGRA, top-down, width*height*4 bytes
    int     width = 0, height = 0;
    int     originX = 0, originY = 0;
 
    bool init(bool allMonitors)
    {
        if (allMonitors) {
            originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            width   = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            height  = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        } else {
            originX = originY = 0;
            width   = GetSystemMetrics(SM_CXSCREEN);
            height  = GetSystemMetrics(SM_CYSCREEN);
        }
        if (width <= 0 || height <= 0) return false;
 
        screenDC = GetDC(nullptr);
        if (!screenDC) return false;
 
        memDC = CreateCompatibleDC(screenDC);
        if (!memDC) return false;
 
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = width;
        bmi.bmiHeader.biHeight      = -height;      // top-down
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
 
        dib = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!dib || !pixels) return false;
 
        oldBmp = SelectObject(memDC, dib);
        return true;
    }
 
    // CAPTUREBLT needed to include layered/transparent windows.
    void grab()
    {
        BitBlt(memDC, 0, 0, width, height,
               screenDC, originX, originY, SRCCOPY | CAPTUREBLT);
        drawCursor();
    }
 
    // BitBlt never includes the pointer it lives in separate hardware
    // layer — so composite it in manually.
    void drawCursor()
    {
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor)
            return;
 
        ICONINFO ii{};
        if (!GetIconInfo(ci.hCursor, &ii)) return;
 
        DrawIconEx(memDC,
                   ci.ptScreenPos.x - originX - static_cast<int>(ii.xHotspot),
                   ci.ptScreenPos.y - originY - static_cast<int>(ii.yHotspot),
                   ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
 
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    }
 
    size_t frameBytes() const { return static_cast<size_t>(width) * height * 4; }
 
    ~ScreenCapture()
    {
        if (memDC && oldBmp) SelectObject(memDC, oldBmp);
        if (dib)      DeleteObject(dib);
        if (memDC)    DeleteDC(memDC);
        if (screenDC) ReleaseDC(nullptr, screenDC);
    }
};

// ffmpeg pipe

static FILE* openEncoder(const std::string& outFile, int w, int h, int fps)
{
    std::string cmd =
        "ffmpeg -hide_banner -loglevel error -y"
        " -f rawvideo -pixel_format bgra"
        " -video_size " + std::to_string(w) + "x" + std::to_string(h) +
        " -framerate "  + std::to_string(fps) +
        " -i -"
        " -vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\""
        " -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p"
        " -movflags +faststart"
        " \"" + outFile + "\"";
 
    return _popen(cmd.c_str(), "wb");
}
 
int main(int argc, char** argv)
{
    // Hardcoded for now, per current requirements.
    constexpr int fps     = 30;
    constexpr int seconds = 60;
 
    enableDpiAwareness();
    SetConsoleCtrlHandler(consoleHandler, TRUE);
 
    const std::string outFile = (argc > 1) ? argv[1] : "out.mp4";
    const bool allMonitors    = (argc > 2) && std::strcmp(argv[2], "all") == 0;
 
    ScreenCapture cap;
    if (!cap.init(allMonitors)) {
        std::cerr << "Failed to initialise capture (GDI error "
                  << GetLastError() << ")\n";
        return 1;
    }
 
    FILE* enc = openEncoder(outFile, cap.width, cap.height, fps);
    if (!enc) {
        std::cerr << "Could not start ffmpeg. Is it on PATH?\n";
        return 1;
    }
 
    // Default timer resolution is ~15.6 ms, which makes a 30 fps schedule
    // impossible to hit. Raise it for the duration of the recording.
    timeBeginPeriod(1);
 
    std::cout << "Recording " << cap.width << "x" << cap.height
              << " @ " << fps << " fps -> " << outFile
              << " (auto-stop after " << seconds << "s)\n"
              << "Press Esc or Ctrl+C to stop early.\n";
 
    using clock = std::chrono::steady_clock;
    const auto framePeriod =
        std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / fps));
 
    const auto started  = clock::now();
    auto       deadline = started;
    long long  frames = 0, late = 0;
    bool       writeFailed = false;
 
    for (;;) {
        if (g_stopRequested) break;
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) break;
        if (clock::now() - started >= std::chrono::seconds(seconds)) break;
 
        cap.grab();
 
        if (fwrite(cap.pixels, 1, cap.frameBytes(), enc) != cap.frameBytes()) {
            writeFailed = true;   // ffmpeg died or the pipe closed
            break;
        }
        ++frames;
 
        deadline += framePeriod;
        const auto now = clock::now();
        if (deadline > now) {
            std::this_thread::sleep_until(deadline);
        } else {
            ++late;
            while (deadline < now) deadline += framePeriod;
        }
    }
 
    timeEndPeriod(1);
    _pclose(enc);
 
    const double elapsed =
        std::chrono::duration<double>(clock::now() - started).count();
 
    if (writeFailed) {
        std::cerr << "\nEncoder pipe closed early — output may be truncated.\n";
        return 1;
    }
 
    std::cout << "\nWrote " << frames << " frames in "
              << elapsed << "s (actual "
              << (elapsed > 0 ? frames / elapsed : 0.0) << " fps"
              << (late ? ", " + std::to_string(late) + " missed deadlines" : "")
              << ")\n";
    return 0;
}
