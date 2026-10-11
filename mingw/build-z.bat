setlocal
set ZLIB_VERSION=1.3.2
set ZSTD_VERSION=1.5.7

set ZLIB_DLL=zlib-%ZLIB_VERSION%\zlib1.dll
set ZLIB_DLLLIB=zlib-%ZLIB_VERSION%\zdll.lib
set ZSTD_DLL=zstd-%ZSTD_VERSION%\build\VS_scripts\bin\Release\x64\libzstd.dll
set ZSTD_DLLLIB=zstd-%ZSTD_VERSION%\build\VS_scripts\bin\Release\x64\libzstd.lib
set ZSTD_EXE=zstd-%ZSTD_VERSION%\build\VS_scripts\bin\Release\x64\zstd.exe

curl -O -L https://github.com/madler/zlib/releases/download/v%ZLIB_VERSION%/zlib-%ZLIB_VERSION%.tar.xz
tar -xf zlib-%ZLIB_VERSION%.tar.xz
cd zlib-%ZLIB_VERSION%
nmake -f win32/Makefile.msc
cd ..

curl -O -L https://github.com/facebook/zstd/releases/download/v%ZSTD_VERSION%/zstd-%ZSTD_VERSION%.tar.zst
tar -xf zstd-%ZSTD_VERSION%.tar.zst
cd zstd-%ZSTD_VERSION%\build\VS_scripts
build.generic.cmd VS2022 x64 Release v143
cd ..\..\..

copy %ZLIB_DLL% ..\bin\
copy %ZSTD_DLL% ..\bin\
copy %ZSTD_EXE% ..\bin\
copy %ZLIB_DLLLIB% ..\lib\volvox\
copy %ZSTD_DLLLIB% ..\lib\volvox\
