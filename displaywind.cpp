// 必须在所有 Windows/D3D11 头文件之前定义，否则 MinGW d3d11.h 不会定义视频处理接口
#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0602   // Windows 8：ID3D11VideoContext 等视频接口需要此版本
#elif _WIN32_WINNT < 0x0602
#  undef  _WIN32_WINNT
#  define _WIN32_WINNT 0x0602
#endif

#include "displaywind.h"
#include "ui_displaywind.h"
// FFmpeg/SDL 头文件只在 .cpp 中引入，避免污染头文件（SDL 会 #define main SDL_main）
#include "ff_ffplay_def.h"
#ifdef main
#  undef main   // SDL_main.h 重定义了 main，这里撤销，避免后续 Qt 代码受影响
#endif

// D3D11VA hwcontext — 提供 AVD3D11VADeviceContext / AVD3D11FrameDescriptor 等结构
extern "C" {
#include "libavutil/hwcontext_d3d11va.h"
}

// MinGW 5.3.0 的 d3d11.h 缺少 ID3D11VideoContext / D3D11_VIDEO_PROCESSOR_STREAM 等类型
// 此 compat header 按 Windows 8 SDK vtable 顺序补全这些接口（必须在 d3d11.h 之后包含）
#include "d3d11_video_compat.h"

#include "easylogging++.h"
#include <dxgi.h>

// OpenGL 3.0+ 常量（MinGW 系统 GL 头只含 OpenGL 1.1，需手动补全）
#ifndef GL_R8
#  define GL_R8   0x8229
#endif
#ifndef GL_R16
#  define GL_R16  0x822A
#endif
#ifndef GL_RG
#  define GL_RG   0x8227
#endif
#ifndef GL_RG8
#  define GL_RG8  0x822B
#endif
#ifndef GL_RG16
#  define GL_RG16 0x822C
#endif

// ---------------------------------------------------------------------------
// 顶点着色器：全屏四边形，翻转 t 轴使 OpenGL Y=0(底) 对齐视频 Y=0(顶)
// ---------------------------------------------------------------------------
static const char *kVertSrc = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
out vec2 vUV;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = aUV;
}
)";

// ---------------------------------------------------------------------------
// 片元着色器：
//   u_hw_mode == 1  → D3D11VA 路径：直接采样 VideoProcessor 输出的 BGRA 纹理
//   u_hw_mode == 0  → CPU 路径：p010le / NV12 双纹理 + BT.709 YUV→RGB
// ---------------------------------------------------------------------------
static const char *kFragSrc = R"(
#version 330 core
uniform sampler2D tex_y;
uniform sampler2D tex_uv;
uniform sampler2D tex_bgra;   // D3D11 VideoProcessor 输出（BGRA, D3D11VA 路径）
uniform int u_is_10bit;
uniform int u_hw_mode;        // 0=CPU YUV, 1=D3D11 BGRA
in  vec2 vUV;
out vec4 fragColor;
void main() {
    if (u_hw_mode == 1) {
        // D3D11 VideoProcessor 输出 DXGI_FORMAT_B8G8R8A8_UNORM
        // WGL 将其映射为 GL_BGRA，GLSL 采样时 .r=B, .g=G, .b=R，需交换 R/B
        vec4 c = texture(tex_bgra, vUV);
        fragColor = vec4(c.b, c.g, c.r, 1.0);
        return;
    }
    float y  = texture(tex_y,  vUV).r;
    vec2  uv = texture(tex_uv, vUV).rg;
    float ey, ecb, ecr;
    if (u_is_10bit == 1) {
        ey  = (y    - 0.06254) * 1.16845;
        ecb = (uv.r - 0.50001) * 1.14284;
        ecr = (uv.g - 0.50001) * 1.14284;
    } else {
        ey  = (y    - 0.06275) * 1.16438;
        ecb = (uv.r - 0.50196) * 1.13839;
        ecr = (uv.g - 0.50196) * 1.13839;
    }
    float r = ey + 1.5748 * ecr;
    float g = ey - 0.1873 * ecb - 0.4681 * ecr;
    float b = ey + 1.8556 * ecb;
    fragColor = vec4(clamp(r, 0.0, 1.0),
                     clamp(g, 0.0, 1.0),
                     clamp(b, 0.0, 1.0), 1.0);
}
)";

