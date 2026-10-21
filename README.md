# FFmpegTrainCode

FFmpeg 音视频编码 / 封装的**渐进式学习代码**，面向「采集 → 编码 → 推流 → 播放器」这条完整链路。

代码从*能跑的最小示例*出发，注释按「【知识点 N】」编号组织，重点讲清楚：

- 每个 API 的参数**各自是什么含义**
- 参数**为什么必须这么设**，设错了会怎样
- 调用**顺序**为什么不能颠倒
- 返回值怎么判，**哪些返回值是「正常」而不是错误**

五个工程共用一套 C++17 风格（源码 + CMake），可以直接作为简历项目里
`encoder` / `muxer` / `pusher` 层的基础。**前四个不用任何硬件，第五个有合成源也不用。**

| 工程 | 一句话 |
| --- | --- |
| `08-01-encode_audio` | 裸 PCM → AAC，讲透音频编码参数与 packed/planar |
| `08-02-encode_video` | 裸 YUV → H.264，讲透视频参数与时间基 |
| `08-03-muxing_flv` | 合成音视频 → flv/mp4，讲透容器与交织 |
| `08-04-mp4muxer` | YUV + PCM → MP4，讲透**音视频同步**与模块化分层 |
| `08-05-capture_push` | 摄像头/麦克风 → 编码 → **落盘或 RTMP 推流**，讲透队列/多线程/重连 |

---

## 环境

| 项目 | 版本 |
| --- | --- |
| Qt | 6.11.0（只用它的 MinGW 工具链，**不链接任何 Qt 库**） |
| 编译器 | MinGW 13.1.0（`C:\Qt\Tools\mingw1310_64`） |
| FFmpeg | 8.1（`libavformat 62` / `libavcodec 62` / `libavutil 60`） |
| 语言 | C++17 |
| 构建 | CMake |

> **注意**：这些工程 `CONFIG -= qt`，一行 Qt API 都不用。
> 选 Qt 的 kit 只是为了拿它的 MinGW 工具链，保证和后续 Qt 音视频项目同一套 ABI。
> 换 MSVC 也能编（FFmpeg 8.1 的 Windows dev 包同时提供 `.lib` 与 `.dll.a`）。

---

## 目录结构

```
FFmpegTrainCode/
├── README.md                       本文件
├── 视频编码与封装知识点详解.md        ← 08-02 / 08-03 的配套讲解
│
├── 08-01-encode_audio/             音频编码：裸 PCM → AAC
│   ├── main.cpp                    ★ 学习重点（知识点 1~28）
│   ├── CMakeLists.txt
│   ├── 音频编码知识点详解.md
│   ├── 说明.txt
│   └── 48000_2_f32le.pcm / 48000_2_s16le.pcm / buweishui.aac   ← 测试素材
│
├── 08-02-encode_video/             视频编码：裸 YUV → H.264
│   ├── main.cpp                    ★ 学习重点（知识点 1~23）
│   ├── CMakeLists.txt
│   ├── fetch_test_yuv.bat          生成 165MB 的 YUV 素材（该文件不入库）
│   └── test_1280x720.flv           参考素材（2MB，用于生成 YUV）
│
├── 08-03-muxing_flv/               封装：H.264 + AAC → flv / mp4
│   ├── main.cpp                    ★ 学习重点（知识点 1~24）
│   └── CMakeLists.txt
│
├── 08-04-mp4muxer/                 ★ 同步封装：YUV + PCM → H.264 + AAC → MP4
│   ├── main.cpp                    串联 + 音视频交织调度（知识点 16~19）
│   ├── videoencoder.{h,cpp}        知识点 2~5
│   ├── audioencoder.{h,cpp}        知识点 6~7
│   ├── audioresampler.{h,cpp}      知识点 8~9
│   ├── muxer.{h,cpp}               知识点 10~15
│   ├── CMakeLists.txt
│   ├── 音视频同步封装知识点详解.md
│   ├── fetch_test_media.bat        生成 YUV + PCM 素材（不入库）
│   └── sound_in_sync_test.mp4      参考素材（132KB，用于生成素材）
│
└── 08-05-capture_push/             ★ 采集推流：摄像头/麦克风 → 编码 → 落盘 / RTMP
    ├── main.cpp                    命令行 + 运行时长 + 退出码（知识点 32）
    ├── pusher.{h,cpp}              编排：4 线程 / 2 队列 / 重连（知识点 22~31）
    ├── capture.{h,cpp}             采集源：设备 + 合成（知识点 3~8、33~37）
    ├── encoder.{h,cpp}             x264 / AAC（知识点 9~16）
    ├── muxer.{h,cpp}               文件 / RTMP 输出（知识点 17~21、38）
    ├── dropqueue.h                 有界队列（知识点 1~2）
    ├── CMakeLists.txt
    └── 采集推流知识点详解.md
```

