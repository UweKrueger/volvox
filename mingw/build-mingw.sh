#!/bin/sh
#
BINUTILS_V=2.46.1
GCC_V=16.1.0
MINGW_V=v14.0.0
MAKE_V=4.4.1
GDB_V=17.2
NUM_CORES=8
START_DIR=`pwd`
CONF_PARAMS="--enable-tls --enable-threads=posix --disable-nls --disable-multilib --disable-lib32 --with-default-msvcrt=ucrt"

for mingwcomp in x86_64-w64-mingw32-gcc x86_64-w64-mingw32-cc x86_64-w64-mingw32-as x86_64-w64-mingw32-ld; do
	if comp=$(which $mingwcomp 2>/dev/null); then
		echo "There is already a mingw-compiler in your PATH: $comp"
		echo "Please remove it or make it unaccessible to run this script"
		exit 1
	fi
done

cd $START_DIR
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
if [ ! -f gdb-${GDB_V}.tar.xz ]; then
	wget https://ftp.gnu.org/pub/gnu/gdb/gdb-${GDB_V}.tar.xz
fi
cd ../build
mkdir cross native
cd cross
mkdir binutils mingw-w64-headers mingw-w64 winpthreads gcc gdb
cd ../native
mkdir binutils mingw-w64 gcc winpthreads make
cd ../../install
mkdir cross native
cd ..
tree
cd src
tar xf binutils-${BINUTILS_V}.tar.xz 
tar xf gcc-${GCC_V}.tar.xz 
tar xf mingw-w64-${MINGW_V}.tar.bz2
tar xf make-${MAKE_V}.tar.lz
tar xf gdb-${GDB_V}.tar.xz
rm -rf make
mv make-${MAKE_V} make
rm -rf binutils
mv binutils-${BINUTILS_V} binutils
rm -rf gdb
mv gdb-${GDB_V} gdb
rm -rf gcc
mv gcc-${GCC_V} gcc
rm -rf mingw-w64
mv mingw-w64-${MINGW_V} mingw-w64
cd gcc/
contrib/download_prerequisites 
cd $START_DIR/mingw-builds/build/cross/binutils
../../../src/binutils/configure --prefix=$START_DIR/mingw-builds/install/cross --target=x86_64-w64-mingw32 --enable-lto --enable-plugins --disable-werror ${CONF_PARAMS}
make -j$NUM_CORES
make install
export PATH="${START_DIR}/mingw-builds/install/cross/bin:$PATH"
cd $START_DIR/mingw-builds/build/cross/mingw-w64-headers
../../../src/mingw-w64/mingw-w64-headers/configure --prefix=$START_DIR/mingw-builds/install/cross/x86_64-w64-mingw32
make install
cd $START_DIR/mingw-builds/build/cross/gcc
../../../src/gcc/configure --prefix=$START_DIR/mingw-builds/install/cross --target=x86_64-w64-mingw32 --enable-shared --enable-static --enable-languages=c,c++ ${CONF_PARAMS}
make -j$NUM_CORES all-gcc
make install-gcc
cd $START_DIR/mingw-builds/build/cross/mingw-w64
../../../src/mingw-w64/configure --host=x86_64-w64-mingw32 --prefix=$START_DIR/mingw-builds/install/cross/x86_64-w64-mingw32 ${CONF_PARAMS}
make
make install
cd $START_DIR/mingw-builds/build/cross/winpthreads
../../../src/mingw-w64/mingw-w64-libraries/winpthreads/configure --prefix=$START_DIR/mingw-builds/install/cross/x86_64-w64-mingw32 --host=x86_64-w64-mingw32 --enable-static --enable-shared ${CONF_PARAMS}
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
../../../src/binutils/configure --prefix=$START_DIR/mingw-builds/install/native --host=x86_64-w64-mingw32 --target=x86_64-w64-mingw32 --enable-static --enable-shared ${CONF_PARAMS}
make -j$NUM_CORES
make install
cd $START_DIR/mingw-builds/build/native/gcc
../../../src/gcc/configure --prefix=$START_DIR/mingw-builds/install/native --host=x86_64-w64-mingw32 --target=x86_64-w64-mingw32  --enable-shared --enable-static --enable-languages=c,c++ ${CONF_PARAMS}
make -j$NUM_CORES
make install
cd $START_DIR/mingw-builds/build/native/mingw-w64
../../../src/mingw-w64/configure --host=x86_64-w64-mingw32 --prefix=$START_DIR/mingw-builds/install/native/x86_64-w64-mingw32 ${CONF_PARAMS}
make -j$NUM_CORES
make install
cd $START_DIR/mingw-builds/build/native/winpthreads
../../../src/mingw-w64/mingw-w64-libraries/winpthreads/configure --prefix=$START_DIR/mingw-builds/install/native --host=x86_64-w64-mingw32 --enable-static --enable-shared ${CONF_PARAMS}
make install
cd $START_DIR/mingw-builds/build/native/binutils
../../../src/binutils/configure --prefix=$START_DIR/mingw-builds/install/native --host=x86_64-w64-mingw32 --target=x86_64-w64-mingw32 --enable-static --enable-shared ${CONF_PARAMS}
make -j$NUM_CORES
make install
cd $START_DIR/mingw-builds/build/native/gcc
../../../src/gcc/configure --prefix=$START_DIR/mingw-builds/install/native --host=x86_64-w64-mingw32 --target=x86_64-w64-mingw32  --enable-shared --enable-static --enable-languages=c,c++ ${CONF_PARAMS}
make -j$NUM_CORES
make install
mv $START_DIR/mingw-builds/install/native/lib/libgcc_s_seh-1.dll $START_DIR/mingw-builds/install/native/bin/
cd $START_DIR
mv $START_DIR/mingw-builds/install/native/x86_64-w64-mingw32/bin/libwinpthread-1.dll $START_DIR/mingw-builds/install/native/bin/
x86_64-w64-mingw32-gcc -o ldd ldd.c -O2 -lpsapi
mv ldd.exe $START_DIR/mingw-builds/install/native/bin/
cd $START_DIR/mingw-builds/src/make
patch -p1 < $START_DIR/make_arglists.diff
cd $START_DIR/mingw-builds/build/native/make
../../../src/make/configure --host=x86_64-w64-mingw32 --disable-nls --without-guile
make -j$NUM_CORES
mv make.exe $START_DIR/mingw-builds/install/native/bin/
cd $START_DIR/mingw-builds/build/cross/gdb
../../../src/gdb/configure --host=x86_64-w64-mingw32 --target=x86_64-w64-mingw32 ${CONF_PARAMS} --with-gmp-include=$START_DIR/mingw-builds/build/native/gcc/gmp --with-gmp-lib=$START_DIR/mingw-builds/build/native/gcc/gmp/.libs --with-mpfr-include=$START_DIR/mingw-builds/src/gcc/mpfr-4.2.2/src --with-mpfr-lib=$START_DIR/mingw-builds/build/native/gcc/mpfr/src/.libs --with-mpc-include=$START_DIR/mingw-builds/src/gcc/mpc-1.3.1/src --with-mpc-lib=$START_DIR/mingw-builds/build/native/gcc/mpc/src/.libs --disable-multilib --disable-werror
make -j$NUM_CORES
mv -v $START_DIR/mingw-builds/build/cross/gdb/gdb/.libs/gdb.exe $START_DIR/mingw-builds/install/native/bin/
mv -v $START_DIR/mingw-builds/build/cross/gdb/gdbserver/gdbserver.exe $START_DIR/mingw-builds/install/native/bin/
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
rm -f bin/ld.exe
mv -v include/c++/${GCC_V} x86_64-w64-mingw32/include/c++
bsdtar -czf $START_DIR/mingw-${MINGW_V}-stdc++-${GCC_V}-ucrt.tgz bin/*.dll bin/ldd.exe bin/make.exe bin/gdb.exe x86_64-w64-mingw32 lib/*.a lib/*.la
zcat $START_DIR/gcc-${GCC_V}-mingw-${MINGW_V}-ucrt.tgz | xz -9 > $START_DIR/gcc-${GCC_V}-mingw-${MINGW_V}-ucrt.txz
zcat $START_DIR/mingw-${MINGW_V}-stdc++-${GCC_V}-ucrt.tgz | xz -9 > $START_DIR/mingw-${MINGW_V}-stdc++-${GCC_V}-ucrt.txz
