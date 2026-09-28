@echo off
setlocal
chcp 65001 >nul 2>nul
cd /d "%~dp0"

rem ggml-cuda.dll needs cublas64_11.dll from the CUDA 11.8 toolkit at runtime.
set "PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v11.8\bin;%PATH%"

set "EXE=build\bin\video_summary.exe"
if not exist "%EXE%" set "EXE=build\bin\Release\video_summary.exe"
if not exist "%EXE%" (
  echo [ERROR] video_summary.exe not found. Build it first:
  echo   cmake -S . -B build
  echo   cmake --build build --config Release --parallel 16
  exit /b 1
)

set "MODEL=models\ggml-large-v3-turbo.bin"
if not exist "%MODEL%" (
  echo [WARN] models\ggml-large-v3-turbo.bin missing, falling back to tiny.
  set "MODEL=models\ggml-tiny.bin"
)
if not exist "%MODEL%" (
  echo [ERROR] no whisper model found under models\
  exit /b 1
)

set "VIDEO="
for %%F in (*.mp4) do (
  if not defined VIDEO set "VIDEO=%%F"
)
if not defined VIDEO (
  echo [ERROR] no *.mp4 found in %~dp0
  exit /b 1
)

set "FFMPEG=F:\Code\LOLClip\bin\ffmpeg-4.1\ffmpeg.exe"

echo ============================================
echo  video_summary demo
echo  exe   : %EXE%
echo  model : %MODEL%
echo  input : %VIDEO%
echo ============================================

if exist "%FFMPEG%" (
  "%EXE%" -m "%MODEL%" --ffmpeg "%FFMPEG%" --ollama-url http://127.0.0.1:11434 --ollama-model qwen3:8b "%VIDEO%"
) else (
  "%EXE%" -m "%MODEL%" --ollama-url http://127.0.0.1:11434 --ollama-model qwen3:8b "%VIDEO%"
)

set "RC=%ERRORLEVEL%"
echo.
echo [demo] exit code: %RC%
exit /b %RC%