> 仓库只保留 **C++ 源码 + CMake 配置 + 文档 + 小体积测试素材**。
> 早期版本里的 qmake `.pro` 文件和原始 C 版本（`*.bak`）已全部移除。
> 08-05 不需要任何测试素材：没有摄像头/麦克风时用内置合成源即可跑通全链路。

---

## 五个工程的关系

```
                      08-01             08-02              08-03            08-04            08-05
                    音频编码           视频编码            封装          同步封装         采集推流
                    ────────          ────────          ────────        ─────────        ────────
  输入          裸 PCM (f32le)      裸 YUV (yuv420p)   YUV + PCM       YUV + PCM      摄像头/麦克风
  数据源        文件               文件               文件            文件             ★实时源
      │              │                  │                  │               │               │
      ▼              ▼                  ▼                  ▼               ▼               ▼
  学到什么      音频参数四要素      视频参数 + 时间基   容器/交织/      两路时间轴如何   队列/背压/
                packed↔planar        pts = 帧号         GLOBAL_HEADER   统一与调度      多线程/重连
                frame_size=1024      GOP/B帧/lookahead  trailer(moov)   模块化分层      设备协商/节流
```

**学习路径是"编码 → 封装 → 同步 → 实时"：**

- 08-01 / 08-02：单个编码器的参数与时间基
- 08-03：容器怎么把两路流装进去（交织、trailer）
- 08-04：两条时间轴怎么对上（同步），并开始分层
- **08-05：数据源从文件换成设备后，多出来的那一整类问题**
  （队列与背压、多线程解耦、阻塞读的中断、断线重连）

**为什么先学编码再学封装？** 因为封装里的每个坑都和编码参数直接相关：

| 封装的坑 | 来自编码的哪个参数 |
| --- | --- |
| 时间基要换算 | 编码器 `time_base`（视频 1/fps、音频 1/sample_rate） |
| dts ≠ pts，要按 dts 排序 | `max_b_frames > 0` |
| 要设 `GLOBAL_HEADER` | 容器要独立存放 SPS/PPS |
| 编码器有延迟，要 flush | x264 的 look-ahead + 帧级多线程 |
| 新观众黑屏 | `gop_size` 决定关键帧间隔 |
| 音视频不同步 | 两路 pts 各自的累加方式与换算 |
| 断线重连后花屏 | `gop_size`（要等下一个 I 帧） |
| 采集跟不上导致丢帧 | 编码耗时 + 队列容量 |

---

## 构建

以 08-01 为例，另外四个只改目录名和 exe 名。

```powershell
cd 08-01-encode_audio

cmake -S . -B build -G "MinGW Makefiles" `
      -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe `
      -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/mingw1310_64/bin/mingw32-make.exe

cmake --build build
```

FFmpeg 路径默认 `C:/Program Files/ffmpeg/ffmpeg8.1`，可用
`-DFFMPEG_PATH=<你的路径>` 覆盖。

> **构建后无需配置 PATH**：CMake 的 POST_BUILD 会把 FFmpeg 的 DLL
> 自动拷到 exe 同目录（约 240MB，所以这些 DLL 不会进仓库）。

> 用 Qt Creator 打开时直接选「Open Project」→ 选 `CMakeLists.txt`，
> kit 选 Qt 6.11.0 MinGW 64-bit 即可（工程不链接 Qt 库，kit 只提供工具链）。

---

## 运行

### 08-01 音频编码

```powershell
cd 08-01-encode_audio

# 默认：aac / 48000 / 2ch / f32le，输出带 ADTS 头（ffplay 可直接播）
.\build\encode_audio.exe 48000_2_f32le.pcm out.aac

# s16le 输入（走 libswresample 转换）
.\build\encode_audio.exe 48000_2_s16le.pcm out.aac aac 48000 2 s16le

# 纯裸 AAC（不含 ADTS 头，ffmpeg 解不开 —— 这是预期行为）
.\build\encode_audio.exe 48000_2_f32le.pcm out.raw aac 48000 2 f32le noadts
```

