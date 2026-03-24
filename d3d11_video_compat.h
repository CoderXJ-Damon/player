// d3d11_video_compat.h
// Defines D3D11 video processing interfaces missing from MinGW 5.3.0 (Qt 5.10.1).
// ID3D11VideoContext vtable order matches Windows 8 SDK d3d11.h exactly.
// Must be included AFTER d3d11.h (i.e., after hwcontext_d3d11va.h).

#pragma once
#ifndef D3D11_VIDEO_COMPAT_H
#define D3D11_VIDEO_COMPAT_H

#ifdef __cplusplus

// ---- D3D11_VIDEO_PROCESSOR_COLOR_SPACE ----
#ifndef __D3D11_VIDEO_PROCESSOR_COLOR_SPACE_DEFINED__
#define __D3D11_VIDEO_PROCESSOR_COLOR_SPACE_DEFINED__
typedef struct D3D11_VIDEO_PROCESSOR_COLOR_SPACE {
    UINT Usage         : 1;   // 0=playback, 1=processing
    UINT RGB_Range     : 1;   // 0=full (0-255), 1=limited (16-235)
    UINT YCbCr_Matrix  : 1;   // 0=BT.601, 1=BT.709
    UINT YCbCr_xvYCC   : 1;
    UINT Nominal_Range : 2;
    UINT Reserved      : 26;
} D3D11_VIDEO_PROCESSOR_COLOR_SPACE;
#endif

// ---- D3D11_VIDEO_PROCESSOR_STREAM ----
#ifndef __D3D11_VIDEO_PROCESSOR_STREAM_DEFINED__
#define __D3D11_VIDEO_PROCESSOR_STREAM_DEFINED__
typedef struct D3D11_VIDEO_PROCESSOR_STREAM {
    BOOL                            Enable;
    UINT                            OutputIndex;
    UINT                            InputFrameOrField;
    UINT                            PastFrames;
    UINT                            FutureFrames;
    ID3D11VideoProcessorInputView **ppPastSurfaces;
    ID3D11VideoProcessorInputView  *pInputSurface;
    ID3D11VideoProcessorInputView **ppFutureSurfaces;
    ID3D11VideoProcessorInputView **ppPastSurfacesRight;
    ID3D11VideoProcessorInputView  *pInputSurfaceRight;
    ID3D11VideoProcessorInputView **ppFutureSurfacesRight;
} D3D11_VIDEO_PROCESSOR_STREAM;
#endif

// ---- ID3D11VideoContext ----
// Extends ID3D11DeviceChild (→ IUnknown).
// Virtual method order matches Windows 8 SDK d3d11.h exactly.
// Stub methods (never called by us) use minimal valid C++ signatures.
#ifndef __ID3D11VideoContext_FWD_DEFINED__
// If only a forward declaration exists (our displaywind.h forward-declared it),
// we complete the type here.
#endif

#ifndef __ID3D11VideoContext_INTERFACE_DEFINED__
#define __ID3D11VideoContext_INTERFACE_DEFINED__

