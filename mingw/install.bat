@echo off
setlocal
set LLVM_VER=23.1.3
set MINGW_VER=v14.0.0
set GCC_VER=16.2.0
set ZLIB_VERSION=1.3.2
set ZSTD_VERSION=1.5.7

set CLANGLLVMPKG=clang+llvm-%LLVM_VER%-x86_64-pc-windows-msvc.tar.xz
set MINGWPKG=mingw-%MINGW_VER%-stdc++-%GCC_VER%-ucrt.txz
set GCCPKG=gcc-%GCC_VER%-mingw-%MINGW_VER%-ucrt.txz
set ZLIB_DLL=zlib-%ZLIB_VERSION%\zlib1.dll
set ZLIB_DLLLIB=zlib-%ZLIB_VERSION%\zdll.lib
set ZSTD_DLL=zstd-%ZSTD_VERSION%\build\VS_scripts\bin\Release\x64\libzstd.dll
set ZSTD_DLLLIB=zstd-%ZSTD_VERSION%\build\VS_scripts\bin\Release\x64\libzstd.lib
set ZSTD_EXE=zstd-%ZSTD_VERSION%\build\VS_scripts\bin\Release\x64\zstd.exe

if not "%~1"=="" if not "%~1"=="mingw+llvm" if not "%~1"=="gcc+llvm" if not "%~1"=="llvm+mingw" if not "%~1"=="llvm+gcc" if not "%~1"=="mingw" if not "%~1"=="gcc" if not "%~1"=="z" goto opterr

if "%~2"=="" goto compresslibs_or_default
if not "%~2"=="link" goto opterr2
if "%~1"=="mingw" goto ok_arg2
if "%~1"=="llvm+mingw" goto ok_arg2
if "%~1"=="mingw+llvm" goto ok_arg2
echo "link" is only valid for "mingw" and "llvm+mingw"
exit /b 1

:compresslibs_or_default
if "%~1"=="z" goto compresslibs

:ok_arg2
if "%~3"=="" goto ok_arg3
echo superfluous option "%~3"
goto usage

:compresslibs
if exist %ZSTD_EXE% goto z_not_missing
echo file "%%" is missing
echo please run build-z.bat as normal user
exit /b 1

:z_not_missing
if exist "C:\Program Files\LLVM\" goto z_target_avail
echo destination directory name "C:\Program Files\LLVM\bin\" does not exists
exit /b 1

:z_target_avail
echo on
copy %ZLIB_DLL% "C:\Program Files\LLVM\bin\"
copy %ZSTD_DLL% "C:\Program Files\LLVM\bin\"
copy %ZSTD_EXE% "C:\Program Files\LLVM\bin\"
copy %ZLIB_DLLLIB% "C:\Program Files\LLVM\lib\"
copy %ZSTD_DLLLIB% "C:\Program Files\LLVM\lib\"
@exit /b 0

:ok_arg3
if "%~1"=="" goto default
if exist %CLANGLLVMPKG% goto llvm_not_missing
if "%~1"=="mingw" goto llvm_not_missing
if "%~1"=="gcc" goto mingw_not_missing
if "%~1"==
echo package file "%CLANGLLVMPKG%" is missing
echo please download from https://github.com/llvm/llvm-project/releases/
exit /b 1

:llvm_not_missing
if exist %MINGWPKG% goto mingw_not_missing
if "%~1"=="gcc" goto mingw_not_missing
if "%~1"=="gcc+llvm" goto mingw_not_missing
if "%~1"=="llvm+gcc" goto mingw_not_missing
echo package file "%MINGWPKG%" is missing
echo please download from https://github.com/UweKrueger/volvox/releases/
exit /b 1

:mingw_not_missing
if exist %GCCPKG% goto have_all_needed_packages
if "%~1"=="mingw" goto have_all_needed_packages
if "%~1"=="llvm+mingw" goto have_all_needed_packages
if "%~1"=="mingw+llvm" goto have_all_needed_packages
echo package file "%GCCPKG%" is missing
echo please download from https://github.com/UweKrueger/volvox/releases/
exit /b 1

:have_all_needed_packages

if "%~1"=="mingw+llvm" goto llvm
if "%~1"=="gcc+llvm" goto llvm
if "%~1"=="llvm+mingw" goto llvm
if "%~1"=="llvm+gcc" goto llvm
if "%~1"=="mingw" goto mingw
if "%~1"=="gcc" goto gcc
:opterr2
echo unknown option: "%~2"
goto usage
:opterr
if "%~1"=="-h" goto usage
echo unknown option: "%~1"
:usage
echo usage:
echo     %0 [-h] [z^|[llvm+](mingw^|gcc)] [link]
echo.
echo     -h ...... print this help message
echo     z ....... install zlib and zstd components
echo     llvm .... install components to build Volvox compiler
echo     mingw ... install components to use Volvox
echo     gcc ..... install GNU compiler collection in addition to mingw
echo     link .... link llvm-binaries as default, e.g nm.exe -> llvm-nm.exe
echo               (only for "mingw" and "llvm+mingw")
exit /b 1

:llvm
if not exist "C:\Program Files\LLVM\" goto nollvmdir
echo destination directory "C:\Program Files\LLVM\" already exists
exit /b 1
:nollvmdir
if not exist "C:\Program Files\LLVM" goto llvmdiravail
echo destination directory name "C:\Program Files\LLVM" already exists as file
exit /b 1

:llvmdiravail
echo extracting llvm+clang to "C:\Program Files"

tar -x -J -f %CLANGLLVMPKG% -C "C:\Program Files"
@if ERRORLEVEL 1 exit /b %ERRORLEVEL%

rename "C:\Program Files\clang+llvm-%LLVM_VER%-x86_64-pc-windows-msvc" "LLVM"
if ERRORLEVEL 1 exit /b %ERRORLEVEL%
echo LLVM infrastructure successfully installed in "C:\Program Files\LLVM\"

if "%~1"=="mingw+llvm" goto mingw
if "%~1"=="gcc+llvm" goto gcc
if "%~1"=="llvm+mingw" goto mingw
if "%~1"=="llvm+gcc" goto gcc
echo internal error
exit /b 1

:mingw
tar -x -z -f %MINGWPKG% -C "C:\Program Files\LLVM"
if ERRORLEVEL 1 exit /b %ERRORLEVEL%
if not "%~2"=="link" goto setlinker
echo setting often used LLVM binary utilities as default
for %%F in (ar nm objdump ranlib size strings strip) do mklink "C:\Program Files\LLVM\bin\%%F.exe" llvm-%%F.exe
mklink "C:\Program Files\LLVM\bin\cc.exe" clang.exe
mklink "C:\Program Files\LLVM\bin\c++.exe" clang++.exe
goto setlinker

:gcc
tar -x -z -f %GCCPKG% -C "C:\Program Files\LLVM"
if ERRORLEVEL 1 exit /b %ERRORLEVEL%
:: GNU ld does not support Thread Local Storage for mingw; replace in any case
echo removing GNU ld as default linker
del "C:\Program Files\LLVM\bin\ld.exe"

:setlinker
echo setting llvm-lld as default linker
mklink "C:\Program Files\LLVM\bin\ld.exe" ld.lld.exe

:default
endlocal