完整参数：`<input_pcm> <output_file> [codec_name] [sample_rate] [channels] [sample_fmt] [noadts]`

### 08-02 视频编码

```powershell
cd 08-02-encode_video

# 首次需要先生成 165MB 的 YUV 素材（未入库）
.\fetch_test_yuv.bat

.\build\encode_video.exe yuv420p_1280x720.yuv out.h264

# 只编前 25 帧，快速验证参数改动
.\build\encode_video.exe yuv420p_1280x720.yuv out.h264 libx264 1280 720 25 25

# 对比 preset 对耗时/码率的影响
.\build\encode_video.exe yuv420p_1280x720.yuv out.h264 libx264 1280 720 25 25 veryslow
```

完整参数：`<in.yuv> <out.h264> [codec] [w] [h] [fps] [frames] [preset] [tune]`

### 08-03 封装

```powershell
cd 08-03-muxing_flv

# 合成测试图 + 正弦声音 → FLV
.\build\muxing_flv.exe out.flv

# 同一份代码 → MP4（观察容器如何改写时间基）
.\build\muxing_flv.exe out.mp4

# 用 08-02 生成的真实 YUV 当视频源
.\build\muxing_flv.exe out.flv flv ..\08-02-encode_video\yuv420p_1280x720.yuv 1280 720 25 5

# -v 打印每一个包的时间戳
.\build\muxing_flv.exe out.flv - - 352 288 25 1 -v
```

完整参数：`<out.flv|out.mp4> [format] [yuvFile] [w] [h] [fps] [duration] [-v]`

### 08-04 同步封装（YUV + PCM → MP4）

```powershell
cd 08-04-mp4muxer

# 首次需要先生成素材（裸 YUV + 裸 PCM，都不入库）
.\fetch_test_media.bat

# 编码 + 封装成 mp4
.\build\mp4muxer.exe 720x576_yuv420p.yuv 44100_2_s16le.pcm out.mp4
```

完整参数：`<in.yuv> <in.pcm> <out.mp4>`

运行时会打印三处关键信息，对照着看最直观：

1. **写头前后的流时间基**（看 muxer 有没有改写）
2. **交织调度过程**：`apts:xx vpts:yy → 处理视频/音频`
3. **结尾汇总**：两路时间轴长度 + 差值（应小于 1 帧）

### 08-05 采集推流（摄像头/麦克风 → 落盘 / RTMP）

```powershell
cd 08-05-capture_push

# 1) 不需要任何硬件：先用合成源把整条链路跑通
.\build\capture_push.exe --synthetic-video --synthetic-audio --output out.mp4 --duration 5

# 2) 查设备名（本程序不打印日志，用 ffmpeg 查）
ffmpeg -list_devices true -f dshow -i dummy

# 3) 真实摄像头 + 麦克风 → mp4
.\build\capture_push.exe --video "video=AMD Privacy View camera" `
                         --audio "audio=Microphone (High Definition Audio Device)" `
                         --output av.mp4 --duration 5

# 4) 推到本地 RTMP（先起 SRS / nginx-rtmp / 或 ffmpeg -listen 1）
.\build\capture_push.exe --video "video=..." --audio "audio=..." `
                         --output rtmp://127.0.0.1/live/demo --duration 30
