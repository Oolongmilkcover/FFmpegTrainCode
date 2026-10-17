@echo off
REM ===========================================================================
REM  Generate the raw YUV test clip for 08-02-encode_video
REM
REM  Why this script:
REM    yuv420p_1280x720.yuv is 165MB, beyond GitHub's 100MB per-file limit,
REM    so it is NOT in the repo (see .gitignore: *.yuv). Regenerate it from
REM    the 2MB test_1280x720.flv that IS committed.
REM
REM  NOTE: this file is intentionally ASCII-only. A .bat containing UTF-8
REM        Chinese gets mis-parsed by cmd.exe (OEM codepage, GBK on zh-CN)
REM        and produces garbled commands. Keep batch files ASCII.
REM
REM  Requires:
REM    1. ffmpeg in PATH, or edit FFMPEG below to point at ffmpeg.exe
REM    2. test_1280x720.flv in this folder (committed with the repo)
REM
REM  Usage: double-click, or run  fetch_test_yuv.bat  in cmd
REM ===========================================================================

setlocal
set FFMPEG=ffmpeg

REM If ffmpeg is not in PATH, uncomment and edit the next line:
REM set FFMPEG="C:\Program Files\ffmpeg\ffmpeg8.1\bin\ffmpeg.exe"

if not exist "test_1280x720.flv" (
    echo [ERROR] test_1280x720.flv not found.
    echo         It should be committed with the repo - run: git checkout -- test_1280x720.flv
    exit /b 1
)

echo ============================================================
echo  Generating yuv420p_1280x720.yuv
echo  source : test_1280x720.flv
echo  params : -t 5 -r 25 -pix_fmt yuv420p  (5s / 25fps / 1280x720)
echo ============================================================
%FFMPEG% -y -i "test_1280x720.flv" -t 5 -r 25 -pix_fmt yuv420p "yuv420p_1280x720.yuv"
if errorlevel 1 (
    echo [ERROR] ffmpeg failed.
    endlocal
    exit /b 1
)

echo.
echo Done. Verify the frame count:
echo   one frame = 1280 x 720 x 1.5 = 1382400 bytes
echo   expected  = 172800000 bytes  (125 frames)
for %%A in ("yuv420p_1280x720.yuv") do set YUVSZ=%%~zA
echo   actual    = %YUVSZ% bytes
echo.
echo Now run:
echo   build\encode_video.exe yuv420p_1280x720.yuv out.h264
endlocal
exit /b 0
