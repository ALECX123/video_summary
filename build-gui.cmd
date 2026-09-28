@echo off
setlocal
set "ROOT=%~dp0."
set "VSVARS=D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=D:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=D:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\ninja\ninja.exe"
set "CUDA118=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v11.8"

call "%VSVARS%" >nul 2>&1
if errorlevel 1 (
    echo VCVARS_FAIL
    exit /b 9
)

"%CMAKE%" -S "%ROOT%" -B "%ROOT%\build" -G Ninja ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_CUDA_COMPILER="%CUDA118%\bin\nvcc.exe" ^
    -DCUDAToolkit_ROOT="%CUDA118%" ^
    -DCMAKE_CUDA_FLAGS=-allow-unsupported-compiler ^
    -DCMAKE_CUDA_ARCHITECTURES=89 ^
    -DOPENSSL_ROOT_DIR=F:/Code/openssl-3.0.22-win ^
    -DCMAKE_PREFIX_PATH=F:/Qt/qt6.10.3-static ^
    > "%ROOT%\build-gui-configure.log" 2>&1
if errorlevel 1 (
    echo CONFIGURE_FAIL
    exit /b 2
)

"%CMAKE%" --build "%ROOT%\build" --parallel 16 > "%ROOT%\build-gui-run.log" 2>&1
if errorlevel 1 (
    echo BUILD_FAIL
    exit /b 3
)

if exist "%ROOT%\build\bin\video_summary.exe" (
    if exist "%ROOT%\build\bin\video_summary_gui.exe" (
        echo BUILD_OK_GUI
        exit /b 0
    )
    echo BUILD_OK_CLI_ONLY
    exit /b 0
)
echo EXE_MISSING
exit /b 4