```

**这个工程刻意不输出任何日志**（见 `main.cpp` 文件头的说明）。
判断成败看**退出码**：

| 退出码 | 含义 |
| --- | --- |
| 0 | 正常结束 |
| 1 | 命令行参数错误 |
| 2 | 视频采集源打开失败（设备名错 / 被占用 / 没插） |
| 3 | 视频编码器打开失败（这份 FFmpeg 没编进 libx264？） |
| 4 | 输出打开失败（路径不可写 / 推流地址连不上） |

`--help` 有全部参数（视频源、音频源、编码旋钮、运行控制）。

---

## 测试素材

| 文件 | 大小 | 是否入库 | 说明 |
| --- | --- | --- | --- |
| `08-01/48000_2_f32le.pcm` | 3.7 MB | ✅ 是 | 48000Hz / 2ch / f32le，481489 采样点 |
| `08-01/48000_2_s16le.pcm` | 1.8 MB | ✅ 是 | 同上，s16le 格式 |
| `08-01/buweishui.aac` | 160 KB | ✅ 是 | 原始参考音频（44100Hz） |
| `08-02/test_1280x720.flv` | 2.1 MB | ✅ 是 | 参考视频（1280x720 / 25fps / 10.28s） |
| `08-02/yuv420p_1280x720.yuv` | **165 MB** | ❌ 否 | 超 GitHub 单文件 100MB 限制，用 `fetch_test_yuv.bat` 生成 |
| `08-04/sound_in_sync_test.mp4` | 132 KB | ✅ 是 | 参考素材（720×576 / 25fps / 44.1kHz / 40s） |
| `08-04/720x576_yuv420p.yuv` | **74 MB** | ❌ 否 | 由上面那个 mp4 生成（前 5 秒），用 `fetch_test_media.bat` |
| `08-04/44100_2_s16le.pcm` | 0.84 MB | ❌ 否 | 同上，也由脚本生成 |

> **08-05 不需要测试素材**：没有摄像头/麦克风就用 `--synthetic-video --synthetic-audio`
> （内置画面 + 440Hz 正弦波），整条链路照样跑通。

如果 08-01 的 PCM 素材丢了，也可以用 ffmpeg 重新生成：

```bash
# 生成 f32le PCM
ffmpeg -i buweishui.aac -ar 48000 -ac 2 -f f32le 48000_2_f32le.pcm
# 生成 s16le PCM
ffmpeg -i buweishui.aac -ar 48000 -ac 2 -f s16le 48000_2_s16le.pcm
# 生成裸 YUV（125 帧）
ffmpeg -i test_1280x720.flv -t 5 -r 25 -pix_fmt yuv420p yuv420p_1280x720.yuv
```

---

## 验证产物

```bash
# 音频
ffprobe -show_format -show_streams out.aac
ffmpeg -v error -i out.aac -f null -        # 返回 0 表示结构正确

# 视频
ffprobe -show_streams out.h264
ffplay out.h264

# 封装（flv / mp4）
ffprobe -show_format -show_streams out.flv
ffprobe -show_packets out.flv               # 看包的时间戳和交织顺序，最有信息量
ffmpeg -v error -i out.flv -f null -
```

08-05 是静默运行的程序，产物验证就更重要了：

```bash
# 落盘：结构 + 时长 + 能否解码
ffprobe -show_format -show_streams av.mp4
ffmpeg -v error -i av.mp4 -f null -

# 确认真的采到了声音（不是静音轨）
ffmpeg -v error -i av.mp4 -af volumedetect -f null -      # 看 mean_volume

# 确认真的采到了画面（抠一帧出来看）
ffmpeg -v error -i av.mp4 -vf "select=eq(n\,30)" -frames:v 1 frame30.png

