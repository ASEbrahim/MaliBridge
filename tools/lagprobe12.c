/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * lagprobe12.exe - lagprobe.exe (tools/lagprobe.c) on D3D12: the same black screen, white while a
 * button is held, the same arguments and the same log, for tools/latency_report.py. Through Proton
 * it runs on vkd3d-proton, as D3D12 games do (RE2 on the S10+), where the frame latency limit is
 * vkd3d-proton's own and needs VK_KHR_present_wait (client/wsi.c).
 *
 *   lagprobe12 [--cap FPS] [--gpu N] [--gpu-size PX] [--latency N] [--buffers N] [--sync 0|1] [--windowed]
 *              [--frames N] [--log PATH]
 *
 *   --gpu N        N copies of a PX x PX RGBA texture into another each frame (GPU load)
 *   --latency N    a swapchain with DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT,
 *                  IDXGISwapChain2::SetMaximumFrameLatency(N), and each frame waits for the swapchain's
 *                  waitable object before its sample (what latency-aware D3D12 games do); default:
 *                  no waitable object, the game's frames limited only by its own fences (one per
 *                  back buffer, as most games keep)
 *   --buffers N    BufferCount, 2-8 (default 3), and as many frames the program lets be in flight
 *   the rest as lagprobe.c.
 *
 * Built with mingw-w64 (tools/build_lagprobe.sh):
 *   x86_64-w64-mingw32-gcc -O2 lagprobe12.c -o lagprobe12.exe -ld3d12 -ldxgi -mwindows
 */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
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
    if (m == WM_CLOSE || m == WM_DESTROY || (m == WM_KEYDOWN && w == VK_ESCAPE)) {
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

static int sample(int *exit_combo)
{
    int src = 0;
    *exit_combo = 0;
    for (DWORD i = 0; xinput_get && i < 4; i++) {
        pad_state_t s;
        if (xinput_get(i, &s) != ERROR_SUCCESS) continue;
        if (s.Gamepad.wButtons || s.Gamepad.bLeftTrigger > 128 || s.Gamepad.bRightTrigger > 128) src |= 1;
        if ((s.Gamepad.wButtons & 0x0030) == 0x0030) *exit_combo = 1;
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

static void barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
}

#define FAIL(what, hr)                                                                                                                     \
    do {                                                                                                                                   \
        fprintf(stderr, "lagprobe12: %s 0x%lx\n", what, (unsigned long)(hr));                                                             \
        return 1;                                                                                                                          \
    } while (0)

int main(int argc, char **argv)
{
    int cap = 0, gpu = 0, gpu_px = 2048, latency = 0, sync = 1, windowed = 0, buffers = 3;
    long frames_max = 0;
    const char *log_path = "lagprobe.log";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cap")) cap = arg_int(argc, argv, &i, 0);
        else if (!strcmp(argv[i], "--gpu")) gpu = arg_int(argc, argv, &i, 0);
        else if (!strcmp(argv[i], "--gpu-size")) gpu_px = arg_int(argc, argv, &i, 2048);
        else if (!strcmp(argv[i], "--latency")) latency = arg_int(argc, argv, &i, 0);
        else if (!strcmp(argv[i], "--sync")) sync = arg_int(argc, argv, &i, 1);
        else if (!strcmp(argv[i], "--buffers")) buffers = arg_int(argc, argv, &i, 3);
        else if (!strcmp(argv[i], "--frames")) frames_max = arg_int(argc, argv, &i, 0);
        else if (!strcmp(argv[i], "--windowed")) windowed = 1;
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_path = argv[++i];
        else {
            fprintf(stderr, "lagprobe12: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (gpu_px < 64) gpu_px = 64;
    if (gpu_px > 8192) gpu_px = 8192;
    if (buffers < 2) buffers = 2;
    if (buffers > 8) buffers = 8;
    QueryPerformanceFrequency(&qpf);
    load_xinput();
    timeBeginPeriod(1);

    int w = windowed ? 1280 : GetSystemMetrics(SM_CXSCREEN), h = windowed ? 720 : GetSystemMetrics(SM_CYSCREEN);
    if (w <= 0 || h <= 0) w = 1280, h = 720;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "lagprobe12";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "lagprobe12", "lagprobe12", windowed ? WS_OVERLAPPEDWINDOW | WS_VISIBLE : WS_POPUP | WS_VISIBLE, 0, 0, w,
                                h, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) FAIL("no window", 0);
    RECT rc;
    GetClientRect(hwnd, &rc);
    w = rc.right - rc.left, h = rc.bottom - rc.top;

    ID3D12Device *dev = NULL;
    HRESULT hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev);
    if (FAILED(hr)) FAIL("D3D12CreateDevice", hr);
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    ID3D12CommandQueue *queue = NULL;
    hr = ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
    if (FAILED(hr)) FAIL("CreateCommandQueue", hr);
    IDXGIFactory2 *fac = NULL;
    hr = CreateDXGIFactory1(&IID_IDXGIFactory2, (void **)&fac);
    if (FAILED(hr)) FAIL("CreateDXGIFactory1", hr);
    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof(sd));
    sd.Width = (UINT)w;
    sd.Height = (UINT)h;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = (UINT)buffers;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = latency > 0 ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0;
    IDXGISwapChain1 *sc1 = NULL;
    hr = IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)queue, hwnd, &sd, NULL, NULL, &sc1);
    if (FAILED(hr)) FAIL("CreateSwapChainForHwnd", hr);
    IDXGISwapChain3 *swap = NULL;
    hr = IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&swap);
    if (FAILED(hr)) FAIL("IDXGISwapChain3", hr);
    HANDLE waitable = NULL;
    if (latency > 0) {
        IDXGISwapChain3_SetMaximumFrameLatency(swap, (UINT)latency);
        waitable = IDXGISwapChain3_GetFrameLatencyWaitableObject(swap);
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, (UINT)buffers, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ID3D12DescriptorHeap *heap = NULL;
    hr = ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap);
    if (FAILED(hr)) FAIL("CreateDescriptorHeap", hr);
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE base = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    ID3D12Resource *back[8] = {0};
    ID3D12CommandAllocator *alloc[8] = {0};
    ID3D12GraphicsCommandList *cl[8] = {0};
    UINT64 fence_at[8] = {0};
    for (int i = 0; i < buffers; i++) {
        hr = IDXGISwapChain3_GetBuffer(swap, (UINT)i, &IID_ID3D12Resource, (void **)&back[i]);
        if (FAILED(hr)) FAIL("GetBuffer", hr);
        D3D12_CPU_DESCRIPTOR_HANDLE hnd = {base.ptr + (SIZE_T)i * inc};
        ID3D12Device_CreateRenderTargetView(dev, back[i], NULL, hnd);
        hr = ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc[i]);
        if (FAILED(hr)) FAIL("CreateCommandAllocator", hr);
        hr = ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[i], NULL, &IID_ID3D12GraphicsCommandList,
                                            (void **)&cl[i]);
        if (FAILED(hr)) FAIL("CreateCommandList", hr);
        ID3D12GraphicsCommandList_Close(cl[i]);
    }
    ID3D12Fence *fence = NULL;
    hr = ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    if (FAILED(hr)) FAIL("CreateFence", hr);
    HANDLE fence_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    UINT64 fence_val = 0;

    ID3D12Resource *load[2] = {NULL, NULL};
    if (gpu > 0) {
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
        D3D12_RESOURCE_DESC rd;
        memset(&rd, 0, sizeof(rd));
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = (UINT64)gpu_px;
        rd.Height = (UINT)gpu_px;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_SOURCE, NULL, &IID_ID3D12Resource,
                                             (void **)&load[0]);
        ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                                             (void **)&load[1]);
        if (!load[0] || !load[1]) gpu = 0;
    }

    FILE *log = fopen(log_path, "w");
    if (!log) FAIL("cannot write the log", 0);
    setvbuf(log, NULL, _IOFBF, 1 << 16);
    fprintf(log, "# lagprobe 1 qpc_hz=%lld %dx%d cap=%d gpu=%dx%d latency=%d sync=%d xinput=%d buffers=%d api=d3d12\n", (long long)qpf.QuadPart, w,
            h, cap, gpu, gpu_px, latency, sync, xinput_get != NULL, buffers);

    uint64_t period = cap > 0 ? 1000000000ull / (uint64_t)cap : 0, next = qpc_ns(), t_flush = next, t_combo = 0;
    int prev = 0;
    for (long frame = 1; !quit && (!frames_max || frame <= frames_max); frame++) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (waitable) WaitForSingleObjectEx(waitable, 1000, TRUE);
        UINT bi = IDXGISwapChain3_GetCurrentBackBufferIndex(swap);
        /* the back buffer's last frame done on the GPU before its allocator is reused (the program's own frames in flight) */
        if (ID3D12Fence_GetCompletedValue(fence) < fence_at[bi]) {
            ID3D12Fence_SetEventOnCompletion(fence, fence_at[bi], fence_ev);
            WaitForSingleObject(fence_ev, 5000);
        }
        if (period) {
            sleep_until(next);
            next += period;
            uint64_t now = qpc_ns();
            if (now > next + period) next = now;
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
        ID3D12CommandAllocator_Reset(alloc[bi]);
        ID3D12GraphicsCommandList *c = cl[bi];
        ID3D12GraphicsCommandList_Reset(c, alloc[bi], NULL);
        for (int i = 0; i < gpu; i++) ID3D12GraphicsCommandList_CopyResource(c, load[1], load[0]);
        barrier(c, back[bi], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float white[4] = {1, 1, 1, 1}, black[4] = {0, 0, 0, 1};
        D3D12_CPU_DESCRIPTOR_HANDLE hnd = {base.ptr + (SIZE_T)bi * inc};
        ID3D12GraphicsCommandList_ClearRenderTargetView(c, hnd, down ? white : black, 0, NULL);
        barrier(c, back[bi], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        ID3D12GraphicsCommandList_Close(c);
        ID3D12CommandList *lists[1] = {(ID3D12CommandList *)c};
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
        uint64_t t_present = qpc_ns();
        fprintf(log, "F %ld %llu\n", frame, (unsigned long long)t_present);
        hr = IDXGISwapChain3_Present(swap, (UINT)sync, 0);
        if (FAILED(hr)) {
            fprintf(log, "# Present failed 0x%lx at frame %ld\n", (unsigned long)hr, frame);
            break;
        }
        ID3D12CommandQueue_Signal(queue, fence, ++fence_val);
        fence_at[bi] = fence_val;
        if (t_present - t_flush > 1000000000ull) {
            fflush(log);
            t_flush = t_present;
        }
    }
    fclose(log);
    if (ID3D12Fence_GetCompletedValue(fence) < fence_val) {
        ID3D12Fence_SetEventOnCompletion(fence, fence_val, fence_ev);
        WaitForSingleObject(fence_ev, 5000);
    }
    return 0;
}
