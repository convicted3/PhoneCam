#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mfobjects.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "h264_decoder.h"
#include "phonecam_client.h"
#include "i18n.h"

// GUIDs defined locally so we don't need mfuuid.lib / wmcodecdsp.lib.
DEFINE_GUID(PC_CLSID_CMSH264DecoderMFT, 0x62CE7E72, 0x4C71, 0x4d20, 0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D);
DEFINE_GUID(PC_CODECAPI_AVDecNumWorkerThreads, 0x9561c3e8, 0xea9e, 0x4435, 0x9b, 0x1e, 0xa9, 0x3e, 0x69, 0x18, 0x94, 0xd8);
DEFINE_GUID(PC_CODECAPI_AVLowLatencyMode, 0x9c27891a, 0xed7a, 0x40e1, 0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee);
DEFINE_GUID(PC_MFMediaType_Video, 0x73646976, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
DEFINE_GUID(PC_MFVideoFormat_H264, 0x34363248, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
DEFINE_GUID(PC_MFVideoFormat_NV12, 0x3231564E, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
DEFINE_GUID(PC_MF_MT_MAJOR_TYPE, 0x48eba18e, 0xf8c9, 0x4687, 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f);
DEFINE_GUID(PC_MF_MT_SUBTYPE, 0xf7e34c9a, 0x42e8, 0x4714, 0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5);
DEFINE_GUID(PC_MF_MT_FRAME_SIZE, 0x1652c33d, 0xd6b2, 0x4012, 0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d);
DEFINE_GUID(PC_MF_MT_DEFAULT_STRIDE, 0x644b4e48, 0x1e02, 0x4516, 0xb0, 0xeb, 0xc0, 0x1c, 0xa9, 0xd4, 0x9a, 0xc6);
DEFINE_GUID(PC_MF_MT_MINIMUM_DISPLAY_APERTURE, 0xd7388766, 0x18fe, 0x48c6, 0xa1, 0x77, 0xee, 0x89, 0x48, 0x67, 0xc8, 0xc4);
DEFINE_GUID(PC_MF_MT_INTERLACE_MODE, 0xe2724bb8, 0xe676, 0x4806, 0xb4, 0xb2, 0xa8, 0xd6, 0xef, 0xb4, 0x4c, 0xcd);
DEFINE_GUID(PC_IID_IMFTransform, 0xbf94c121, 0x5b05, 0x4e6f, 0x80, 0x00, 0xba, 0x59, 0x89, 0x61, 0x41, 0x4d);
DEFINE_GUID(PC_IID_IMF2DBuffer, 0x7DC9D5F9, 0x9ED9, 0x44ec, 0x9B, 0xBF, 0x06, 0x80, 0xBB, 0xC4, 0x45, 0x0D);

namespace phonecam {

struct H264Decoder::Impl {
    IMFTransform* mft = nullptr;
    bool mf_started = false;
    bool com_inited = false;
    UINT32 width = 0, height = 0;      // allocated (coded) size
    UINT32 disp_w = 0, disp_h = 0;     // visible size
    LONG stride = 0;
    bool provides_samples = false;
    DWORD out_size = 0;
    std::vector<uint8_t> copy;
};

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

H264Decoder::H264Decoder() : d_(new Impl) {}

H264Decoder::~H264Decoder() {
    if (d_->mft) {
        d_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        SafeRelease(d_->mft);
    }
    if (d_->mf_started) MFShutdown();
    if (d_->com_inited) CoUninitialize();
    delete d_;
}

static std::string Hex(HRESULT hr) {
    char b[32];
    snprintf(b, sizeof b, "0x%08lX", (unsigned long)hr);
    return b;
}

bool H264Decoder::Init(std::string& err) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    d_->com_inited = SUCCEEDED(hr);
    hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) { err = "MFStartup failed " + Hex(hr) + TR(" (нет Media Feature Pack?)"); return false; }
    d_->mf_started = true;

    hr = CoCreateInstance(PC_CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, PC_IID_IMFTransform,
                          (void**)&d_->mft);
    if (FAILED(hr)) { err = TR("Не найден H.264 декодер Windows ") + Hex(hr) + TR(" (установи Media Feature Pack для N-версий)"); return false; }

    IMFAttributes* attrs = nullptr;
    if (SUCCEEDED(d_->mft->GetAttributes(&attrs)) && attrs) {
        attrs->SetUINT32(PC_CODECAPI_AVLowLatencyMode, TRUE);
        // use several CPU cores: Full HD 60 is close to the limit of a single core
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        attrs->SetUINT32(PC_CODECAPI_AVDecNumWorkerThreads, std::max<DWORD>(2, std::min<DWORD>(8, si.dwNumberOfProcessors)));
        attrs->Release();
    }

    IMFMediaType* in = nullptr;
    MFCreateMediaType(&in);
    in->SetGUID(PC_MF_MT_MAJOR_TYPE, PC_MFMediaType_Video);
    in->SetGUID(PC_MF_MT_SUBTYPE, PC_MFVideoFormat_H264);
    in->SetUINT32(PC_MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = d_->mft->SetInputType(0, in, 0);
    in->Release();
    if (FAILED(hr)) { err = "SetInputType " + Hex(hr); return false; }

    if (!NegotiateOutput()) { err = TR("Декодер не отдаёт NV12"); return false; }
    d_->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    d_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool H264Decoder::NegotiateOutput() {
    for (DWORD i = 0;; i++) {
        IMFMediaType* t = nullptr;
        if (FAILED(d_->mft->GetOutputAvailableType(0, i, &t))) return false;
        GUID sub{};
        t->GetGUID(PC_MF_MT_SUBTYPE, &sub);
        if (sub == PC_MFVideoFormat_NV12) {
            HRESULT hr = d_->mft->SetOutputType(0, t, 0);
            if (SUCCEEDED(hr)) {
                UINT64 fs = 0;
                if (SUCCEEDED(t->GetUINT64(PC_MF_MT_FRAME_SIZE, &fs))) {
                    d_->width = (UINT32)(fs >> 32);
                    d_->height = (UINT32)(fs & 0xffffffff);
                }
                d_->disp_w = d_->width;
                d_->disp_h = d_->height;
                MFVideoArea area{};
                UINT32 sz = 0;
                if (SUCCEEDED(t->GetBlob(PC_MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof area, &sz)) &&
                    sz == sizeof area && area.Area.cx > 0 && area.Area.cy > 0) {
                    d_->disp_w = (UINT32)area.Area.cx;
                    d_->disp_h = (UINT32)area.Area.cy;
                }
                UINT32 st = 0;
                d_->stride = SUCCEEDED(t->GetUINT32(PC_MF_MT_DEFAULT_STRIDE, &st)) ? (LONG)(INT32)st : (LONG)d_->width;
                if (d_->stride <= 0) d_->stride = (LONG)d_->width;
                MFT_OUTPUT_STREAM_INFO info{};
                d_->mft->GetOutputStreamInfo(0, &info);
                d_->provides_samples =
                    (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
                d_->out_size = info.cbSize;
                char lb[200];
                snprintf(lb, sizeof lb, "decoder output NV12 %ux%u visible %ux%u stride %ld cbSize %lu provides %d flags 0x%lx",
                         d_->width, d_->height, d_->disp_w, d_->disp_h, (long)d_->stride, (unsigned long)info.cbSize,
                         (int)d_->provides_samples, (unsigned long)info.dwFlags);
                Log(lb);
            }
            t->Release();
            return SUCCEEDED(hr);
        }
        t->Release();
    }
}

void H264Decoder::Reset() {
    if (d_->mft) d_->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
}

bool H264Decoder::Drain(const std::function<void(const Frame&)>& on_frame, std::string& err) {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER out{};
        IMFSample* sample = nullptr;
        if (!d_->provides_samples) {
            DWORD need = d_->out_size ? d_->out_size : d_->width * d_->height * 3 / 2;
            if (need == 0) need = 1920 * 1088 * 3 / 2;
            IMFMediaBuffer* buf = nullptr;
            MFCreateSample(&sample);
            MFCreateMemoryBuffer(need, &buf);
            sample->AddBuffer(buf);
            buf->Release();
            out.pSample = sample;
        }
        DWORD status = 0;
        HRESULT hr = d_->mft->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            SafeRelease(sample);
            return true;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            SafeRelease(sample);
            if (!NegotiateOutput()) { err = TR("смена формата не удалась"); return false; }
            continue;
        }
        if (FAILED(hr)) {
            SafeRelease(sample);
            err = "ProcessOutput " + Hex(hr);
            return false;
        }
        IMFSample* got = out.pSample;
        if (got) {
            IMFMediaBuffer* mb = nullptr;
            if (SUCCEEDED(got->ConvertToContiguousBuffer(&mb)) && mb) {
                BYTE* p = nullptr;
                LONG pitch = d_->stride;
                IMF2DBuffer* b2 = nullptr;
                bool locked2d = false;
                if (SUCCEEDED(mb->QueryInterface(PC_IID_IMF2DBuffer, (void**)&b2)) && b2) {
                    if (SUCCEEDED(b2->Lock2D(&p, &pitch))) locked2d = true;
                }
                DWORD maxlen = 0, curlen = 0;
                if (!locked2d) mb->Lock(&p, &maxlen, &curlen);
                if (p && pitch > 0) {
                    // Chroma plane starts after the allocated (coded) height.
                    UINT32 alloc_h = d_->height;
                    if (!locked2d && curlen && pitch) {
                        UINT32 rows = (UINT32)(curlen / (DWORD)pitch);  // = alloc_h * 1.5
                        if (rows >= d_->disp_h * 3 / 2) alloc_h = rows * 2 / 3;
                    }
                    Frame f;
                    f.width = (int)(d_->disp_w & ~1u);
                    f.height = (int)(d_->disp_h & ~1u);
                    f.y = p;
                    f.y_stride = pitch;
                    f.uv = p + (size_t)pitch * alloc_h;
                    f.uv_stride = pitch;
                    if (f.width > 0 && f.height > 0) on_frame(f);
                }
                if (locked2d) b2->Unlock2D(); else mb->Unlock();
                SafeRelease(b2);
                mb->Release();
            }
        }
        if (got && got != sample) got->Release();
        SafeRelease(sample);
    }
}

bool H264Decoder::Decode(const uint8_t* data, size_t size, int64_t pts_us,
                         const std::function<void(const Frame&)>& on_frame, std::string& err) {
    IMFMediaBuffer* buf = nullptr;
    if (FAILED(MFCreateMemoryBuffer((DWORD)size, &buf))) { err = TR("нет памяти"); return false; }
    BYTE* p = nullptr;
    buf->Lock(&p, nullptr, nullptr);
    memcpy(p, data, size);
    buf->Unlock();
    buf->SetCurrentLength((DWORD)size);
    IMFSample* s = nullptr;
    MFCreateSample(&s);
    s->AddBuffer(buf);
    buf->Release();
    s->SetSampleTime(pts_us * 10);

    HRESULT hr = d_->mft->ProcessInput(0, s, 0);
    if (hr == MF_E_NOTACCEPTING) {
        if (!Drain(on_frame, err)) { s->Release(); return false; }
        hr = d_->mft->ProcessInput(0, s, 0);
    }
    s->Release();
    if (FAILED(hr)) { err = "ProcessInput " + Hex(hr); return false; }
    return Drain(on_frame, err);
}

}  // namespace phonecam
