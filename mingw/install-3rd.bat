@echo off
setlocal
set LLVM_VER=18.1.8
set MINGW_VER=v12.0.0
set GCC_VER=14.2.0

set CLANGLLVMPKG=clang+llvm-%LLVM_VER%-x86_64-pc-windows-msvc.tar.xz
set MINGWPKG=mingw-%MINGW_VER%-stdc++-%GCC_VER%-ucrt.tgz
set GCCPKG=gcc-%GCC_VER%-mingw-%MINGW_VER%-ucrt.tgz

:: find out OS Version
for /f "tokens=4-5 delims=[.] " %%i in ('ver') do set OSVER=%%i.%%j

if not "%~1"=="" if not "%~1"=="mingw+llvm" if not "%~1"=="gcc+llvm" if not "%~1"=="llvm+mingw" if not "%~1"=="llvm+gcc" if not "%~1"=="mingw" if not "%~1"=="gcc" goto opterr

if "%~2"=="" goto ok_arg2
if not "%~2"=="link" goto opterr2
if "%~1"=="mingw" goto ok_arg2
if "%~1"=="llvm+mingw" goto ok_arg2
if "%~1"=="mingw+llvm" goto ok_arg2
echo "link" is only valid for "mingw" and "llvm+mingw"
exit /b 1

:ok_arg2
if "%~3"=="" goto ok_arg3
echo superfluous option "%~3"
goto usage

:ok_arg3
if "%~1"=="" goto default
if exist %CLANGLLVMPKG% goto llvm_not_missing
if "%~1"=="mingw" goto llvm_not_missing
if "%~1"=="gcc" goto mingw_not_missing
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
echo unknown option: "%~1"
:usage
echo usage:
echo     %0 [[llvm+](mingw^|gcc)] [link]
echo
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
if "%OSVER%"=="10.0" (
    echo detected Windows 10 - trying 7-Zip + native tar as extractor
	if exist "C:\Program Files\7-Zip\7z.exe" (
	    7z x -so %CLANGLLVMPKG% | tar -x -f - -C "C:\Program Files"
		if ERRORLEVEL 1 exit /b %ERRORLEVEL%
	) else (
	    echo 7-Zip not found - please install it from https://www.7-zip.org
		exit /b 1
	)
) else (
    echo detected Windows 11 - using native "tar" as extractor
    tar -x -J -f %CLANGLLVMPKG% -C "C:\Program Files"
    @if ERRORLEVEL 1 exit /b %ERRORLEVEL%
)
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
