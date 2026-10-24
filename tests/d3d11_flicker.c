/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * d3d11_flicker.exe [frames] [draws]: every frame of a game-like D3D11 workload checked. Per frame
 * a 256-cell grid is drawn into an offscreen target, one draw per cell, each with a
 * Map(WRITE_DISCARD) constant buffer and its own texture binding (which texture and colour depend
 * on the frame), one texture rewritten with UpdateSubresource every frame; the target is copied to
 * one of three staging textures and read back two frames later, as games read back. A cell with
 * another frame's or another draw's colour is what a game shows as a flickering frame.
 *
 * Built with mingw-w64:  x86_64-w64-mingw32-gcc -O2 d3d11_flicker.c -o d3d11_flicker.exe -ld3d11 -ld3dcompiler_47
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GRID 16
#define CELL 16
#define SIZE (GRID * CELL)
#define NTEX 8
#define RING 3

static const uint32_t tex_rgba[NTEX] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xff00ffff, 0xffff00ff, 0xffffff00, 0xffffffff, 0xff808080};

/* What draw d of frame fr writes: texture colour times the constant buffer's colour. */
static uint32_t dyn_rgba(int fr) { return 0xff000000u | (uint32_t)((fr * 37) & 0xff) << 16 | (uint32_t)((fr * 11) & 0xff) << 8 | (uint32_t)((fr * 5) & 0xff); }
static int tex_of(int d, int fr) { return d % 5 == 0 ? -1 : (d + fr) % NTEX; }
static void col_of(int d, int fr, float c[4])
{
    int k = d * 7 + fr;
    c[0] = (k & 1) ? 1.0f : 0.5f;
    c[1] = (k & 2) ? 1.0f : 0.5f;
    c[2] = (k & 4) ? 1.0f : 0.5f;
    c[3] = 1;
}
static uint32_t expect(int d, int fr)
{
    int t = tex_of(d, fr);
    uint32_t tc = t < 0 ? dyn_rgba(fr) : tex_rgba[t];
    float c[4];
    col_of(d, fr, c);
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        float v = (float)((tc >> (8 * i)) & 0xff) / 255.0f * c[i];
        r |= (uint32_t)(v * 255.0f + 0.5f) << (8 * i);
    }
    return r;
}
static int close_enough(uint32_t a, uint32_t b)
{
    for (int i = 0; i < 4; i++) {
        int x = (int)((a >> (8 * i)) & 0xff), y = (int)((b >> (8 * i)) & 0xff);
        if (x - y > 1 || y - x > 1) return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 600;
    int draws = argc > 2 ? atoi(argv[2]) : GRID * GRID;
    if (draws > GRID * GRID) draws = GRID * GRID;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, &ctx))) {
        printf("FAILED: no device\n");
        return 1;
    }
    const char *vs = "cbuffer C : register(b0) { float4 pos; float4 col; };"
                     "struct O { float4 p : SV_Position; float4 c : COLOR; };"
                     "O main(uint id : SV_VertexID) { O o; float2 t = float2(id & 1, id >> 1);"
                     "o.p = float4(pos.xy + t * pos.zw, 0, 1); o.c = col; return o; }";
    const char *ps = "Texture2D t : register(t0); SamplerState s : register(s0);"
                     "float4 main(float4 p : SV_Position, float4 c : COLOR) : SV_Target { return c * t.Sample(s, float2(0.5, 0.5)); }";
    ID3DBlob *vb, *pb, *err = NULL;
    if (FAILED(D3DCompile(vs, strlen(vs), NULL, NULL, NULL, "main", "vs_5_0", 0, 0, &vb, &err)) ||
        FAILED(D3DCompile(ps, strlen(ps), NULL, NULL, NULL, "main", "ps_5_0", 0, 0, &pb, &err))) {
        printf("FAILED: shader compile\n");
        return 1;
    }
    ID3D11VertexShader *vsh;
    ID3D11PixelShader *psh;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &vsh);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &psh);
    D3D11_BUFFER_DESC bd = {32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ID3D11Buffer *cb;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &cb);
    ID3D11ShaderResourceView *srv[NTEX], *dyn_srv;
    ID3D11Texture2D *dyn;
    uint32_t px[16];
    for (int i = 0; i < NTEX; i++) {
        for (int j = 0; j < 16; j++) px[j] = tex_rgba[i];
        D3D11_TEXTURE2D_DESC td = {4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SUBRESOURCE_DATA init = {px, 16, 0};
        ID3D11Texture2D *t;
        ID3D11Device_CreateTexture2D(dev, &td, &init, &t);
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t, NULL, &srv[i]);
    }
    {
        D3D11_TEXTURE2D_DESC td = {4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &dyn);
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)dyn, NULL, &dyn_srv);
    }
    D3D11_SAMPLER_DESC sd = {D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                             0, 1, D3D11_COMPARISON_NEVER, {0}, 0, D3D11_FLOAT32_MAX};
    ID3D11SamplerState *smp;
    ID3D11Device_CreateSamplerState(dev, &sd, &smp);
    D3D11_TEXTURE2D_DESC rd = {SIZE, SIZE, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    ID3D11Texture2D *rt, *st[RING];
    ID3D11Device_CreateTexture2D(dev, &rd, NULL, &rt);
    rd.Usage = D3D11_USAGE_STAGING;
    rd.BindFlags = 0;
    rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int i = 0; i < RING; i++) ID3D11Device_CreateTexture2D(dev, &rd, NULL, &st[i]);
    ID3D11RenderTargetView *rtv;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv);
    D3D11_VIEWPORT vp = {0, 0, SIZE, SIZE, 0, 1};
    D3D11_RASTERIZER_DESC rsd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
    ID3D11RasterizerState *rs;
    ID3D11Device_CreateRasterizerState(dev, &rsd, &rs);

    int bad_frames = 0, bad_cells = 0, checked = 0;
    char first[256] = "";
    for (int fr = 0; fr < frames + RING; fr++) {
        if (fr >= RING - 1 && fr - (RING - 1) < frames) {
            /* Read back the frame from two frames ago. */
            int pf = fr - (RING - 1);
            D3D11_MAPPED_SUBRESOURCE m;
            if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st[pf % RING], 0, D3D11_MAP_READ, 0, &m))) {
                printf("FAILED: map\n");
                return 1;
            }
            int bad = 0;
            for (int d = 0; d < draws; d++) {
                int x = (d % GRID) * CELL + CELL / 2, y = (d / GRID) * CELL + CELL / 2;
                uint32_t v = *(const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch + (size_t)x * 4);
                uint32_t want = expect(d, pf);
                if (!close_enough(v, want)) {
                    if (!bad_cells && !bad)
                        snprintf(first, sizeof(first), "frame %d cell %d: 0x%08x, want 0x%08x", pf, d, v, want);
                    bad++;
                }
            }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st[pf % RING], 0);
            checked++;
            bad_cells += bad;
            bad_frames += bad != 0;
        }
        if (fr >= frames) continue;
        uint32_t dp[16];
        for (int j = 0; j < 16; j++) dp[j] = dyn_rgba(fr);
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)dyn, 0, NULL, dp, 16, 0);
        float clear[4] = {0, 0, 0, 0};
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_RSSetState(ctx, rs);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ID3D11DeviceContext_VSSetShader(ctx, vsh, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, psh, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb);
        ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &smp);
        for (int d = 0; d < draws; d++) {
            D3D11_MAPPED_SUBRESOURCE m;
            ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
            float *p = m.pData;
            p[0] = -1 + 2.0f * (d % GRID) / GRID;
            p[1] = 1 - 2.0f * (d / GRID + 1) / GRID;
            p[2] = 2.0f / GRID; /* a quad (strip of four) covering the cell */
            p[3] = 2.0f / GRID;
            col_of(d, fr, p + 4);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb, 0);
            int t = tex_of(d, fr);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, t < 0 ? &dyn_srv : &srv[t]);
            ID3D11DeviceContext_Draw(ctx, 4, 0);
        }
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st[fr % RING], (ID3D11Resource *)rt);
        ID3D11DeviceContext_Flush(ctx);
    }
    printf("%s every frame's cells: %d frames checked, %d cells wrong in %d frames%s%s\n", bad_cells ? "FAIL" : "PASS", checked, bad_cells,
           bad_frames, first[0] ? "; first: " : "", first);
    printf("%s\n", bad_cells || checked != frames ? "FAILED" : "OK");
    return bad_cells || checked != frames;
}
