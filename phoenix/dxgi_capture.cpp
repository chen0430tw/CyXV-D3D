/*
 * dxgi_capture.cpp — DXGI Desktop Duplication screen capture
 *
 * Replaces the PrintWindow approach in phoenix_streamer.py.
 * Uses IDXGIOutputDuplication to grab frames directly from the GPU
 * framebuffer, reducing capture latency from ~20ms to ~2ms.
 *
 * Output protocol (stdout, binary):
 *   Per frame:
 *     [width  : uint32_t LE]
 *     [height : uint32_t LE]
 *     [pixels : width * height * 3 bytes, BGR24]
 *
 * phoenix_streamer.py reads from this process's stdout via subprocess.
 *
 * Build (MSVC):
 *   cl /EHsc /O2 dxgi_capture.cpp /link dxgi.lib d3d11.lib
 *
 * Build (MinGW-w64 cross from Cygwin):
 *   x86_64-w64-mingw32-g++ -O2 dxgi_capture.cpp -o dxgi_capture.exe \
 *       -ldxgi -ld3d11 -lole32
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdint.h>
#include <io.h>
#include <fcntl.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void die(const char *msg, HRESULT hr) {
    fprintf(stderr, "[dxgi_capture] %s (HRESULT=0x%08lX)\n", msg, (unsigned long)hr);
    exit(1);
}

/* write_all: keep writing until all bytes are sent or pipe breaks */
static int write_all(const void *buf, size_t n) {
    const char *p = (const char *)buf;
    while (n) {
        int written = _write(1 /*stdout*/, p, (unsigned int)n);
        if (written <= 0) return -1;
        p += written;
        n -= written;
    }
    return 0;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    /* stdout must be binary — no CR/LF translation */
    _setmode(1, _O_BINARY);
    _setmode(0, _O_BINARY);

    /* Which output/monitor to duplicate (0 = primary) */
    int monitor_idx = (argc > 1) ? atoi(argv[1]) : 0;

    HRESULT hr;

    /* 1. Create D3D11 device ------------------------------------------------ */
    ID3D11Device        *device  = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL    level;

    hr = D3D11CreateDevice(
        NULL,                        /* default adapter */
        D3D_DRIVER_TYPE_HARDWARE,
        NULL, 0,
        NULL, 0,                     /* default feature levels */
        D3D11_SDK_VERSION,
        &device, &level, &context
    );
    if (FAILED(hr)) die("D3D11CreateDevice", hr);

    /* 2. Get DXGI device / adapter / output -------------------------------- */
    IDXGIDevice  *dxgi_device  = NULL;
    IDXGIAdapter *dxgi_adapter = NULL;
    IDXGIOutput  *dxgi_output  = NULL;
    IDXGIOutput1 *dxgi_output1 = NULL;

    hr = device->QueryInterface(__uuidof(IDXGIDevice),
                                (void **)&dxgi_device);
    if (FAILED(hr)) die("QI IDXGIDevice", hr);

    hr = dxgi_device->GetAdapter(&dxgi_adapter);
    if (FAILED(hr)) die("GetAdapter", hr);

    hr = dxgi_adapter->EnumOutputs((UINT)monitor_idx, &dxgi_output);
    if (FAILED(hr)) die("EnumOutputs (invalid monitor index?)", hr);

    hr = dxgi_output->QueryInterface(__uuidof(IDXGIOutput1),
                                     (void **)&dxgi_output1);
    if (FAILED(hr)) die("QI IDXGIOutput1", hr);

    /* 3. Create duplication ------------------------------------------------ */
    IDXGIOutputDuplication *dup = NULL;
    hr = dxgi_output1->DuplicateOutput(device, &dup);
    if (FAILED(hr)) die("DuplicateOutput", hr);

    DXGI_OUTDUPL_DESC dup_desc;
    dup->GetDesc(&dup_desc);
    int W = (int)dup_desc.ModeDesc.Width;
    int H = (int)dup_desc.ModeDesc.Height;

    fprintf(stderr, "[dxgi_capture] monitor %d: %dx%d\n", monitor_idx, W, H);

    /* 4. Staging texture (CPU-readable) ------------------------------------ */
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width  = (UINT)W;
    td.Height = (UINT)H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage     = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device->CreateTexture2D(&td, NULL, &staging);
    if (FAILED(hr)) die("CreateTexture2D staging", hr);

    /* Output frame header once so the reader can init the encoder */
    uint32_t frame_w = (uint32_t)W;
    uint32_t frame_h = (uint32_t)H;

    /* BGR24 row buffer */
    int    row_bytes = W * 3;
    BYTE  *row_buf   = (BYTE *)malloc((size_t)row_bytes);

    /* 5. Capture loop ------------------------------------------------------- */
    while (1) {
        DXGI_OUTDUPL_FRAME_INFO fi;
        IDXGIResource *resource = NULL;

        hr = dup->AcquireNextFrame(500 /*ms timeout*/, &fi, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) {
            /* Output change (resolution/rotation): recreate duplication */
            dup->Release();
            hr = dxgi_output1->DuplicateOutput(device, &dup);
            if (FAILED(hr)) die("DuplicateOutput (recreate)", hr);
            continue;
        }

        /* Copy GPU texture → staging (CPU-visible) */
        ID3D11Texture2D *tex = NULL;
        resource->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex);
        if (tex) {
            context->CopyResource(staging, tex);
            tex->Release();
        }
        resource->Release();
        dup->ReleaseFrame();

        /* Map staging texture */
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) continue;

        /* Send frame header */
        if (write_all(&frame_w, 4) < 0) break;
        if (write_all(&frame_h, 4) < 0) break;

        /* Send BGR24 rows (drop alpha channel) */
        BYTE *src = (BYTE *)mapped.pData;
        int  ok = 0;
        for (int y = 0; y < H; y++) {
            BYTE *row_src = src + y * mapped.RowPitch;
            /* BGRA → BGR24 */
            for (int x = 0; x < W; x++) {
                row_buf[x*3+0] = row_src[x*4+0]; /* B */
                row_buf[x*3+1] = row_src[x*4+1]; /* G */
                row_buf[x*3+2] = row_src[x*4+2]; /* R */
            }
            if (write_all(row_buf, row_bytes) < 0) { ok = -1; break; }
        }
        context->Unmap(staging, 0);
        if (ok < 0) break;
    }

    free(row_buf);
    return 0;
}