# 推流：本地起一个只接一次的接收端，看能不能收到
ffmpeg -listen 1 -i rtmp://127.0.0.1:19354/live/demo -c copy -y recv.flv
```

---

## 这份代码里踩到并修掉的真实坑

记录在这里，是为了说明「注释里那些『★ 踩坑记录』不是抄来的，是实测撞出来的」。

| # | 坑 | 现象 | 正解 |
| --- | --- | --- | --- |
| 1 | 08-02 把 `time_base` 当毫秒用（`pts += 40`） | 25 帧被标成 40 秒的幻灯片 | `pts = 帧号` |
| 2 | `av_image_fill_arrays` 后立即 `free(buf)` | **进程崩溃 0xC0000005** | 该函数会改写 `frame->data[]` 指向；要拷进 frame 自己的缓冲区 |
| 3 | 写完包再读 `pkt` 字段 | 显示 `size=0 pts=NOPTS`，误以为写了空包 | `av_interleaved_write_frame` 成功后会把 pkt **清零**，要打印的字段必须提前存下来 |
| 4 | 冲刷后再 `send` 非空帧 | `avcodec_send_frame 失败: End of file` | 用 `flushed` 标志记住状态 |
| 5 | 链接时写 `-Wl,-rpath,<含空格路径>` | `ld: cannot find Files/ffmpeg/...` | MinGW 对 DLL 不支持 rpath，改用 POST_BUILD 拷 DLL |
| 6 | `goto cleanup` | C++ 编译报错：jump crosses initialization | 改成立即执行 lambda + `return` |
| 7 | `av_ts2str` 宏 | C++ 报 `taking address of temporary array` | 直接调 `av_ts_make_string(buf, ts)` |
| 8 | `const_cast<const uint8_t**>(p)` | 编译报错（`T*` 不能转 `const T**`） | 用 `const uint8_t* inData[1] = {p};` 数组退化 |
| 9 | 空字符串当位置参数 | `atoi("")=0` → 分辨率 0×0 | 用 `-` 占位，并加参数校验 |
| 10 | `ffprobe` 读裸 `.h264` | 报 `r_frame_rate=50/1`（实际 25） | 裸流靠启发式猜帧率，转 mp4 后读 VUI 才准 |
| 11 | **PATH 上有别的 MinGW** | 启动即退出，`0xC0000139`（ENTRYPOINT_NOT_FOUND），日志空 | 静态链接 `-static-libgcc -static-libstdc++` + 拷配套 `libwinpthread-1.dll`（四个工程的 CMakeLists 已处理） |
| 12 | 编码出错时 `push_back(NULL)` | 空指针崩溃 | 出错就 free + break，绝不 push |
| 13 | `avformat_close_input` 关**输出**上下文 | 未定义行为、句柄泄漏 | `avio_closep` + `avformat_free_context` |
| 14 | 写完 trailer 立刻查文件大小 | 读到 0 字节 | 先 `avio_closep`（或显式 DeInit）再查 |
| 15 | `.bat` 里写 UTF-8 中文 | cmd 按 GBK 解析，命令全乱 | 批处理文件保持纯 ASCII |
| 16 | 合成源不节流 | 跑 5 秒生成 2895 帧、容器 375 秒 | 采集层按 fps/采样数节流（`sleepUntil`） |
| 17 | 节流基准记在 `open()` 里 | 跑 4 秒出 5 秒视频（开头 30 帧被瞬间放出） | 改成首帧**惰性**记录起点 |
| 18 | 虚拟摄像头不等帧率 | 4.4 秒拿到 1630 帧（≈370fps） | 帧率限制要加在**采集层**，不能只加在合成源 |
| 19 | dshow 音频默认 500ms 一块 | 音频比视频长 0.48 秒、延迟 ≥0.5 秒 | 传 `audio_buffer_size=100`（毫秒） |
| 20 | `rtmp://` 靠扩展名猜格式 | `avformat_alloc_output_context2` 返回 **-22 (EINVAL)**，推流根本起不来 | 按协议头映射到 `flv`（`effectiveFormat`） |
| 21 | 编码器写死 `stream_index` | 只采音频时写包失败（音频是 0 号流，包却标 1） | 由编排层按实际流数分配，在 sink 里写 |
| 22 | 端口探测打断 RTMP 握手 | `ffmpeg -listen 1` 只接一次连接，探测把它用掉了 | 验证推流时**别**去连那个端口 |
| 23 | 重连后立刻写 P 帧 | 画面花屏/解不出 | 等下一个关键帧（`AV_PKT_FLAG_KEY`）再写 |

> 第 11 条是 08-04 改造里最花时间的问题：`C:\mingw64`（GCC 15.2 / win32 线程）
> 在 PATH 里排在 Qt 的 MinGW 13.1（posix 线程）之前，导致 exe 加载到不匹配的
> `libstdc++-6.dll`。完整排查过程见
> [08-04 的文档](08-04-mp4muxer/音视频同步封装知识点详解.md) §9.1。
>
> 第 16~20 条都是 08-05 实测撞出来的（每一处都有前后对比数据），
> 完整排查过程见
> [08-05 的文档](08-05-capture_push/采集推流知识点详解.md) §3.6~§5.1。

---

## 许可证与来源

- `08-01` / `08-02` / `08-04` 原始版本来自 **Liao Qingfu** 的 FFmpeg 音视频教程
- `08-03` 原始版本源自 FFmpeg 官方示例 `doc/examples/muxing.c`
- `08-05` 的 pusher 模块参考 `QtFFmpegPlayer/Player/QtPlayer/pusher/`，
  本仓库做了简化（去掉日志与实验脚手架）并修掉 7 处实测问题
- C++17 改写、FFmpeg 8.1 适配、注释、文档、CMake 构建：本仓库

> 早期版本曾保留原始 C 源码（`main.c.legacy-c-version.bak`）作为「C → C++ 迁移」
> 的对照证据，现已移除，仓库只保留 C++ 实现。
> 迁移过程中 `08-01` 的 C++ 版输出与 C 版**逐字节完全一致**（MD5 相同），
> 说明转换是纯语法/风格迁移，没有改变行为。