// ---------------------------------------------------------------------------
DisplayWind::DisplayWind(QWidget *parent)
    : QOpenGLWidget(parent), ui(new Ui::DisplayWind)
{
    ui->setupUi(this);
    play_state_ = 2;
}

DisplayWind::~DisplayWind()
{
    // QOpenGLWidget 析构时需要当前上下文才能删除 GL 资源
    makeCurrent();
    cleanupD3D11Interop();
    if (program_) { delete program_; program_ = nullptr; }
    if (tex_y_)   { glDeleteTextures(1, &tex_y_);   tex_y_  = 0; }
    if (tex_uv_)  { glDeleteTextures(1, &tex_uv_);  tex_uv_ = 0; }
    if (vao_)     { glDeleteVertexArrays(1, &vao_);  vao_ = 0; }
    if (vbo_)     { glDeleteBuffers(1, &vbo_);       vbo_ = 0; }
    doneCurrent();

    QMutexLocker lk(&mutex_);
    if (pending_hw_) { av_frame_free(&pending_hw_); }
    delete ui;
}

// ---------------------------------------------------------------------------
// Draw — 由 video_refresh_thread 调用：
//   D3D11VA 帧：若 WGL_NV_DX_interop 可用则走 GPU 路径（零拷贝）
//              否则回退 av_hwframe_transfer_data → CPU 路径
//   CPU 帧（NV12/P010LE）：memcpy 到 CPU 缓冲，触发 GL 刷新
// ---------------------------------------------------------------------------
int DisplayWind::Draw(const Frame *frame)
{
    if (!frame || !frame->frame) return -1;
    const AVFrame *avf = frame->frame;

    // ---- D3D11VA GPU 路径（仅当 WGL_NV_DX_interop 函数已成功加载时） ----
    if (avf->format == AV_PIX_FMT_D3D11 && fn_OpenDevice_) {
        QMutexLocker lk(&mutex_);
        // 释放上一帧引用，接管新帧
        if (pending_hw_) av_frame_free(&pending_hw_);
        pending_hw_ = av_frame_alloc();
        if (!pending_hw_) return -1;
        av_frame_ref(pending_hw_, avf);   // 增加引用，防止 FrameQueue 回收纹理
        buf_width_  = avf->width;
        buf_height_ = avf->height;
        hw_mode_    = true;
        has_frame_  = true;
        update();
        return 0;
    }

    // ---- D3D11VA CPU 回退（WGL 不可用时：hwframe_transfer → NV12/P010LE） ----
    AVFrame *sw_tmp = nullptr;
    if (avf->format == AV_PIX_FMT_D3D11) {
        sw_tmp = av_frame_alloc();
        if (!sw_tmp) return -1;
        if (av_hwframe_transfer_data(sw_tmp, avf, 0) < 0) {
            LOG(WARNING) << "[Draw] D3D11 hwframe_transfer fallback failed";
            av_frame_free(&sw_tmp);
            return -1;
        }
        av_frame_copy_props(sw_tmp, avf);
        avf = sw_tmp;   // 后续统一走 CPU 路径
    }

    // ---- CPU 路径（NV12 / P010LE / DXVA2 回退） ----
    if (avf->format != AV_PIX_FMT_NV12 &&
        avf->format != AV_PIX_FMT_P010LE) {
        if (sw_tmp) av_frame_free(&sw_tmp);
        return -1;
    }

    const bool is10bit = (avf->format == AV_PIX_FMT_P010LE);
    const int  y_bytes  = avf->linesize[0] * avf->height;
    const int  uv_bytes = avf->linesize[1] * (avf->height / 2);

    {
        QMutexLocker lk(&mutex_);
        y_buf_.resize(y_bytes);
        uv_buf_.resize(uv_bytes);
        memcpy(y_buf_.data(),  avf->data[0], y_bytes);
        memcpy(uv_buf_.data(), avf->data[1], uv_bytes);
        buf_y_ls_   = avf->linesize[0];
        buf_uv_ls_  = avf->linesize[1];
        buf_width_  = avf->width;
        buf_height_ = avf->height;
        buf_10bit_  = is10bit;
        hw_mode_    = false;
        has_frame_  = true;
    }

    if (sw_tmp) av_frame_free(&sw_tmp);
    update();
    return 0;
}

