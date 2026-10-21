#include "ai/wic_decode.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <propidl.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "platform_win.h"

namespace ai {

// IWICBitmapSourceTransform is missing from MinGW's wincodec.h; layout per the Windows SDK.
static const GUID kIID_SourceTransform = {0x3b16811b, 0x6a43, 0x4ec9, {0xb7, 0x13, 0x3d, 0x5a, 0x0c, 0x13, 0xb9, 0x40}};
struct ISourceTransform : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE CopyPixels(const WICRect* rc, UINT w, UINT h, WICPixelFormatGUID* fmt,
                                               WICBitmapTransformOptions opts, UINT stride, UINT size, BYTE* buf) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetClosestSize(UINT* w, UINT* h) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetClosestPixelFormat(WICPixelFormatGUID* fmt) = 0;
  virtual HRESULT STDMETHODCALLTYPE DoesSupportTransform(WICBitmapTransformOptions opts, BOOL* ok) = 0;
};

template <class T>
struct ComPtr {
  T* p = nullptr;
  ~ComPtr() {
    if (p) p->Release();
  }
  T** operator&() { return &p; }
  T* operator->() { return p; }
};

// COM apartment + WIC factory scoped to one decode call. Declared before the other COM
// objects in decodeScaled, so those are released first and nothing outlives CoUninitialize
// (a thread_local factory crashed when threads / the process exited).
namespace {
struct WicScope {
  IWICImagingFactory* f = nullptr;
  bool uninit = false;
  WicScope() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    uninit = SUCCEEDED(hr);  // RPC_E_CHANGED_MODE: thread already has an STA, which is fine
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
  }
  ~WicScope() {
    if (f) f->Release();
    if (uninit) CoUninitialize();
  }
};
}  // namespace

static int readOrientation(IWICMetadataQueryReader* qr) {
  const wchar_t* keys[] = {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"};
  for (const wchar_t* k : keys) {
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(qr->GetMetadataByName(k, &v))) {
      int o = v.vt == VT_UI2 ? v.uiVal : 1;
      PropVariantClear(&v);
      if (o >= 1 && o <= 8) return o;
    }
  }
  return 1;
}

static int64_t readShotTime(IWICMetadataQueryReader* qr) {
  const wchar_t* keys[] = {L"/app1/ifd/exif/{ushort=36867}", L"/ifd/exif/{ushort=36867}"};
  for (const wchar_t* k : keys) {
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(qr->GetMetadataByName(k, &v))) {
      int Y = 0, M = 0, D = 0, h = 0, m = 0, s = 0;
      bool ok = v.vt == VT_LPSTR && sscanf(v.pszVal, "%d:%d:%d %d:%d:%d", &Y, &M, &D, &h, &m, &s) == 6;
      PropVariantClear(&v);
      if (ok && Y > 1900) {
        // Days from civil (Howard Hinnant); only differences matter.
        Y -= M <= 2;
        int64_t era = Y / 400, yoe = Y - era * 400, doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
        int64_t days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return days * 86400 + h * 3600 + m * 60 + s;
      }
    }
  }
  return 0;
}

static void applyOrientation(SmallImage& im, int o) {
  if (o <= 1) return;
  int W = im.w, H = im.h;
  int ow = o >= 5 ? H : W, oh = o >= 5 ? W : H;
  std::vector<uint8_t> out((size_t)ow * oh * 3);
  for (int y = 0; y < oh; y++)
    for (int x = 0; x < ow; x++) {
      int sx, sy;
      switch (o) {
        case 2: sx = W - 1 - x; sy = y; break;
        case 3: sx = W - 1 - x; sy = H - 1 - y; break;
        case 4: sx = x; sy = H - 1 - y; break;
        case 5: sx = y; sy = x; break;
        case 6: sx = y; sy = H - 1 - x; break;
        case 7: sx = W - 1 - y; sy = H - 1 - x; break;
        default: sx = W - 1 - y; sy = x; break;  // 8
      }
      const uint8_t* s = &im.rgb[((size_t)sy * W + sx) * 3];
      uint8_t* d = &out[((size_t)y * ow + x) * 3];
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
    }
  im.rgb.swap(out);
  im.w = ow;
  im.h = oh;
}

