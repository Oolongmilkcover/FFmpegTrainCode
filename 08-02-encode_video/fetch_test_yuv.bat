@echo off
REM ===========================================================================
REM  生成 08-02 用的裸 YUV 测试素材
REM
REM  为什么要这个脚本：
REM    yuv420p_1280x720.yuv 是 165MB，超过 GitHub 单文件 100MB 的硬限制，
REM    所以没进仓库（见 .gitignore 的 *.yuv）。用这个脚本重新生成即可。
REM
REM  前置条件：
REM    1. ffmpeg 在 PATH 里，或修改下面的 FFMPEG 变量指向 ffmpeg.exe
REM    2. 本目录下有 test_1280x720.flv（2MB，已随仓库提交）
REM
REM  用法：在本目录双击，或在 cmd 里执行： fetch_test_yuv.bat
REM ===========================================================================

setlocal
set FFMPEG=ffmpeg

REM 如果 ffmpeg 不在 PATH，取消下面这行注释并改成你的实际路径
REM set FFMPEG="C:\Program Files\ffmpeg\ffmpeg8.1\bin\ffmpeg.exe"

if not exist "test_1280x720.flv" (
    echo [错误] 找不到 test_1280x720.flv
    echo         这个文件应该随仓库一起提交，请先 git checkout 它。
    exit /b 1
)

echo 正在生成 yuv420p_1280x720.yuv ...
echo   源文件 : test_1280x720.flv
echo   参数   : -t 5 -r 25 -pix_fmt yuv420p  (5 秒 / 25fps / 1280x720)
%FFMPEG% -y -i "test_1280x720.flv" -t 5 -r 25 -pix_fmt yuv420p "yuv420p_1280x720.yuv"
if errorlevel 1 (
    echo [失败] ffmpeg 执行出错
    exit /b 1
)

echo.
echo 完成。校验一下帧数：
echo   1280 x 720 x 1.5 = 1382400 字节/帧
for %%A in ("yuv420p_1280x720.yuv") do echo   文件大小 = %%~zA 字节
echo   期望大小 = 172800000 字节（= 125 帧 x 1382400）
echo.
echo 现在可以运行 08-02 的编码演示了：
echo   build\encode_video.exe yuv420p_1280x720.yuv out.h264
endlocal