void DisplayWind::DeInit()
{
    QMutexLocker lk(&mutex_);
    y_buf_.clear();
    uv_buf_.clear();
    if (pending_hw_) { av_frame_free(&pending_hw_); }
    has_frame_ = false;
    hw_mode_   = false;
}

void DisplayWind::StartPlay()
{
    play_state_ = 1;
}

void DisplayWind::StopPlay()
{
    play_state_ = 2;
    update();
}

// ---------------------------------------------------------------------------
// initializeGL — GL 上下文首次就绪时由 Qt 调用（UI 线程）
// ---------------------------------------------------------------------------
void DisplayWind::initializeGL()
{
    initializeOpenGLFunctions();
    glClearColor(0.f, 0.f, 0.f, 1.f);

    program_ = new QOpenGLShaderProgram(this);
    program_->addShaderFromSourceCode(QOpenGLShader::Vertex,   kVertSrc);
    program_->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragSrc);
    program_->link();

    loc_tex_y_    = program_->uniformLocation("tex_y");
    loc_tex_uv_   = program_->uniformLocation("tex_uv");
    loc_is_10bit_ = program_->uniformLocation("u_is_10bit");
    loc_hw_mode_  = program_->uniformLocation("u_hw_mode");
    loc_tex_bgra_ = program_->uniformLocation("tex_bgra");

    // 全屏四边形：NDC(-1~1) + 纹理坐标(0~1)，t 轴翻转对齐视频行序
    static const float kQuad[] = {
        -1.f, -1.f,  0.f, 1.f,
         1.f, -1.f,  1.f, 1.f,
        -1.f,  1.f,  0.f, 0.f,
         1.f,  1.f,  1.f, 0.f,
    };

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    GLuint texs[2];
    glGenTextures(2, texs);
    tex_y_  = texs[0];
    tex_uv_ = texs[1];
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, texs[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    // 加载 WGL_NV_DX_interop 函数指针（有扩展则 D3D11 路径可用）
    loadWGLFuncs();
}

void DisplayWind::resizeGL(int /*w*/, int /*h*/)
{
    // viewport 在 paintGL 中按宽高比动态设置
}

