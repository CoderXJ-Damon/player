#include "displaywind.h"
#include "ui_displaywind.h"
// FFmpeg/SDL 头文件只在 .cpp 中引入，避免污染头文件（SDL 会 #define main SDL_main）
#include "ff_ffplay_def.h"
#ifdef main
#  undef main   // SDL_main.h 重定义了 main，这里撤销，避免后续 Qt 代码受影响
#endif

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
// 片元着色器：p010le(10bit) / NV12(8bit) → BT.709 limited-range → RGB
// p010le: 10-bit 值左移6位存入16-bit，GL_R16 归一化后：
//   Y  ∈ [64*64/65535, 940*64/65535] ≈ [0.06254, 0.91802]
//   UV ∈ [64*64/65535, 960*64/65535] ≈ [0.06254, 0.93750]，中心 ≈ 0.50001
// NV12: 8-bit，GL_R8 归一化后：
//   Y  ∈ [16/255, 235/255] ≈ [0.06275, 0.92157]
//   UV ∈ [16/255, 240/255] ≈ [0.06275, 0.94118]，中心 = 128/255 ≈ 0.50196
// ---------------------------------------------------------------------------
static const char *kFragSrc = R"(
#version 330 core
uniform sampler2D tex_y;
uniform sampler2D tex_uv;
uniform int u_is_10bit;
in  vec2 vUV;
out vec4 fragColor;
void main() {
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
    if (program_) { delete program_; program_ = nullptr; }
    if (tex_y_)  { glDeleteTextures(1, &tex_y_);   tex_y_  = 0; }
    if (tex_uv_) { glDeleteTextures(1, &tex_uv_);  tex_uv_ = 0; }
    if (vao_)    { glDeleteVertexArrays(1, &vao_);  vao_ = 0; }
    if (vbo_)    { glDeleteBuffers(1, &vbo_);       vbo_ = 0; }
    doneCurrent();
    delete ui;
}

// ---------------------------------------------------------------------------
// Draw — 由解码线程调用：复制帧数据到 CPU 缓冲，触发 GL 刷新
// ---------------------------------------------------------------------------
int DisplayWind::Draw(const Frame *frame)
{
    if (!frame || !frame->frame) return -1;
    const AVFrame *avf = frame->frame;

    if (avf->format != AV_PIX_FMT_NV12 &&
        avf->format != AV_PIX_FMT_P010LE) {
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
        has_frame_  = true;
    }

    update();   // 线程安全，触发 paintGL()
    return 0;
}

void DisplayWind::DeInit()
{
    QMutexLocker lk(&mutex_);
    y_buf_.clear();
    uv_buf_.clear();
    has_frame_ = false;
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

    // 在锁保护下直接操作缓冲区，避免额外 24MB 拷贝
    QMutexLocker lk(&mutex_);
    if (!has_frame_ || y_buf_.empty()) return;

    const int  fw   = buf_width_;
    const int  fh   = buf_height_;
    const bool b10  = buf_10bit_;

    // ---------- 保持宽高比的 viewport ----------
    const float vr = static_cast<float>(fw) / fh;
    const float wr = static_cast<float>(width()) / height();
    int vp_x, vp_y, vp_w, vp_h;
    if (wr > vr) {
        vp_h = height();
        vp_w = static_cast<int>(vr * vp_h);
        vp_x = (width()  - vp_w) / 2;
        vp_y = 0;
    } else {
        vp_w = width();
        vp_h = static_cast<int>(vp_w / vr);
        vp_x = 0;
        vp_y = (height() - vp_h) / 2;
    }
    glViewport(vp_x, vp_y, vp_w, vp_h);

    // ---------- 纹理格式 ----------
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

    const bool size_changed =
        (tex_width_ != fw || tex_height_ != fh || tex_is_10bit_ != b10);

    // ---------- 上传 Y 纹理 ----------
    glBindTexture(GL_TEXTURE_2D, tex_y_);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, buf_y_ls_ / bpp_y);
    if (size_changed)
        glTexImage2D(GL_TEXTURE_2D, 0, int_y,
                     fw, fh, 0, fmt_y, gl_type, y_buf_.data());
    else
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        fw, fh, fmt_y, gl_type, y_buf_.data());

    // ---------- 上传 UV 纹理 ----------
    glBindTexture(GL_TEXTURE_2D, tex_uv_);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, buf_uv_ls_ / bpp_uv);
    if (size_changed)
        glTexImage2D(GL_TEXTURE_2D, 0, int_uv,
                     fw/2, fh/2, 0, fmt_uv, gl_type, uv_buf_.data());
    else
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        fw/2, fh/2, fmt_uv, gl_type, uv_buf_.data());

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    if (size_changed) {
        tex_width_    = fw;
        tex_height_   = fh;
        tex_is_10bit_ = b10;
    }

    // ---------- 渲染四边形 ----------
    program_->bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_y_);
    program_->setUniformValue(loc_tex_y_, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_uv_);
    program_->setUniformValue(loc_tex_uv_, 1);

    program_->setUniformValue(loc_is_10bit_, b10 ? 1 : 0);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    program_->release();
}
