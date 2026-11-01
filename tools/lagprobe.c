/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * lagprobe.exe - an input-to-photon test for the camera (D3D11, through Proton and DXVK, as games
 * are). The whole screen is black; while a button is held it is white. Filmed at 240 fps with the
 * controller in the shot, the frames from the button bottoming out to the first white frame are the
 * latency of everything from the controller to the panel, with a known sample point: input is read
 * once per frame, at the frame's start, as a game does.
 *
 *   lagprobe [--cap FPS] [--gpu N] [--gpu-size PX] [--latency N] [--buffers N] [--sync 0|1] [--windowed]
 *            [--frames N] [--log PATH]
 *
 *   --cap FPS      hold the frame rate to FPS by sleeping before the sample (a CPU-bound game at that
 *                  rate); 0 (the default): as fast as the swapchain lets it
 *   --gpu N        N full copies of a PX x PX RGBA texture each frame (an artificial GPU load, to get
 *                  a GPU-bound 30, 40 or 60 fps; raise N until the rate drops); default 0
 *   --gpu-size PX  default 2048 (16 MB a copy)
 *   --latency N    IDXGIDevice1::SetMaximumFrameLatency(N) (D3D11's default is 3; the plan's fix
 *                  candidate is 1), default: left alone
 *   --buffers N    the swapchain's BufferCount, 2-8 (default 2; DXVK keeps at most this many frames
 *                  queued, so 3 shows a game's default of three)
 *   --sync 0|1     Present's sync interval, default 1
 *   --frames N     quit after N frames (default: run until Esc, or Back+Start held for 2 s)
 *   --log PATH     default lagprobe.log in the current directory
 *
 * A press is any pad button or trigger (XInput, pad 0-3), the space bar or the left mouse button
 * (the touch screen). The log, for tools/latency_report.py, which lines it up with the bridge's
 * vkbridge-latency.log and DroidDeck's droiddeck-latency.log:
 *   # lagprobe 1 qpc_hz=<f> <w>x<h> cap=<fps> gpu=<n>x<px> latency=<n> sync=<n>
 *   F <frame> <t_present>             each frame: just before its Present() (ns of the QPC clock)
 *   P <frame> <t_sample> <down> <src> the input changed at this frame's sample (down 1: this frame is
 *                                     the first white one); src: 1 pad, 2 space, 4 mouse
 * The QPC clock is Wine's (CLOCK_MONOTONIC_RAW, or the TSC): the report finds its offset to the
 * bridge's CLOCK_MONOTONIC from the frames themselves.
 *
 * Built with mingw-w64 (tools/build_lagprobe.sh):
 *   x86_64-w64-mingw32-gcc -O2 lagprobe.c -o lagprobe.exe -ld3d11 -ldxgi -mwindows
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} pad_t;
typedef struct {
    DWORD dwPacketNumber;
    pad_t Gamepad;
} pad_state_t;
typedef DWORD(WINAPI *xinput_get_t)(DWORD, pad_state_t *);

static xinput_get_t xinput_get;
static LARGE_INTEGER qpf;
static int quit;

static uint64_t qpc_ns(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)qpf.QuadPart);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE || m == WM_DESTROY) {
        quit = 1;
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) {
        quit = 1;
        return 0;
    }
    if (m == WM_SETCURSOR && LOWORD(l) == HTCLIENT) {
        SetCursor(NULL);
        return TRUE;
    }
    return DefWindowProcA(h, m, w, l);
}

static void load_xinput(void)
{
    static const char *const dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (int i = 0; i < 3 && !xinput_get; i++) {
        HMODULE x = LoadLibraryA(dlls[i]);
        if (x) xinput_get = (xinput_get_t)(void *)GetProcAddress(x, "XInputGetState");
    }
}

/* the input now: 1 pad, 2 space, 4 left mouse button; *exit_combo: Back+Start held */
static int sample(int *exit_combo)
{
    int src = 0;
    *exit_combo = 0;
    for (DWORD i = 0; xinput_get && i < 4; i++) {
        pad_state_t s;
        if (xinput_get(i, &s) != ERROR_SUCCESS) continue;
        if (s.Gamepad.wButtons || s.Gamepad.bLeftTrigger > 128 || s.Gamepad.bRightTrigger > 128) src |= 1;
        if ((s.Gamepad.wButtons & 0x0030) == 0x0030) *exit_combo = 1; /* START 0x10, BACK 0x20 */
    }
    if (GetAsyncKeyState(VK_SPACE) & 0x8000) src |= 2;
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) src |= 4;
    return src;
}

static void sleep_until(uint64_t t)
{
    for (;;) {
        uint64_t now = qpc_ns();
        if (now >= t) return;
        uint64_t left = t - now;
        if (left > 2000000) Sleep((DWORD)((left - 1000000) / 1000000));
        else YieldProcessor();
    }
}

static int arg_int(int argc, char **argv, int *i, int def)
{
    if (*i + 1 >= argc) return def;
    return atoi(argv[++*i]);
}

