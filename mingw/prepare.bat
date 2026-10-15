@echo off
setlocal
set LLVM_VER=23.1.3
set MINGW_VER=v14.0.0
set GCC_VER=16.2.0
set VOLVOX_VER=0.01

set LLVM_FILE=clang+llvm-%LLVM_VER%-x86_64-pc-windows-msvc.tar.zst
set LLVM_URL=https://github.com/llvm/llvm-project/releases/download/llvmorg-%LLVM_VER%/%LLVM_FILE%
set LLVM_MSI_FILE=LLVM-%LLVM_VER%-win64.msi
set LLVM_MSI_URL=https://github.com/llvm/llvm-project/releases/download/llvmorg-%LLVM_VER%/%LLVM_MSI_FILE%
set VOLVOX_FILE=volvox-Windows-%VOLVOX_VER%.txz
set VOLVOX_URL=https://github.com/UweKrueger/volvox/releases/download/v%VOLVOX_VER%/%VOLVOX_FILE%
set GCC_FILE=gcc-%GCC_VER%-mingw-%MINGW_VER%-ucrt.txz
set GCC_URL=https://github.com/UweKrueger/volvox/releases/download/v%VOLVOX_VER%/%GCC_FILE%

if not "%~1"=="" if not "%~1"=="devel" goto opterr
if "%~1"=="" goto no_devel
if not "%~2"=="" goto opterr2

echo Downloading %LLVM_FILE%
curl -O -L -f %LLVM_URL%
if ERRORLEVEL 1 goto error_download
echo ... success
goto llvm_msi

:no_devel
echo Downloading %VOLVOX_FILE%
curl -O -L -f %VOLVOX_URL%
if ERRORLEVEL 1 goto error_download
echo ... success

:llvm_msi
echo Downloading %LLVM_MSI_FILE%
curl -O -L -f %LLVM_MSI_URL%
if ERRORLEVEL 1 goto error_download
echo ... success
echo.

echo Starting LLVM installer
.\%LLVM_MSI_FILE%
if ERRORLEVEL 1 goto error_install
exit /b 0

:error_install
echo Installation failed 1>&2
exit /b 1

:have_error
echo error downloading
echo This is error line 2 1>&2
exit /b 1

:opterr2
echo superfluous option "%~2" 1>&2
goto usage

:opterr
if "%~1"=="-h" goto usage
echo unknown option: "%~1" 1>&2

:usage
echo usage:
echo     %0 [-h] [devel]
echo.
echo     -h ...... print this help message
echo     devel ... download packages needed to develop Volvox compiler itself
if "%~1"=="-h" exit /b 0
exit /b 1

:error_download
echo ... failed 1>&2
exit /b 1