bool decodeScaled(const std::string& path, int maxDim, SmallImage& out, DecodeInfo* info, std::string& err) {
  WicScope wic;
  IWICImagingFactory* f = wic.f;
  if (!f) { err = "WIC unavailable"; return false; }
  ComPtr<IWICBitmapDecoder> dec;
  if (FAILED(f->CreateDecoderFromFilename(utf8ToWide(path).c_str(), nullptr, GENERIC_READ,
                                          WICDecodeMetadataCacheOnDemand, &dec))) {
    err = "unsupported or unreadable image";
    return false;
  }
  ComPtr<IWICBitmapFrameDecode> fr;
  if (FAILED(dec->GetFrame(0, &fr))) { err = "no frame"; return false; }
  UINT W = 0, H = 0;
  fr->GetSize(&W, &H);
  int orient = 1;
  int64_t shot = 0;
  {
    ComPtr<IWICMetadataQueryReader> qr;
    if (SUCCEEDED(fr->GetMetadataQueryReader(&qr))) {
      orient = readOrientation(qr.p);
      shot = readShotTime(qr.p);
    }
  }
  // Largest power-of-two reduction that keeps the long edge >= maxDim.
  UINT k = 1;
  while (k < 8 && std::max(W, H) / (k * 2) >= (UINT)maxDim) k *= 2;
  UINT dw = std::max(1u, W / k), dh = std::max(1u, H / k);
  SmallImage im;
  bool done = false;
  ComPtr<ISourceTransform> tr;
  if (k > 1 && SUCCEEDED(fr->QueryInterface(kIID_SourceTransform, (void**)&tr))) {
    UINT cw = dw, ch = dh;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(tr->GetClosestSize(&cw, &ch)) && SUCCEEDED(tr->GetClosestPixelFormat(&fmt)) &&
        IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR)) {
      im.w = (int)cw;
      im.h = (int)ch;
      UINT stride = cw * 3;
      im.rgb.resize((size_t)stride * ch);
      done = SUCCEEDED(tr->CopyPixels(nullptr, cw, ch, &fmt, WICBitmapTransformRotate0, stride, (UINT)im.rgb.size(),
                                      im.rgb.data()));
    }
  }
  if (!done) {
    ComPtr<IWICBitmapScaler> sc;
    ComPtr<IWICFormatConverter> cv;
    if (FAILED(f->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(fr.p, dw, dh, WICBitmapInterpolationModeFant)) ||
        FAILED(f->CreateFormatConverter(&cv)) ||
        FAILED(cv->Initialize(sc.p, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeCustom))) {
      err = "decode setup failed";
      return false;
    }
    im.w = (int)dw;
    im.h = (int)dh;
    UINT stride = dw * 3;
    im.rgb.resize((size_t)stride * dh);
    if (FAILED(cv->CopyPixels(nullptr, stride, (UINT)im.rgb.size(), im.rgb.data()))) {
      err = "decode failed";
      return false;
    }
  }
  for (size_t i = 0; i < im.rgb.size(); i += 3) std::swap(im.rgb[i], im.rgb[i + 2]);  // BGR -> RGB
  applyOrientation(im, orient);
  fitLongEdge(im, maxDim, out);
  if (info) {
    info->origW = orient >= 5 ? (int)H : (int)W;
    info->origH = orient >= 5 ? (int)W : (int)H;
    info->shotTime = shot;
  }
  return true;
}

void resample(const SmallImage& in, float x0, float y0, float x1, float y1, int outW, int outH, SmallImage& out) {
  out.w = outW;
  out.h = outH;
  out.rgb.assign((size_t)outW * outH * 3, 0);
  float sx = (x1 - x0) / outW, sy = (y1 - y0) / outH;
  bool area = sx > 1.f || sy > 1.f;
  for (int y = 0; y < outH; y++) {
    for (int x = 0; x < outW; x++) {
      float acc[3] = {0, 0, 0};
      if (area) {
        int ax0 = (int)std::floor(x0 + x * sx), ax1 = std::max(ax0 + 1, (int)std::floor(x0 + (x + 1) * sx));
        int ay0 = (int)std::floor(y0 + y * sy), ay1 = std::max(ay0 + 1, (int)std::floor(y0 + (y + 1) * sy));
        int n = 0;
        for (int yy = ay0; yy < ay1; yy++) {
          int cy = std::clamp(yy, 0, in.h - 1);
          for (int xx = ax0; xx < ax1; xx++) {
            const uint8_t* p = &in.rgb[((size_t)cy * in.w + std::clamp(xx, 0, in.w - 1)) * 3];
            acc[0] += p[0];
            acc[1] += p[1];
            acc[2] += p[2];
            n++;
          }
        }
        for (float& a : acc) a /= n;
      } else {
        float fx = x0 + (x + 0.5f) * sx - 0.5f, fy = y0 + (y + 0.5f) * sy - 0.5f;
        int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
        float tx = fx - ix, ty = fy - iy;
        for (int c = 0; c < 3; c++) {
          auto px = [&](int xx, int yy) {
            return (float)in.rgb[((size_t)std::clamp(yy, 0, in.h - 1) * in.w + std::clamp(xx, 0, in.w - 1)) * 3 + c];
          };
          acc[c] = (px(ix, iy) * (1 - tx) + px(ix + 1, iy) * tx) * (1 - ty) +
                   (px(ix, iy + 1) * (1 - tx) + px(ix + 1, iy + 1) * tx) * ty;
        }
      }
      uint8_t* d = &out.rgb[((size_t)y * outW + x) * 3];
      for (int c = 0; c < 3; c++) d[c] = (uint8_t)std::clamp((int)std::lround(acc[c]), 0, 255);
    }
  }
}

void fitLongEdge(const SmallImage& in, int maxDim, SmallImage& out) {
  int L = std::max(in.w, in.h);
  if (L <= maxDim) {
    if (&out != &in) out = in;
    return;
  }
  float s = (float)maxDim / L;
  int w = std::max(1, (int)std::lround(in.w * s)), h = std::max(1, (int)std::lround(in.h * s));
  SmallImage tmp;
  resample(in, 0, 0, (float)in.w, (float)in.h, w, h, tmp);
  out = std::move(tmp);
}

}  // namespace ai
