// Decode an image through Windows Imaging Component (e.g. the Nikon NEF Codec)
// and dump frame 0 as raw pixels (48bpp RGB, little-endian) to a file.
// Build: g++ -O2 wicdecode.cpp -o wicdecode.exe -lole32 -lwindowscodecs -luuid
#include <windows.h>
#include <wincodec.h>
#include <cstdio>
#include <vector>
#include <string>

static std::wstring widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    w.resize(n - 1);
    return w;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: wicdecode in.NEF out.rgb48\n"); return 2; }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory *f = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (FAILED(hr)) { fprintf(stderr, "factory %08lx\n", hr); return 1; }
    IWICBitmapDecoder *dec = nullptr;
    hr = f->CreateDecoderFromFilename(widen(argv[1]).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (FAILED(hr)) { fprintf(stderr, "decoder %08lx\n", hr); return 1; }
    IWICBitmapFrameDecode *fr = nullptr;
    hr = dec->GetFrame(0, &fr);
    if (FAILED(hr)) { fprintf(stderr, "frame %08lx\n", hr); return 1; }
    UINT w = 0, h = 0;
    fr->GetSize(&w, &h);
    IWICFormatConverter *cv = nullptr;
    f->CreateFormatConverter(&cv);
    hr = cv->Initialize(fr, GUID_WICPixelFormat48bppRGB, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) { fprintf(stderr, "convert %08lx\n", hr); return 1; }
    std::vector<BYTE> buf(size_t(w) * h * 6);
    DWORD t0 = GetTickCount();
    hr = cv->CopyPixels(nullptr, w * 6, UINT(buf.size()), buf.data());
    if (FAILED(hr)) { fprintf(stderr, "copy %08lx\n", hr); return 1; }
    FILE *o = fopen(argv[2], "wb");
    if (!o) return 1;
    fwrite(buf.data(), 1, buf.size(), o);
    fclose(o);
    printf("%u %u rgb48 decode_ms=%lu\n", w, h, GetTickCount() - t0);
    return 0;
}