struct ID3D11VideoContext : public ID3D11DeviceChild {
    // -- Decoder stubs (slots 7-12, never called) --
    virtual HRESULT STDMETHODCALLTYPE GetDecoderBuffer(
        void*, void*, UINT*, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReleaseDecoderBuffer(
        void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE DecoderBeginFrame(
        void*, void*, UINT, const void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE DecoderEndFrame(
        void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SubmitDecoderBuffers(
        void*, UINT, const void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE DecoderExtension(
        void*, const void*) = 0;

    // -- Output color / rect / misc (slots 13-26) --
    virtual void STDMETHODCALLTYPE VideoProcessorSetOutputTargetRect(
        ID3D11VideoProcessor*, BOOL, const RECT*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorSetOutputBackgroundColor(
        ID3D11VideoProcessor*, BOOL, const void*) = 0;

    // slot 15 — WE USE THIS
    virtual void STDMETHODCALLTYPE VideoProcessorSetOutputColorSpace(
        ID3D11VideoProcessor *pVideoProcessor,
        const D3D11_VIDEO_PROCESSOR_COLOR_SPACE *pColorSpace) = 0;

    virtual void STDMETHODCALLTYPE VideoProcessorSetOutputAlphaFillMode(
        ID3D11VideoProcessor*, UINT, UINT) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorSetOutputConstriction(
        ID3D11VideoProcessor*, BOOL, SIZE) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorSetOutputStereoMode(
        ID3D11VideoProcessor*, BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE VideoProcessorSetOutputExtension(
        ID3D11VideoProcessor*, const GUID*, UINT, void*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorGetOutputTargetRect(
        ID3D11VideoProcessor*, BOOL*, RECT*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorGetOutputBackgroundColor(
        ID3D11VideoProcessor*, BOOL*, void*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorGetOutputColorSpace(
        ID3D11VideoProcessor*, D3D11_VIDEO_PROCESSOR_COLOR_SPACE*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorGetOutputAlphaFillMode(
        ID3D11VideoProcessor*, UINT*, UINT*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorGetOutputConstriction(
        ID3D11VideoProcessor*, BOOL*, SIZE*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorGetOutputStereoMode(
        ID3D11VideoProcessor*, BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE VideoProcessorGetOutputExtension(
        ID3D11VideoProcessor*, const GUID*, UINT, void*) = 0;

    // -- Stream format / color (slots 27-28) --
    virtual void STDMETHODCALLTYPE VideoProcessorSetStreamFrameFormat(
        ID3D11VideoProcessor*, UINT, UINT) = 0;

    // slot 28 — WE USE THIS
    virtual void STDMETHODCALLTYPE VideoProcessorSetStreamColorSpace(
        ID3D11VideoProcessor *pVideoProcessor,
        UINT StreamIndex,
        const D3D11_VIDEO_PROCESSOR_COLOR_SPACE *pColorSpace) = 0;

    // -- Stream misc stubs (slots 29-52) --
    virtual void STDMETHODCALLTYPE VideoProcessorSetStreamOutputRate(
        ID3D11VideoProcessor*, UINT, UINT, BOOL, const void*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorSetStreamSourceRect(
        ID3D11VideoProcessor*, UINT, BOOL, const RECT*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorSetStreamDestRect(
        ID3D11VideoProcessor*, UINT, BOOL, const RECT*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorSetStreamAlpha(
        ID3D11VideoProcessor*, UINT, BOOL, float) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorSetStreamPalette(
        ID3D11VideoProcessor*, UINT, UINT, const UINT*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorSetStreamPixelAspectRatio(
        ID3D11VideoProcessor*, UINT, BOOL, const void*, const void*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorSetStreamLumaKey(
        ID3D11VideoProcessor*, UINT, BOOL, float, float) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorSetStreamStereoFormat(
        ID3D11VideoProcessor*, UINT, BOOL, UINT, BOOL, BOOL, UINT, int) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorSetStreamAutoProcessingMode(
        ID3D11VideoProcessor*, UINT, BOOL) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorSetStreamFilter(
        ID3D11VideoProcessor*, UINT, UINT, BOOL, int) = 0;
    virtual HRESULT STDMETHODCALLTYPE VideoProcessorSetStreamExtension(
        ID3D11VideoProcessor*, UINT, const GUID*, UINT, void*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorGetStreamFrameFormat(
        ID3D11VideoProcessor*, UINT, UINT*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorGetStreamColorSpace(
        ID3D11VideoProcessor*, UINT, D3D11_VIDEO_PROCESSOR_COLOR_SPACE*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorGetStreamOutputRate(
        ID3D11VideoProcessor*, UINT, UINT*, BOOL*, void*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorGetStreamSourceRect(
        ID3D11VideoProcessor*, UINT, BOOL*, RECT*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorGetStreamDestRect(
        ID3D11VideoProcessor*, UINT, BOOL*, RECT*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorGetStreamAlpha(
        ID3D11VideoProcessor*, UINT, BOOL*, float*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorGetStreamPalette(
        ID3D11VideoProcessor*, UINT, UINT, UINT*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorGetStreamPixelAspectRatio(
        ID3D11VideoProcessor*, UINT, BOOL*, void*, void*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorGetStreamLumaKey(
        ID3D11VideoProcessor*, UINT, BOOL*, float*, float*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorGetStreamStereoFormat(
        ID3D11VideoProcessor*, UINT, BOOL*, UINT*, BOOL*, BOOL*, UINT*, int*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorGetStreamAutoProcessingMode(
        ID3D11VideoProcessor*, UINT, BOOL*) = 0;
    virtual void STDMETHODCALLTYPE VideoProcessorGetStreamFilter(
        ID3D11VideoProcessor*, UINT, UINT, BOOL*, int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE VideoProcessorGetStreamExtension(
        ID3D11VideoProcessor*, UINT, const GUID*, UINT, void*) = 0;

    // slot 53 — WE USE THIS
    virtual HRESULT STDMETHODCALLTYPE VideoProcessorBlt(
        ID3D11VideoProcessor           *pVideoProcessor,
        ID3D11VideoProcessorOutputView *pView,
        UINT                            OutputFrame,
        UINT                            StreamCount,
        const D3D11_VIDEO_PROCESSOR_STREAM *pStreams) = 0;

    // -- Remaining stubs (slots 54-57) --
    virtual HRESULT STDMETHODCALLTYPE NegotiateProcessorOutputDesc(
        void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE NegotiateDecoderOutputDesc(
        void*, void*, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE VideoDecoderGetHandle(
        void*, HANDLE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE VideoProcessorGetBehaviorHints(
        void*, UINT, UINT, void*, UINT*) = 0;
};

#endif // __ID3D11VideoContext_INTERFACE_DEFINED__

#endif // __cplusplus
#endif // D3D11_VIDEO_COMPAT_H
