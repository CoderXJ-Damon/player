#ifndef DISPLAYWIND_H
#define DISPLAYWIND_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QMutex>
#include <vector>

// 只做指针使用，前向声明即可，避免把 FFmpeg/SDL 头文件拖入
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

    QOpenGLShaderProgram *program_ = nullptr;
    GLuint tex_y_  = 0;
    GLuint tex_uv_ = 0;
    GLuint vao_    = 0;
    GLuint vbo_    = 0;

    int loc_tex_y_    = -1;
    int loc_tex_uv_   = -1;
    int loc_is_10bit_ = -1;

    int  tex_width_    = 0;
    int  tex_height_   = 0;
    bool tex_is_10bit_ = false;

    QMutex mutex_;
    std::vector<uint8_t> y_buf_;
    std::vector<uint8_t> uv_buf_;
    int  buf_y_ls_   = 0;
    int  buf_uv_ls_  = 0;
    int  buf_width_  = 0;
    int  buf_height_ = 0;
    bool buf_10bit_  = false;
    bool has_frame_  = false;

    int play_state_ = 2;
};

#endif // DISPLAYWIND_H