// ---------------------------------------------------------------------------
// paintGL — 每帧由 Qt 调用（UI 线程）
// ---------------------------------------------------------------------------
void DisplayWind::paintGL()
{
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (play_state_ != 1) return;

    QMutexLocker lk(&mutex_);
    if (!has_frame_) return;

    // ---- D3D11VA GPU 路径 ----
    if (hw_mode_ && pending_hw_) {
        paintHwFrame();
        return;
    }

    // ---- CPU 路径 ----
    if (y_buf_.empty()) return;

    const int  fw  = buf_width_;
    const int  fh  = buf_height_;
    const bool b10 = buf_10bit_;

    // 保持宽高比的 viewport
    const float vr = static_cast<float>(fw) / fh;
    const float wr = static_cast<float>(width()) / height();
    int vp_x, vp_y, vp_w, vp_h;
    if (wr > vr) {
        vp_h = height(); vp_w = static_cast<int>(vr * vp_h);
        vp_x = (width() - vp_w) / 2; vp_y = 0;
    } else {
        vp_w = width(); vp_h = static_cast<int>(vp_w / vr);
        vp_x = 0; vp_y = (height() - vp_h) / 2;
    }
    glViewport(vp_x, vp_y, vp_w, vp_h);

    GLenum int_y, int_uv, fmt_y, fmt_uv, gl_type;
    int    bpp_y, bpp_uv;
    if (b10) {
        int_y = GL_R16;  int_uv = GL_RG16;
        fmt_y = GL_RED;  fmt_uv = GL_RG;
        gl_type = GL_UNSIGNED_SHORT;
        bpp_y = 2;  bpp_uv = 4;
    } else {
        int_y = GL_R8;   int_uv = GL_RG8;
        fmt_y = GL_RED;  fmt_uv = GL_RG;
        gl_type = GL_UNSIGNED_BYTE;
        bpp_y = 1;  bpp_uv = 2;
    }

    const bool size_changed = (tex_width_ != fw || tex_height_ != fh || tex_is_10bit_ != b10);

    glBindTexture(GL_TEXTURE_2D, tex_y_);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, buf_y_ls_ / bpp_y);
    if (size_changed)
        glTexImage2D(GL_TEXTURE_2D, 0, int_y, fw, fh, 0, fmt_y, gl_type, y_buf_.data());
    else
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh, fmt_y, gl_type, y_buf_.data());

    glBindTexture(GL_TEXTURE_2D, tex_uv_);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, buf_uv_ls_ / bpp_uv);
    if (size_changed)
        glTexImage2D(GL_TEXTURE_2D, 0, int_uv, fw/2, fh/2, 0, fmt_uv, gl_type, uv_buf_.data());
    else
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw/2, fh/2, fmt_uv, gl_type, uv_buf_.data());

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    if (size_changed) {
        tex_width_ = fw; tex_height_ = fh; tex_is_10bit_ = b10;
    }

    program_->bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_y_);
    program_->setUniformValue(loc_tex_y_, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_uv_);
    program_->setUniformValue(loc_tex_uv_, 1);

    program_->setUniformValue(loc_is_10bit_, b10 ? 1 : 0);
    program_->setUniformValue(loc_hw_mode_,  0);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    program_->release();
}

