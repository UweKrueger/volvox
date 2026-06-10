# Build and Install

### Build on Linux

To build and run Volvox on Linux you need GNU `make`, the LLVM development packages, `clang` and the BSD editline development packages. To install them e.g. on Ubuntu type
```bash
sudo apt install make clang llvm-dev libedit-dev
```
Change to the `volvox` directory and type
```bash
make -j8
```

### Build on Windows

To compile and run Volvox on Windows, you need the LLVM packages and a supporting compiler infrastructure. While Microsoft Visual Studio can be used, the recommended approach is to use `clang`, `mingw`, and a selection of GNU tools. Please note that **Microsoft Visual Studio must still be installed** to **build** Volvox[^1] even though it's no runtime requirement.

#### 1. Setting up the Toolchain (Mingw)

The most efficient way to obtain a compatible `mingw` environment is via cross-compilation on a Linux machine. 

1. Unpack the Volvox sources on your Linux machine.
2. Navigate to the `mingw` directory and run the build script:
   
```bash
cd volvox/mingw
./build-mingw.sh
```

Upon successful completion, two archive files will be created in this directory: `gcc-14.3.0-mingw-v14.0.0-ucrt.tgz` and `mingw-v14.0.0-stdc++-14.3.0-ucrt.tgz`. Additionally, download the LLVM binary archive:

```bash
wget https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.7/clang+llvm-22.1.7-x86_64-pc-windows-msvc.tar.xz
```

#### 2. Toolchain Installation on Windows

Copy all three archives to your Windows system. Unpack the Volvox sources and move the archives into the `volvox\mingw` directory.

You can choose whether to install `gcc` in addition to `mingw`. While `gcc` is not required for Volvox, it is recommended for general development. To proceed with the installation:

1. Open a command prompt with **Administrator privileges**.
2. Navigate to the `volvox\mingw` directory.
3. Run the installation batch file (use `llvm+gcc` for LLVM, mingw and gcc, or just `llvm+mingw` for LLVM and mingw only):

```cmd
install-3rd.bat llvm+gcc
```

This script installs LLVM, Mingw, `make.exe` (GNU Make), and `ldd.exe` (a standalone version of `ldd`). 

**Important:** Add `C:\Program Files\LLVM\bin` to your system `PATH` environment variable.

#### 3. Provide Bash

The `Makefile` uses the `bash` shell which requires a POSIX layer and thus can't be build with `mingw`.  
The easiest way to get a working `bash.exe` is by installing Git for Windows.

#### 4. Building Volvox

Once the toolchain is configured, you can build Volvox:

1. Open the **x64 Native Tools Command Prompt for VS 2022**.
2. Change the directory to the `volvox` root folder.
3. Execute the build command:

```cmd
make -j8
```

[^1]: The resulting `volvox.exe` is an `msvc` binary but it can create both `ming` binaries (using the toolchain that is created by `build-mingw.sh`) and `msvc` binaries (using libraries from the Microsoft Visual Studio installation if present).

### Build on BSD

The installation on FreeBSD, NetBSD and OpenBSD is similar to the Linux installation, except that you usually have to install `bash` and `gmake` in addition to the requirements above. The command to build Volvox is

```bash
gmake -j8
```

Please note that BSD systems are not the main development platform for Volvox, so there may be issues. In particular on OpenBSD the compiler and interpreter (or to be more precise: the LLVM backend) might produce illegal instructions (at least for real hardware with newer CPUs).

### Installation

When building was successful there should be an executable named "`volvox.exe`" (on Windows) or "`volvox`" (on Unix systems).

There is no "`make install`" but to test Volvox it is not necessary to install anything. Just call the `volvox` executable with fully qualified path.

However, if you want to install the compiler, you can copy the executable to some `bin` directory that is part of your `PATH` and copy the `lib` directory parallel to that `bin` directory.
