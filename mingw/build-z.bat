setlocal
set ZLIB_VERSION=1.3.2
set ZSTD_VERSION=1.5.7

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
