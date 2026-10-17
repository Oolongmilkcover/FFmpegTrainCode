@echo off
REM ===========================================================================
REM  Generate test media for 08-04-mp4muxer (raw YUV + raw PCM)
REM
REM  Why this script:
REM    720x576_yuv420p.yuv is several hundred MB (622080 bytes per frame),
REM    far beyond GitHub's 100MB per-file limit, so it is NOT in the repo
REM    (see .gitignore: *.yuv). Regenerate it from the 135KB
REM    sound_in_sync_test.mp4 that IS committed.
REM
REM  NOTE: this file is intentionally ASCII-only. A .bat containing UTF-8
REM        Chinese gets mis-parsed by cmd.exe (which uses the OEM codepage,
REM        GBK on zh-CN), producing garbled commands. Keep batch files ASCII.
REM
REM  Requires:
REM    1. ffmpeg in PATH, or edit FFMPEG below to point at ffmpeg.exe
REM    2. sound_in_sync_test.mp4 in this folder (committed with the repo)
REM
REM  Usage: double-click, or run  fetch_test_media.bat  in cmd
REM ===========================================================================

setlocal
set FFMPEG=ffmpeg

REM If ffmpeg is not in PATH, uncomment and edit the next line:
REM set FFMPEG="C:\Program Files\ffmpeg\ffmpeg8.1\bin\ffmpeg.exe"

REM How many seconds to extract. The source is 40s; default 5s (125 frames),
REM because 5s of YUV is already ~76MB. Set to 40 for the full clip (622MB).
set SECONDS=5

if not exist "sound_in_sync_test.mp4" (
    echo [ERROR] sound_in_sync_test.mp4 not found.
    echo         It should be committed with the repo - run: git checkout -- sound_in_sync_test.mp4
    exit /b 1
)

echo ============================================================
echo  1/2  raw YUV  (720x576, 25fps, yuv420p, first %SECONDS%s)
echo ============================================================
%FFMPEG% -y -i "sound_in_sync_test.mp4" -t %SECONDS% -pix_fmt yuv420p "720x576_yuv420p.yuv"
if errorlevel 1 goto :fail

echo.
echo ============================================================
echo  2/2  raw PCM  (44100Hz, 2ch, s16le, first %SECONDS%s)
echo ============================================================
%FFMPEG% -y -i "sound_in_sync_test.mp4" -t %SECONDS% -vn -ar 44100 -ac 2 -f s16le "44100_2_s16le.pcm"
if errorlevel 1 goto :fail

echo.
echo ============================================================
echo  Done. Verify the sizes:
echo ============================================================
echo   YUV: one frame = 720 x 576 x 1.5 = 622080 bytes
echo   PCM: one second = 44100 x 2 x 2 = 176400 bytes

for %%A in ("720x576_yuv420p.yuv") do set YUVSZ=%%~zA
for %%A in ("44100_2_s16le.pcm")   do set PCMSZ=%%~zA
echo.
echo   yuv actual = %YUVSZ% bytes
echo   pcm actual = %PCMSZ% bytes

echo.
echo Now run:
echo   build\mp4muxer.exe 720x576_yuv420p.yuv 44100_2_s16le.pcm out.mp4
endlocal
exit /b 0

:fail
echo [ERROR] ffmpeg failed.
endlocal
exit /b 1
