#ifndef DISPLAYWIND_H
#define DISPLAYWIND_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QMutex>
#include <vector>

// 仅需 HANDLE / BOOL / UINT / WINAPI / PROC — windows.h 不引入 d3d11.h
#include <windows.h>

// D3D11 接口只用作指针，前向声明即可（避免各 TU 对 _WIN32_WINNT 的依赖）
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11VideoDevice;
struct ID3D11VideoContext;
struct ID3D11VideoProcessorEnumerator;
struct ID3D11VideoProcessor;
struct ID3D11Texture2D;
struct ID3D11VideoProcessorOutputView;
struct ID3D11VideoProcessorInputView;

// WGL_NV_DX_interop 函数指针类型（HANDLE / GLuint / GLenum 均已定义）
#ifndef WGL_ACCESS_READ_ONLY_NV
#  define WGL_ACCESS_READ_ONLY_NV  0x0000
#endif
typedef HANDLE (WINAPI *PFN_wglDXOpenDeviceNV)      (void *dxDevice);
typedef BOOL   (WINAPI *PFN_wglDXCloseDeviceNV)     (HANDLE hDevice);
typedef HANDLE (WINAPI *PFN_wglDXRegisterObjectNV)  (HANDLE hDevice, void *dxObj,
                                                      GLuint name, GLenum type, GLenum access);
typedef BOOL   (WINAPI *PFN_wglDXUnregisterObjectNV)(HANDLE hDevice, HANDLE hObject);
typedef BOOL   (WINAPI *PFN_wglDXLockObjectsNV)     (HANDLE hDevice, GLint count, HANDLE *objs);
typedef BOOL   (WINAPI *PFN_wglDXUnlockObjectsNV)   (HANDLE hDevice, GLint count, HANDLE *objs);

// AVFrame — 只作指针，前向声明
struct AVFrame;

// Frame — 只作指针，前向声明
struct Frame;

namespace Ui {
class DisplayWind;
}

class DisplayWind : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core
{
    Q_OBJECT

public:
    explicit DisplayWind(QWidget *parent = nullptr);
    ~DisplayWind();
    int Draw(const Frame *frame);
    void DeInit();
    void StartPlay();
    void StopPlay();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    Ui::DisplayWind *ui;

    // ---------- OpenGL resources (CPU path: NV12/P010LE via glTexSubImage2D) ----------
    QOpenGLShaderProgram *program_ = nullptr;
    GLuint tex_y_  = 0;
    GLuint tex_uv_ = 0;
    GLuint vao_    = 0;
    GLuint vbo_    = 0;

    int loc_tex_y_    = -1;
    int loc_tex_uv_   = -1;
    int loc_is_10bit_ = -1;
    int loc_hw_mode_  = -1;
    int loc_tex_bgra_ = -1;

    int  tex_width_    = 0;
    int  tex_height_   = 0;
    bool tex_is_10bit_ = false;

    // ---------- CPU frame buffer (DXVA2 fallback / SW decode) ----------
    QMutex mutex_;
    std::vector<uint8_t> y_buf_;
    std::vector<uint8_t> uv_buf_;
    int  buf_y_ls_   = 0;
    int  buf_uv_ls_  = 0;
    int  buf_width_  = 0;
    int  buf_height_ = 0;
    bool buf_10bit_  = false;
    bool has_frame_  = false;

    // ---------- D3D11VA GPU-direct path (WGL_NV_DX_interop) ----------
    AVFrame *pending_hw_ = nullptr;
    bool     hw_mode_    = false;

    // D3D11 objects (borrowed from FFmpeg's hw device, AddRef'd)
    ID3D11Device           *d3d_dev_    = nullptr;
    ID3D11DeviceContext    *d3d_devctx_ = nullptr;
    ID3D11VideoDevice      *d3d_vdev_   = nullptr;
    ID3D11VideoContext     *d3d_vctx_   = nullptr;

    void (*d3d_lock_)(void *)   = nullptr;
    void (*d3d_unlock_)(void *) = nullptr;
    void  *d3d_lock_ctx_        = nullptr;

    // D3D11 Video Processor
    ID3D11VideoProcessorEnumerator *vp_enum_ = nullptr;
    ID3D11VideoProcessor           *vp_      = nullptr;
    ID3D11Texture2D                *vp_out_  = nullptr;
    ID3D11VideoProcessorOutputView *vp_outv_ = nullptr;
    ID3D11VideoProcessorInputView  *vp_inv_      = nullptr;
    ID3D11Texture2D                *vp_inv_tex_  = nullptr;
    UINT                            vp_inv_slice_ = 0;
    int  vp_w_ = 0;
    int  vp_h_ = 0;

    // WGL_NV_DX_interop
    PFN_wglDXOpenDeviceNV       fn_OpenDevice_   = nullptr;
    PFN_wglDXCloseDeviceNV      fn_CloseDevice_  = nullptr;
    PFN_wglDXRegisterObjectNV   fn_Register_     = nullptr;
    PFN_wglDXUnregisterObjectNV fn_Unregister_   = nullptr;
    PFN_wglDXLockObjectsNV      fn_Lock_         = nullptr;
    PFN_wglDXUnlockObjectsNV    fn_Unlock_       = nullptr;
    HANDLE wgl_dev_  = nullptr;
    HANDLE wgl_tex_  = nullptr;
    GLuint tex_bgra_ = 0;

    // Helper methods
    bool loadWGLFuncs();
    bool initD3D11Interop(const AVFrame *hw_frame);
    bool setupVideoProcessor(int w, int h);
    void cleanupD3D11Interop();
    void paintHwFrame();

    int play_state_ = 2;
};

#endif // DISPLAYWIND_H