// ---------------------------------------------------------------------------
// paintHwFrame — D3D11VA 路径（在 paintGL 内调用，mutex_ 已持有）
// ---------------------------------------------------------------------------
void DisplayWind::paintHwFrame()
{
    const AVFrame *avf = pending_hw_;
    const int fw = avf->width;
    const int fh = avf->height;

    LOG(INFO) << "[HW] paintHwFrame enter " << fw << "x" << fh
              << " d3d_dev=" << (void*)d3d_dev_
              << " wgl_dev=" << (void*)wgl_dev_;

    // 首次或尺寸变化时初始化 D3D11 + VideoProcessor
    if (!d3d_dev_) {
        if (!initD3D11Interop(avf)) {
            LOG(WARNING) << "[HW] D3D11 interop init failed, disabling GPU path";
            // 禁用 GPU 路径，后续帧将在 Draw() 中走 CPU 回退
            fn_OpenDevice_ = nullptr;
            av_frame_free(&pending_hw_);
            hw_mode_   = false;
            has_frame_ = false;
            return;
        }
    }
    if (fw != vp_w_ || fh != vp_h_) {
        if (!setupVideoProcessor(fw, fh)) {
            LOG(WARNING) << "[HW] VideoProcessor setup failed";
            av_frame_free(&pending_hw_);
            return;
        }
    }

    // ---- 使用 D3D11 VideoProcessor 转换 P010/NV12 → BGRA（全 GPU 操作） ----
    ID3D11Texture2D *src_tex   = reinterpret_cast<ID3D11Texture2D*>(avf->data[0]);
    UINT             src_slice = static_cast<UINT>(reinterpret_cast<intptr_t>(avf->data[1]));

    // 更新 VideoProcessorInputView（仅在纹理或 slice 变化时重建）
    if (src_tex != vp_inv_tex_ || src_slice != vp_inv_slice_) {
        if (vp_inv_) { vp_inv_->Release(); vp_inv_ = nullptr; }

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
        ivd.FourCC = 0;
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.MipSlice   = 0;
        ivd.Texture2D.ArraySlice = src_slice;
        if (FAILED(d3d_vdev_->CreateVideoProcessorInputView(
                src_tex, vp_enum_, &ivd, &vp_inv_))) {
            LOG(WARNING) << "[HW] CreateVideoProcessorInputView failed";
            av_frame_free(&pending_hw_);
            return;
        }
        vp_inv_tex_   = src_tex;
        vp_inv_slice_ = src_slice;
    }

    // 执行 GPU 端颜色转换
    if (d3d_lock_) d3d_lock_(d3d_lock_ctx_);
    HRESULT blt_hr;
    {
        D3D11_VIDEO_PROCESSOR_STREAM stream = {};
        stream.Enable        = TRUE;
        stream.pInputSurface = vp_inv_;
        blt_hr = d3d_vctx_->VideoProcessorBlt(vp_, vp_outv_, 0, 1, &stream);
    }
    if (d3d_unlock_) d3d_unlock_(d3d_lock_ctx_);
    if (FAILED(blt_hr)) {
        LOG(WARNING) << "[HW] VideoProcessorBlt failed hr=" << (long)blt_hr;
        av_frame_free(&pending_hw_);
        return;
    }

    // 释放 hw frame 引用（纹理数据已由 D3D11 复制到输出）
    av_frame_free(&pending_hw_);

    // ---- WGL interop：锁定后用 OpenGL 渲染 BGRA 纹理 ----
    BOOL lock_ok = fn_Lock_(wgl_dev_, 1, &wgl_tex_);
    if (!lock_ok) {
        LOG(WARNING) << "[HW] wglDXLockObjectsNV failed error=" << GetLastError();
        return;
    }

    // 保持宽高比的 viewport
    const float vr = static_cast<float>(fw) / fh;
    const float wr = static_cast<float>(width()) / height();
    int vp_x, vp_y, vp_w, vp_h;
    if (wr > vr) {
        vp_h = height(); vp_w = static_cast<int>(vr * vp_h);
        vp_x = (width() - vp_w) / 2; vp_y = 0;
    } else {
        vp_w = width(); vp_h = static_cast<int>(vp_w / vr);
        vp_x = 0; vp_y = (height() - vp_h) / 2;
    }
    glViewport(vp_x, vp_y, vp_w, vp_h);

    program_->bind();
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, tex_bgra_);
    program_->setUniformValue(loc_tex_bgra_, 2);
    program_->setUniformValue(loc_hw_mode_,  1);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    program_->release();

    fn_Unlock_(wgl_dev_, 1, &wgl_tex_);
}

// ---------------------------------------------------------------------------
// loadWGLFuncs — 加载 WGL_NV_DX_interop 函数指针（initializeGL 中调用）
// ---------------------------------------------------------------------------
bool DisplayWind::loadWGLFuncs()
{
    HMODULE hGL = GetModuleHandleA("opengl32.dll");
    if (!hGL) return false;

    auto getProcAddr = reinterpret_cast<PROC(WINAPI*)(LPCSTR)>(
        GetProcAddress(hGL, "wglGetProcAddress"));
    if (!getProcAddr) return false;

    fn_OpenDevice_  = reinterpret_cast<PFN_wglDXOpenDeviceNV>     (getProcAddr("wglDXOpenDeviceNV"));
    fn_CloseDevice_ = reinterpret_cast<PFN_wglDXCloseDeviceNV>    (getProcAddr("wglDXCloseDeviceNV"));
    fn_Register_    = reinterpret_cast<PFN_wglDXRegisterObjectNV> (getProcAddr("wglDXRegisterObjectNV"));
    fn_Unregister_  = reinterpret_cast<PFN_wglDXUnregisterObjectNV>(getProcAddr("wglDXUnregisterObjectNV"));
    fn_Lock_        = reinterpret_cast<PFN_wglDXLockObjectsNV>    (getProcAddr("wglDXLockObjectsNV"));
    fn_Unlock_      = reinterpret_cast<PFN_wglDXUnlockObjectsNV>  (getProcAddr("wglDXUnlockObjectsNV"));

    bool ok = fn_OpenDevice_ && fn_CloseDevice_ && fn_Register_ &&
              fn_Unregister_ && fn_Lock_ && fn_Unlock_;
    if (ok)
        LOG(INFO) << "[HW] WGL_NV_DX_interop functions loaded";
    else
        LOG(WARNING) << "[HW] WGL_NV_DX_interop not available — GPU-direct path disabled";
    return ok;
}

