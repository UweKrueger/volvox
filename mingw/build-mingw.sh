#!/bin/sh
#
BINUTILS_V=2.41
GCC_V=14.2.0
MINGW_V=v12.0.0
MAKE_V=4.4.1
CONF_PARAMS="--enable-tls --enable-threads=win32 --disable-nls --disable-lib32 --with-default-msvcrt=ucrt"
export NUM_CORES=8
export START_DIR=`pwd`
mkdir -p mingw-builds
cd mingw-builds
mkdir -p src build install
cd src
# mkdir binutils mingw-w64 gcc
if [ ! -f binutils-${BINUTILS_V}.tar.xz ]; then
	wget https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_V}.tar.xz
fi
if [ ! -f gcc-${GCC_V}.tar.xz ]; then
	wget https://ftp.gnu.org/gnu/gcc/gcc-${GCC_V}/gcc-${GCC_V}.tar.xz
fi
if [ ! -f mingw-w64-${MINGW_V}.tar.bz2 ]; then
	wget https://sourceforge.net/projects/mingw-w64/files/mingw-w64/mingw-w64-release/mingw-w64-${MINGW_V}.tar.bz2
fi
if [ ! -f make-${MAKE_V}.tar.lz ]; then
	wget https://ftp.gnu.org/pub/gnu/make/make-${MAKE_V}.tar.lz
fi
cd ../build
mkdir cross native
cd cross
mkdir binutils mingw-w64-headers mingw-w64 gcc
cd ../native
mkdir binutils mingw-w64 gcc make
cd ../../install
mkdir cross native
cd ..
tree
cd src
tar xf binutils-${BINUTILS_V}.tar.xz 
tar xf gcc-${GCC_V}.tar.xz 
tar xf mingw-w64-${MINGW_V}.tar.bz2
tar xf make-${MAKE_V}.tar.lz
mv make-${MAKE_V} make
mv binutils-${BINUTILS_V} binutils
mv gcc-${GCC_V} gcc
mv mingw-w64-${MINGW_V} mingw-w64
cd gcc/
contrib/download_prerequisites 
cd $START_DIR/mingw-builds/build/cross/binutils
../../../src/binutils/configure --prefix=$START_DIR/mingw-builds/install/cross --target=x86_64-w64-mingw32 --disable-multilib ${CONF_PARAMS}
make -j$NUM_CORES
make install
export PATH="${START_DIR}/mingw-builds/install/cross/bin:$PATH"
cd $START_DIR/mingw-builds/build/cross/mingw-w64-headers
../../../src/mingw-w64/mingw-w64-headers/configure --host=x86_64-w64-mingw32 --prefix=$START_DIR/mingw-builds/install/cross/x86_64-w64-mingw32 ${CONF_PARAMS}
make install
cd $START_DIR/mingw-builds/build/cross/gcc
../../../src/gcc/configure --prefix=$START_DIR/mingw-builds/install/cross --target=x86_64-w64-mingw32 --disable-multilib --enable-languages=c,c++ ${CONF_PARAMS}
make -j$NUM_CORES all-gcc
make install-gcc
cd $START_DIR/mingw-builds/build/cross/mingw-w64
../../../src/mingw-w64/configure --host=x86_64-w64-mingw32 --prefix=$START_DIR/mingw-builds/install/cross/x86_64-w64-mingw32 ${CONF_PARAMS}
make
make install
cd $START_DIR/mingw-builds/build/cross/gcc
make -j$NUM_CORES
make install
which x86_64-w64-mingw32-gcc
x86_64-w64-mingw32-gcc --version
echo -ne '#include <windows.h>\n#include <stdio.h>\nint main() {printf("Hello\\n"); return 0;}\n' | x86_64-w64-mingw32-gcc -xc -
ls -l
file a.exe
cd $START_DIR/mingw-builds/build/native/binutils
../../../src/binutils/configure --prefix=$START_DIR/mingw-builds/install/native --host=x86_64-w64-mingw32 --target=x86_64-w64-mingw32 --disable-multilib ${CONF_PARAMS}
make -j$NUM_CORES
make install
cd $START_DIR/mingw-builds/build/native/gcc
../../../src/gcc/configure --prefix=$START_DIR/mingw-builds/install/native --host=x86_64-w64-mingw32 --target=x86_64-w64-mingw32 --disable-multilib --enable-languages=c,c++ ${CONF_PARAMS}
make -j$NUM_CORES
make install
mv $START_DIR/mingw-builds/install/native/lib/libgcc_s_seh-1.dll $START_DIR/mingw-builds/install/native/bin/
cd $START_DIR/mingw-builds/build/native/mingw-w64
../../../src/mingw-w64/configure --host=x86_64-w64-mingw32 --prefix=$START_DIR/mingw-builds/install/native/x86_64-w64-mingw32 --with-libraries=winpthreads ${CONF_PARAMS}
make
make install
mv $START_DIR/mingw-builds/install/native/x86_64-w64-mingw32/bin/libwinpthread-1.dll $START_DIR/mingw-builds/install/native/bin/
cd $START_DIR
x86_64-w64-mingw32-gcc -o ldd ldd.c -O2 -lpsapi
mv ldd.exe $START_DIR/mingw-builds/install/native/bin/
cd $START_DIR/mingw-builds/build/native/make
../../../src/make/configure --host=x86_64-w64-mingw32 --disable-nls --without-guile
make
x86_64-w64-mingw32-strip --strip-debug make.exe
mv make.exe $START_DIR/mingw-builds/install/native/bin/
cd $START_DIR/mingw-builds/install/native/bin
x86_64-w64-mingw32-strip --strip-debug *
cd $START_DIR/mingw-builds/install/native/lib/bfd-plugins
x86_64-w64-mingw32-strip --strip-debug *
cd $START_DIR/mingw-builds/install/native/lib/gcc/x86_64-w64-mingw32/${GCC_V}
mkdir -p ../../../../x86_64-w64-mingw32/mingw/lib
x86_64-w64-mingw32-strip --strip-debug *.o *.a
cp -p libgcc*.a ../../../../x86_64-w64-mingw32/mingw/lib
cd $START_DIR/mingw-builds/install/native/libexec/gcc/x86_64-w64-mingw32/${GCC_V}/install-tools
x86_64-w64-mingw32-strip --strip-debug *.exe
cd ..
x86_64-w64-mingw32-strip --strip-debug *.exe *.dll *.a
cd $START_DIR/mingw-builds/install/native/x86_64-w64-mingw32/lib
x86_64-w64-mingw32-strip --strip-debug *.a
cd ../..
bsdtar -czf $START_DIR/gcc-${GCC_V}-mingw-${MINGW_V}-ucrt.tgz bin include lib libexec share x86_64-w64-mingw32
bsdtar -czf $START_DIR/mingw-${MINGW_V}-ucrt.tgz bin/*[a-z]-[0-9].dll bin/ldd.exe bin/make.exe x86_64-w64-mingw32
