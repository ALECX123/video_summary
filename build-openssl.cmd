@echo off
setlocal
rem Build MSVC (Windows) OpenSSL 3.0.22 as shared DLLs into F:\Code\openssl-3.0.22-win
rem One-time; after this, the main CMake build finds it via OPENSSL_ROOT_DIR.
rem In-tree build: out-of-tree fails with MSYS perl (srcdir not in @INC).
rem Git perl (native Windows paths) + msys64 modules filled into Git's perl by perlfill.
rem CONFIGURE_INSIST=1: skips windows-checker.pm backslash-path check (Git perl is MSYS-based).
rem no-asm: no NASM on this machine.
rem Targeted nmake (libs only, skip ~700 test programs), then manual install of artifacts.
set "VSVARS=D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
set "PERL=D:\Program Files\Git\usr\bin\perl.exe"
set "SRC=F:\Code\openssl-3.0.22"
set "INST=F:\Code\openssl-3.0.22-win"
set CONFIGURE_INSIST=1

call "%VSVARS%" >nul 2>&1
if errorlevel 1 (echo VCVARS_FAIL & exit /b 9)

cd /d "%SRC%"

rem Always re-run Configure: the tree carries a stale Linux Makefile (case-insensitive FS).
"%PERL%" Configure VC-WIN64A no-asm --prefix="%INST%" --openssldir="%INST%\ssl" > "%SRC%\configure.log" 2>&1
if errorlevel 1 (echo CONFIGURE_FAIL & exit /b 2)

nmake /nologo libcrypto.lib libssl.lib > "%SRC%\nmake.log" 2>&1
if errorlevel 1 (
  rem Fallback: default target (builds tests too, much slower)
  nmake /nologo > "%SRC%\nmake.log" 2>&1
  if errorlevel 1 (echo NMAKE_FAIL & exit /b 3)
)

if not exist "%SRC%\libcrypto.lib" (echo IMPLIB_CRYPTO_MISSING & exit /b 5)
if not exist "%SRC%\libssl.lib" (echo IMPLIB_SSL_MISSING & exit /b 5)
if not exist "%SRC%\libcrypto-3-x64.dll" (echo DLL_CRYPTO_MISSING & exit /b 6)
if not exist "%SRC%\libssl-3-x64.dll" (echo DLL_SSL_MISSING & exit /b 6)

if not exist "%INST%\lib" mkdir "%INST%\lib"
if not exist "%INST%\bin" mkdir "%INST%\bin"
if not exist "%INST%\include" mkdir "%INST%\include"
copy /y "%SRC%\libcrypto.lib" "%INST%\lib\" >nul
copy /y "%SRC%\libssl.lib" "%INST%\lib\" >nul
copy /y "%SRC%\libcrypto-3-x64.dll" "%INST%\bin\" >nul
copy /y "%SRC%\libssl-3-x64.dll" "%INST%\bin\" >nul
xcopy /e /i /y "%SRC%\include\openssl" "%INST%\include\openssl" >nul
if errorlevel 1 (echo HEADER_COPY_FAIL & exit /b 7)

if not exist "%INST%\lib\libssl.lib" (echo LIB_MISSING & exit /b 5)
if not exist "%INST%\bin\libssl-3-x64.dll" (echo DLL_MISSING & exit /b 6)
echo OPENSSL_OK
exit /b 0