// ---------------------------------------------------------------------------
// initD3D11Interop — 首帧到达时从 hw_frames_ctx 中提取 D3D11 设备并注册 WGL
// ---------------------------------------------------------------------------
bool DisplayWind::initD3D11Interop(const AVFrame *hw_frame)
{
    if (!fn_OpenDevice_) {
        LOG(WARNING) << "[HW] WGL interop funcs not loaded";
        return false;
    }
    if (!hw_frame->hw_frames_ctx) {
        LOG(WARNING) << "[HW] hw_frames_ctx is null — cannot get D3D11 device";
        return false;
    }

    auto *fctx    = reinterpret_cast<AVHWFramesContext*>(hw_frame->hw_frames_ctx->data);
    auto *dctx    = reinterpret_cast<AVHWDeviceContext*>(fctx->device_ctx);
    auto *avd3d11 = reinterpret_cast<AVD3D11VADeviceContext*>(dctx->hwctx);

    d3d_dev_      = avd3d11->device;
    d3d_devctx_   = avd3d11->device_context;
    d3d_vdev_     = avd3d11->video_device;
    d3d_vctx_     = avd3d11->video_context;
    d3d_lock_     = avd3d11->lock;
    d3d_unlock_   = avd3d11->unlock;
    d3d_lock_ctx_ = avd3d11->lock_ctx;

    // 保持引用，防止 FFmpeg 设备提前释放
    d3d_dev_->AddRef();
    d3d_devctx_->AddRef();
    d3d_vdev_->AddRef();
    d3d_vctx_->AddRef();

    // 向 WGL 注册 D3D11 设备
    wgl_dev_ = fn_OpenDevice_(d3d_dev_);
    if (!wgl_dev_) {
        LOG(WARNING) << "[HW] wglDXOpenDeviceNV failed (error=" << GetLastError() << ")";
        cleanupD3D11Interop();
        return false;
    }
    LOG(INFO) << "[HW] D3D11 device registered with WGL";
    return true;
}

