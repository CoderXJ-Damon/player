# 0voice_player 播放器项目架构文档

> 生成日期：2026-03-23
> 项目路径：`0voice_player-master/`
> 技术栈：C++17 · Qt5 · FFmpeg 4.2.1 · SDL2

---

## 目录

1. [项目概述](#1-项目概述)
2. [目录与文件说明](#2-目录与文件说明)
3. [模块划分](#3-模块划分)
4. [类关系图](#4-类关系图)
5. [数据流与信号槽连接](#5-数据流与信号槽连接)
6. [线程模型](#6-线程模型)
7. [核心数据结构](#7-核心数据结构)
8. [状态机](#8-状态机)
9. [关键设计模式](#9-关键设计模式)
10. [第三方依赖](#10-第三方依赖)
11. [UI 层说明](#11-ui-层说明)
12. [构建配置](#12-构建配置)

---

## 1. 项目概述

**0voice_player** 是零声教育的跨平台多媒体播放器教学项目，基于 FFmpeg + SDL2 + Qt5 构建，强调 **UI 与播放引擎完全分离**，便于向 Android/iOS/macOS 等平台迁移。

**核心功能：**

| 功能 | 描述 |
|------|------|
| 本地文件播放 | 支持主流音视频格式 |
| 网络流播放 | RTSP / RTMP / UDP / HTTP |
| 变速播放 | 0.5X – 2.0X，基于 Sonic 算法保留音调 |
| 音量控制 | 实时调节，0–100% |
| 截图 | FFmpeg JPEG 编码帧截取 |
| 播放列表 | 拖拽加载、上下文菜单管理 |
| 缓冲监控 | 实时显示音视频缓存时长 |
| Seek | 精确跳转，串号机制保证 flush 一致性 |

---

## 2. 目录与文件说明

```
0voice_player-master/
├── main.cpp                  # 程序入口，easylogging++ 初始化
│
├── ── 核心播放引擎 ──
├── ff_ffplay.h / .cpp        # FFmpeg 播放核心（解码、渲染、同步）
├── ff_ffplay_def.h / .cpp    # 数据结构定义（PacketQueue, FrameQueue, Clock）
├── ijkmediaplayer.h / .cpp   # 高层 API 包装，状态机管理
├── ffmsg.h                   # 跨线程消息类型定义（100+ 类型）
├── ffmsg_queue.h / .cpp      # 线程安全消息队列
│
├── ── UI 层 ──
├── homewindow.h / .cpp / .ui # 主窗口，播放控制器
├── displaywind.h / .cpp / .ui# 视频渲染 Widget
├── playlist.h / .cpp / .ui   # 播放列表窗口
├── playlistwind.h / .cpp / .ui
├── medialist.h / .cpp        # 文件列表（QListWidget 子类）
├── urldialog.h / .cpp / .ui  # 网络 URL 输入对话框
├── customslider.h / .cpp     # 自定义进度/音量滑块
├── toast.h / .cpp            # 非阻塞 Toast 通知（单例）
│
├── ── 工具与辅助 ──
├── imagescaler.h             # YUV→RGB 视频帧格式转换（libswscale 封装）
├── screenshot.h / .cpp       # 截图功能（FFmpeg JPEG 编码）
├── sonic.h / .cpp            # Sonic 变速音频算法
├── ijksdl_timer.h / .cpp     # 网络速度采样工具
├── globalhelper.h / .cpp     # QSS 加载、图标管理、配置持久化
├── util.h / .cpp             # 通用工具、错误码定义
│
├── ── 资源与依赖 ──
├── resource.qrc              # Qt 资源文件
├── res/                      # 图标、样式表
├── ffmpeg-4.2.1-win32-dev/   # FFmpeg 静态头文件
├── SDL2/                     # SDL2 头文件
├── dll/                      # 运行时 DLL（ffmpeg, SDL2）
└── log/                      # 运行日志输出目录
```

---

## 3. 模块划分

```
┌──────────────────────────────────────────────────────┐
│                   Qt UI Layer                        │
│   HomeWindow · DisplayWind · Playlist · Toast        │
│   CustomSlider · UrlDialog · MediaList               │
└────────────────────┬─────────────────────────────────┘
                     │ Qt Signal/Slot
┌────────────────────▼─────────────────────────────────┐
│               High-Level Player API                  │
│                IjkMediaPlayer                        │
│   状态机 · 消息循环 · 属性控制接口                   │
└────────────────────┬─────────────────────────────────┘
                     │ 函数调用 + 回调
┌────────────────────▼─────────────────────────────────┐
│              Core Playback Engine                    │
│                  FFPlayer                            │
│  read_thread · Decoder × 2 · video_refresh_thread   │
│  PacketQueue · FrameQueue · Clock · Sonic            │
└────────────────────┬─────────────────────────────────┘
                     │ API 调用
┌────────────────────▼─────────────────────────────────┐
│              Third-Party Libraries                   │
│   FFmpeg 4.2.1 (avformat/avcodec/swscale/swresample) │
│   SDL2 (mutex/cond/audio)  · Sonic · easylogging++  │
└──────────────────────────────────────────────────────┘
```

---

## 4. 类关系图

```
QMainWindow
└── HomeWindow
    ├── uses   → IjkMediaPlayer
    │             └── owns → FFPlayer
    │                         ├── owns → Decoder (audio)
    │                         │           └── std::thread decoder_thread_
    │                         ├── owns → Decoder (video)
    │                         │           └── std::thread decoder_thread_
    │                         ├── owns → PacketQueue (audioq / videoq)
    │                         ├── owns → FrameQueue  (sampq / pictq)
    │                         ├── owns → Clock (audclk / vidclk)
    │                         ├── owns → MessageQueue
    │                         ├── owns → sonicStream* (变速)
    │                         ├── std::thread read_thread_
    │                         └── std::thread video_refresh_thread_
    │
    ├── owns   → DisplayWind (QWidget)
    │             ├── owns → ImageScaler (SwsContext 封装)
    │             └── QMutex (帧保护)
    │
    ├── owns   → Playlist (QWidget)
    │             └── owns → MediaList (QListWidget)
    │                         └── QMenu / QAction (右键菜单)
    │
    ├── owns   → CustomSlider × 2 (进度 / 音量)
    ├── uses   → Toast (singleton)
    └── uses   → UrlDialog (QDialog)
```

---

## 5. 数据流与信号槽连接

### 5.1 播放初始化流程

```
用户双击文件 / 拖拽文件
  → Playlist::SigPlay(url)
  → HomeWindow::play(url)
  → IjkMediaPlayer::ijkmp_set_data_source(url)
  → IjkMediaPlayer::ijkmp_prepare_async()
      ├─ 启动 msg_thread_（消息循环）
      └─ FFPlayer::ffp_prepare_async_l()
            → stream_open()
                ├─ packet_queue_init × 2
                ├─ frame_queue_init  × 2
                ├─ init_clock        × 2
                ├─ 启动 read_thread_
                └─ 启动 video_refresh_thread_
```

### 5.2 播放数据流（生产者-消费者）

```
read_thread（I/O 密集）
  av_read_frame() → 压入 videoq / audioq
                             │
          ┌──────────────────┴──────────────────┐
          ▼                                     ▼
 Decoder::audio_thread（CPU）        Decoder::video_thread（CPU）
  avcodec_receive_frame()            avcodec_receive_frame()
  swr_convert() [重采样]             ImageScaler::Scale() [YUV→RGB]
  sonicWrite() [变速]
          │                                     │
          ▼                                     ▼
     sampq (FrameQueue)                  pictq (FrameQueue)
                                               │
                                               ▼
                                  video_refresh_thread（定时）
                                    video_refresh_callback_()
                                               │
                                               ▼
                                  HomeWindow::OutputVideo()
                                  DisplayWind::Draw()
                                  DisplayWind::update() → paintEvent()
```

### 5.3 Qt 信号槽映射

| 发射方 | 信号 | 槽 | 说明 |
|--------|------|----|------|
| HomeWindow | `sig_showTips(Level, QString)` | `on_showTips()` | 显示 Toast 通知 |
| HomeWindow | `sig_updateCurrentPosition(long)` | `on_updateCurrentPosition()` | 更新进度条 |
| HomeWindow | `sig_updateAudioCacheDuration(int64_t)` | `on_UpdateAudioCacheDuration()` | 音频缓存显示 |
| HomeWindow | `sig_updateVideoCacheDuration(int64_t)` | `on_UpdateVideoCacheDuration()` | 视频缓存显示 |
| HomeWindow | `sig_updatePlayOrPause(int)` | `on_updatePlayOrPause()` | 更新播放按钮状态 |
| HomeWindow | `sig_stopped()` | `stop()` | 播放结束处理 |
| Playlist | `SigPlay(std::string)` | `HomeWindow::play()` | 触发播放 |
| CustomSlider | `SigCustomSliderValueChanged(int)` | `on_playSliderValueChanged()` | Seek 操作 |
| CustomSlider | `SigCustomSliderValueChanged(int)` | `on_volumeSliderValueChanged()` | 音量调节 |

### 5.4 跨线程消息流

```
FFPlayer 各工作线程
  → ffp_notify_msg*() 写入 MessageQueue
      ↓
msg_thread_（IjkMediaPlayer）
  ijkmp_msg_loop() 读取消息
      ├─ FFP_MSG_PREPARED          → ijkmp_start() 开始播放
      ├─ FFP_MSG_VIDEO_RENDERING_START → DisplayWind::StartPlay()
      ├─ FFP_MSG_PLAY_FNISH        → emit sig_stopped()
      ├─ FFP_MSG_SEEK_COMPLETE     → 更新进度
      └─ FFP_MSG_SCREENSHOT_COMPLETE → Toast 通知
```

---

## 6. 线程模型

| 线程 | 归属 | 职责 | 同步方式 |
|------|------|------|----------|
| **主线程** | Qt 事件循环 | UI 渲染、信号分发、定时器(250ms) | Qt 信号槽 |
| **msg_thread_** | IjkMediaPlayer | 消费 FFP_MSG_* 消息，发射 Qt 信号 | SDL_mutex + SDL_cond |
| **read_thread_** | FFPlayer | av_read_frame()，填充 audioq/videoq | SDL_mutex + SDL_cond |
| **auddec.decoder_thread_** | Decoder | 解码音频包，重采样，变速，填充 sampq | SDL_mutex + SDL_cond |
| **viddec.decoder_thread_** | Decoder | 解码视频包，YUV→RGB，填充 pictq | SDL_mutex + SDL_cond |
| **video_refresh_thread_** | FFPlayer | 定时取帧，音视频同步，触发渲染回调 | av_usleep + SDL_mutex |

**同步原语：**

- `SDL_mutex` + `SDL_cond`：PacketQueue、FrameQueue、MessageQueue 内部同步
- `QMutex`：DisplayWind 视频帧读写保护
- `std::mutex`：HomeWindow 缓存时长字段
- `abort_request` flag：优雅退出，避免死锁

---

## 7. 核心数据结构

### PacketQueue（线程安全链表）

```cpp
struct PacketQueue {
    MyAVPacketList *first_pkt, *last_pkt;   // 链表头尾
    int nb_packets;                          // 包数量
    int size;                                // 字节总大小
    int64_t duration;                        // 累计时长
    int serial;                              // 串号（Seek 检测）
    SDL_mutex *mutex;
    SDL_cond  *cond;
};
```

### FrameQueue（环形缓冲区，固定大小 16）

```cpp
struct FrameQueue {
    Frame queue[FRAME_QUEUE_SIZE];  // 圆形队列
    int rindex;                     // 读指针
    int windex;                     // 写指针
    int size;
    SDL_mutex *mutex;
    SDL_cond  *cond;
};
```

### Clock（播放时钟）

```cpp
struct Clock {
    double pts;          // 当前时间戳
    double pts_drift;    // 时钟基准 - 系统时间偏移
    double speed;        // 播放速率
    int    serial;       // 串号（与队列对齐）
    int    paused;
};
```

### MessageQueue（消息总线）

```cpp
struct AVMessage {
    int   what;     // 消息类型（FFP_MSG_*）
    int   arg1, arg2;
    void *obj;
    AVMessage *next;
};
```

---

## 8. 状态机

`IjkMediaPlayer` 维护播放器有限状态机：

```
MP_STATE_IDLE (0)
  ↓  ijkmp_set_data_source()
MP_STATE_INITIALIZED (1)
  ↓  ijkmp_prepare_async()
MP_STATE_ASYNC_PREPARING (2)
  ↓  [stream_open 完成, FFP_MSG_PREPARED]
MP_STATE_PREPARED (3)
  ↓  ijkmp_start()          ← 也可以先 seek
MP_STATE_STARTED (4)   ←──────────────────┐
  ↓  ijkmp_pause()                         │ ijkmp_start()
MP_STATE_PAUSED (5) ──────────────────────┘
  ↓  [EOF]
MP_STATE_COMPLETED (6)
  ↓  ijkmp_stop()
MP_STATE_STOPPED (7)
  ↓  ijkmp_release()
MP_STATE_END (9)

  ↓  [任意时刻发生错误]
MP_STATE_ERROR (8)
```

---

## 9. 关键设计模式

| 模式 | 应用位置 | 说明 |
|------|----------|------|
| **生产者-消费者** | PacketQueue / FrameQueue | read→decode→render 管道 |
| **有限状态机** | IjkMediaPlayer | 播放生命周期管理 |
| **消息传递（Actor）** | MessageQueue + FFP_MSG_* | 线程间解耦通信 |
| **回调（Callback）** | `video_refresh_callback_` / `msg_loop_` | 引擎通知 UI 层 |
| **单例** | Toast | 全局通知系统 |
| **门面（Facade）** | IjkMediaPlayer | 对外屏蔽 FFPlayer 复杂度 |
| **观察者** | Qt Signal/Slot | UI 组件响应播放状态 |
| **策略** | Sonic 变速 | 不同速率区间使用不同算法 |

---

## 10. 第三方依赖

| 库 | 版本 | 用途 |
|----|------|------|
| **FFmpeg** | 4.2.1 (win32-dev) | 解封装、解码、缩放、重采样、截图编码 |
| **SDL2** | 2.x | 音频输出、跨平台 mutex/cond |
| **Qt** | 5.x | UI 框架、信号槽、绘图 |
| **Sonic** | 内嵌 | 变速/变调音频处理（PICOLA 算法） |
| **easylogging++** | 内嵌 | 日志系统（文件 + 控制台） |

**FFmpeg 使用模块：**

| 模块 | 功能 |
|------|------|
| libavformat | 容器解析（mp4/mkv/flv/ts…） |
| libavcodec | 音视频编解码 |
| libavutil | 时间戳、像素格式工具 |
| libswscale | YUV ↔ RGB 图像缩放/转换 |
| libswresample | 音频采样率/格式转换 |
| libavfilter | 音视频滤镜（可选） |

---

## 11. UI 层说明

### 主窗口布局（HomeWindow）

```
┌──────────────────────────────────────────────────┐
│  文件  设置  帮助                                │
├──────────────────────────────────────────────────┤
│                                                  │
│              DisplayWind                         │
│          （QPainter 绘制 QImage）                │
│                                                  │
├──────────────────────────────────────────────────┤
│  进度: [════════════>        ]  00:15 / 01:30    │
│  音量: [══════>              ]  50%              │
├──────────────────────────────────────────────────┤
│  [▶/⏸]  [⏹]  [上一个]  [下一个]  [后退]  [前进] │
│  [截图]  [速度▼]  [列表]                         │
├──────────────────────────────────────────────────┤
│  音频缓存: 500ms    视频缓存: 480ms              │
└──────────────────────────────────────────────────┘
```

### 视频渲染流程（DisplayWind）

```
video_refresh_callback_()
  → HomeWindow::OutputVideo()
      → ImageScaler::Scale()       // YUV420P → RGB24
      → DisplayWind::Draw(QImage)
          → 保持宽高比，黑边填充
          → update() 触发重绘
  → paintEvent()
      → QPainter::drawImage()
```

### 播放列表（Playlist / MediaList）

- 支持拖拽（`dragEnterEvent` / `dropEvent`）
- 右键菜单：添加文件 / 添加 URL / 删除 / 清空
- 双击条目触发 `SigPlay` 信号

### 自定义滑块（CustomSlider）

重写鼠标事件，实现点击任意位置即跳转：
- `mousePressEvent` → 计算目标值 → emit `SigCustomSliderValueChanged`
- `mouseReleaseEvent` → 触发 Seek
- 拖动期间不重复触发，减少多余 Seek 请求

---

## 12. 构建配置

**Qt 项目文件（0voice_player.pro）关键配置：**

```qmake
CONFIG += c++17
TARGET  = 0voice_player
QT     += core gui widgets

# FFmpeg 头文件
INCLUDEPATH += ffmpeg-4.2.1-win32-dev/include

# SDL2 头文件
INCLUDEPATH += SDL2/include

# FFmpeg 链接库
LIBS += -Lffmpeg-4.2.1-win32-dev/lib \
        -lavformat -lavcodec -lavutil \
        -lswscale -lswresample \
        -lavdevice -lavfilter

# SDL2 链接库
LIBS += -LSDL2/lib -lSDL2

# Windows 32-bit 平台
DEFINES += WIN32 _WIN32
```

**运行时依赖 DLL（dll/ 目录）：**

- `avformat-58.dll` / `avcodec-58.dll` / `avutil-56.dll`
- `swscale-5.dll` / `swresample-3.dll`
- `SDL2.dll`
- Qt5 相关 DLL

---

*本文档由 Claude Code 自动生成，基于对全部源文件的静态分析。*