int main(int argc, char **argv)
{
    int cap = 0, gpu = 0, gpu_px = 2048, latency = 0, sync = 1, windowed = 0, buffers = 2;
    long frames_max = 0;
    const char *log_path = "lagprobe.log";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cap")) cap = arg_int(argc, argv, &i, 0);
        else if (!strcmp(argv[i], "--gpu")) gpu = arg_int(argc, argv, &i, 0);
        else if (!strcmp(argv[i], "--gpu-size")) gpu_px = arg_int(argc, argv, &i, 2048);
        else if (!strcmp(argv[i], "--latency")) latency = arg_int(argc, argv, &i, 0);
        else if (!strcmp(argv[i], "--sync")) sync = arg_int(argc, argv, &i, 1);
        else if (!strcmp(argv[i], "--buffers")) buffers = arg_int(argc, argv, &i, 2);
        else if (!strcmp(argv[i], "--frames")) frames_max = arg_int(argc, argv, &i, 0);
        else if (!strcmp(argv[i], "--windowed")) windowed = 1;
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_path = argv[++i];
        else {
            fprintf(stderr, "lagprobe: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (gpu_px < 64) gpu_px = 64;
    if (buffers < 2) buffers = 2;
    if (buffers > 8) buffers = 8;
    if (gpu_px > 8192) gpu_px = 8192;
    QueryPerformanceFrequency(&qpf);
    load_xinput();
    timeBeginPeriod(1);

    int w = windowed ? 1280 : GetSystemMetrics(SM_CXSCREEN), h = windowed ? 720 : GetSystemMetrics(SM_CYSCREEN);
    if (w <= 0 || h <= 0) w = 1280, h = 720;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "lagprobe";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "lagprobe", "lagprobe", windowed ? WS_OVERLAPPEDWINDOW | WS_VISIBLE : WS_POPUP | WS_VISIBLE, 0, 0, w,
                                h, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        fprintf(stderr, "lagprobe: no window\n");
        return 1;
    }
    RECT rc;
    GetClientRect(hwnd, &rc);
    w = rc.right - rc.left, h = rc.bottom - rc.top;

    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferCount = (UINT)buffers;
    sd.BufferDesc.Width = (UINT)w;
    sd.BufferDesc.Height = (UINT)h;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain *swap = NULL;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &swap, &dev, &fl,
                                               &ctx);
    if (FAILED(hr)) {
        fprintf(stderr, "lagprobe: D3D11CreateDeviceAndSwapChain 0x%lx\n", (unsigned long)hr);
        return 1;
    }
    if (latency > 0) {
        IDXGIDevice1 *d1 = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice1, (void **)&d1)) && d1) {
            IDXGIDevice1_SetMaximumFrameLatency(d1, (UINT)latency);
            IDXGIDevice1_Release(d1);
        }
    }
    ID3D11Texture2D *back = NULL;
    IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&back);
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)back, NULL, &rtv);

    ID3D11Texture2D *load[2] = {NULL, NULL};
    if (gpu > 0) {
        D3D11_TEXTURE2D_DESC td = {0};
        td.Width = td.Height = (UINT)gpu_px;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (int i = 0; i < 2; i++) ID3D11Device_CreateTexture2D(dev, &td, NULL, &load[i]);
        if (!load[0] || !load[1]) gpu = 0;
    }

    FILE *log = fopen(log_path, "w");
    if (!log) {
        fprintf(stderr, "lagprobe: cannot write %s\n", log_path);
        return 1;
    }
    setvbuf(log, NULL, _IOFBF, 1 << 16);
    fprintf(log, "# lagprobe 1 qpc_hz=%lld %dx%d cap=%d gpu=%dx%d latency=%d sync=%d xinput=%d buffers=%d\n", (long long)qpf.QuadPart, w, h, cap,
            gpu, gpu_px, latency, sync, xinput_get != NULL, buffers);

    uint64_t period = cap > 0 ? 1000000000ull / (uint64_t)cap : 0, next = qpc_ns(), t_flush = next, t_combo = 0;
    int prev = 0;
    for (long frame = 1; !quit && (!frames_max || frame <= frames_max); frame++) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (period) {
            sleep_until(next);
            next += period;
            uint64_t now = qpc_ns();
            if (now > next + period) next = now; /* fell behind (GPU-bound): no catching up */
        }
        int combo;
        uint64_t t_sample = qpc_ns();
        int src = sample(&combo);
        int down = src != 0;
        if (combo) {
            if (!t_combo) t_combo = t_sample;
            else if (t_sample - t_combo > 2000000000ull) quit = 1;
        } else {
            t_combo = 0;
        }
        if (down != (prev != 0)) fprintf(log, "P %ld %llu %d %d\n", frame, (unsigned long long)t_sample, down, down ? src : prev);
        prev = src;
        for (int i = 0; i < gpu; i++) ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)load[i & 1], (ID3D11Resource *)load[!(i & 1)]);
        const float white[4] = {1, 1, 1, 1}, black[4] = {0, 0, 0, 1};
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, down ? white : black);
        uint64_t t_present = qpc_ns();
        fprintf(log, "F %ld %llu\n", frame, (unsigned long long)t_present);
        hr = IDXGISwapChain_Present(swap, (UINT)sync, 0);
        if (FAILED(hr)) {
            fprintf(log, "# Present failed 0x%lx at frame %ld\n", (unsigned long)hr, frame);
            break;
        }
        if (t_present - t_flush > 1000000000ull) {
            fflush(log);
            t_flush = t_present;
        }
    }
    fclose(log);
    return 0;
}
