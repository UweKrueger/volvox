### Installation on Windows

In order to compile and run Volvox on Windows you need the LLVM packages and a compiler infrastructure. While it is in principle possible to use Microsoft Visual Studio for the latter the recommended way is to use `clang`, `mingw` and some GNU tools to compile and run Volvox. However it is still necessary to have Microsoft Visual Studio installed.

#### 1. Compile Mingw

The easiest way to compile `mingw` is to do a cross compilation on a Linux machine. Unpack the Volvox sources and go to the `mingw` directory and run the build script:
 
```bash
cd volvox/mingw
./build-mingw.sh
```

If everything went fine there should be two archive files in this directory: `gcc-14.3.0-mingw-v14.0.0-ucrt.tgz` and `mingw-v14.0.0-stdc++-14.3.0-ucrt.tgz`. You should also get the LLVM binary archive:

```bash
wget https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.7/clang+llvm-22.1.7-x86_64-pc-windows-msvc.tar.xz
```

Copy all three archives to your Windows system. Unpack the Volvox sources there and move the archives to the directory `volvox\mingw`.

Now you have to make a decision if you want to install `gcc` in addition to `clang`. It is not needed by Volvox but might be nice to have so that's the option shown here. Become `Administrator`, go to the directory `volvox\mingw` and run the installation batch file:

```cmd
install-3rd.bat llvm+gcc
```

In addition to LLVM and Mingw this also installs `make.exe` (GNU make) and `ldd.exe` (an `ldd` version that does not depend on MSYS or Cygwin). Add `C:\Program Files\LLVM\bin` to your `PATH` environment variable.

Now you should be able to build Volvox. Open an `x64 Native Tools Command Prompt for VS 2022` and change to the `volvox` directory. Run:

```cmd
make -j8
```