// ---------------------------------------------------------------------------
// setupVideoProcessor — 创建 VideoProcessor 及输出 BGRA 纹理
// ---------------------------------------------------------------------------
bool DisplayWind::setupVideoProcessor(int w, int h)
{
    // 释放旧资源
    if (wgl_tex_)  { fn_Unregister_(wgl_dev_, wgl_tex_); wgl_tex_ = nullptr; }
    if (tex_bgra_) { glDeleteTextures(1, &tex_bgra_); tex_bgra_ = 0; }
    if (vp_inv_)   { vp_inv_->Release();  vp_inv_  = nullptr; }
    if (vp_outv_)  { vp_outv_->Release(); vp_outv_ = nullptr; }
    if (vp_out_)   { vp_out_->Release();  vp_out_  = nullptr; }
    if (vp_)       { vp_->Release();      vp_      = nullptr; }
    if (vp_enum_)  { vp_enum_->Release(); vp_enum_ = nullptr; }
    vp_inv_tex_ = nullptr; vp_inv_slice_ = 0;

    // 创建 VideoProcessorEnumerator（描述源/目标规格）
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC vpcd = {};
    vpcd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    vpcd.InputWidth  = static_cast<UINT>(w);
    vpcd.InputHeight = static_cast<UINT>(h);
    vpcd.OutputWidth  = static_cast<UINT>(w);
    vpcd.OutputHeight = static_cast<UINT>(h);
    vpcd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(d3d_vdev_->CreateVideoProcessorEnumerator(&vpcd, &vp_enum_))) {
        LOG(WARNING) << "[HW] CreateVideoProcessorEnumerator failed";
        return false;
    }

    // 创建 VideoProcessor
    if (FAILED(d3d_vdev_->CreateVideoProcessor(vp_enum_, 0, &vp_))) {
        LOG(WARNING) << "[HW] CreateVideoProcessor failed";
        return false;
    }

    // 设置色彩空间：BT.709 limited range 输入 → full range RGB 输出
    if (d3d_lock_) d3d_lock_(d3d_lock_ctx_);
    {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs = {};
        cs.Usage         = 0;   // 0 = playback
        cs.RGB_Range     = 1;   // 1 = limited (16-235)
        cs.YCbCr_Matrix  = 1;   // 1 = BT.709
        cs.YCbCr_xvYCC   = 0;
        d3d_vctx_->VideoProcessorSetStreamColorSpace(vp_, 0, &cs);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs = {};
        out_cs.RGB_Range = 0;   // 0 = full range (0-255)
        d3d_vctx_->VideoProcessorSetOutputColorSpace(vp_, &out_cs);
    }
    if (d3d_unlock_) d3d_unlock_(d3d_lock_ctx_);

    // 创建输出 BGRA 纹理（用于 WGL 注册）
    D3D11_TEXTURE2D_DESC td = {};
    td.Width  = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage     = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;  // WGL interop 需要 SHADER_RESOURCE
    td.MiscFlags = 0;
    if (FAILED(d3d_dev_->CreateTexture2D(&td, nullptr, &vp_out_))) {
        LOG(WARNING) << "[HW] CreateTexture2D (BGRA output) failed";
        return false;
    }

    // 创建 VideoProcessorOutputView
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovd.Texture2D.MipSlice = 0;
    if (FAILED(d3d_vdev_->CreateVideoProcessorOutputView(
            vp_out_, vp_enum_, &ovd, &vp_outv_))) {
        LOG(WARNING) << "[HW] CreateVideoProcessorOutputView failed";
        return false;
    }

    // 创建 OpenGL 纹理并通过 WGL 与 D3D11 纹理关联
    glGenTextures(1, &tex_bgra_);
    glBindTexture(GL_TEXTURE_2D, tex_bgra_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    wgl_tex_ = fn_Register_(wgl_dev_, vp_out_, tex_bgra_,
                             GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
    if (!wgl_tex_) {
        LOG(WARNING) << "[HW] wglDXRegisterObjectNV failed (error=" << GetLastError() << ")";
        return false;
    }

    vp_w_ = w;
    vp_h_ = h;
    LOG(INFO) << "[HW] VideoProcessor + WGL interop ready: " << w << "x" << h;
    return true;
}

// ---------------------------------------------------------------------------
// cleanupD3D11Interop — 释放所有 D3D11 / WGL 资源
// ---------------------------------------------------------------------------
void DisplayWind::cleanupD3D11Interop()
{
    if (wgl_tex_ && wgl_dev_) {
        fn_Unregister_(wgl_dev_, wgl_tex_);
        wgl_tex_ = nullptr;
    }
    if (tex_bgra_) { glDeleteTextures(1, &tex_bgra_); tex_bgra_ = 0; }

    if (vp_inv_)  { vp_inv_->Release();  vp_inv_  = nullptr; }
    if (vp_outv_) { vp_outv_->Release(); vp_outv_ = nullptr; }
    if (vp_out_)  { vp_out_->Release();  vp_out_  = nullptr; }
    if (vp_)      { vp_->Release();      vp_      = nullptr; }
    if (vp_enum_) { vp_enum_->Release(); vp_enum_ = nullptr; }

    if (wgl_dev_ && fn_CloseDevice_) {
        fn_CloseDevice_(wgl_dev_);
        wgl_dev_ = nullptr;
    }

    if (d3d_vctx_)   { d3d_vctx_->Release();   d3d_vctx_   = nullptr; }
    if (d3d_vdev_)   { d3d_vdev_->Release();    d3d_vdev_   = nullptr; }
    if (d3d_devctx_) { d3d_devctx_->Release();  d3d_devctx_ = nullptr; }
    if (d3d_dev_)    { d3d_dev_->Release();      d3d_dev_    = nullptr; }
}
