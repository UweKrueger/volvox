# ![Volvox](volvox.svg) Volvox Compiler / Interpreter Usage

### Invocation

The following assumes that the `volvox` executable file is located in a
directory that is included the `PATH` environment variable. Otherwise prepend
that directory, e.g. `./volvox` or `/my/path/volvox` (Unix) or
`C:\Users\myname\mydir\volvox.exe` (Windows).

```sh
volvox [-h] [-v] [-c] [-g] [-r] [-j] [-J] [-d] [-D] [-M <mainf>] [-f<flag>] \
       [-emit-llvm] [-O<n>[<m]] [-i <file>] [-o <file>] [-strip] [-s size] \
       [-m<target>] [-no-<flag>] [-t] [-C <n,g,b>] [<file> [...]]
```

- `-h`: print help summary
- `-v`: be verbose, i.e. show configuration information and invocations of
        external tools like system compiler or linker
- `-c`: compile only, i.e. don't link. Unless `-o` is given the result of
        compiling `filename.vx` will be an object file named `filename.o`
        or `filename.obj`. Statements outside of defined functions will
        be placed in a function called `main` unless `-M` is given.
- `-g`: create debug information. These are meant to debug the program with
        `lldb` or `gdb`. Currently debug information is only created for
        fixed size objects, i.e. basic types, structs and fixed size arrays.
        Dynamic objects might require special support in the debugger.
        On Windows only the `mingw` target is supported, but (thread local)
        `global` variables are not visible (but that's not specific to
        Volvox).
- `-r`: compile the program and run it immediately (consider `-j` instead)
- `-j`: JIT compile and run program
- `-J`: JIT compile and run statement by statement as if those had been
        entered interactively. This is significantly slower than `-j` but
        will leave a prompt to enter further commands (unless the last
        statement was `return` or `exit`).
- `-d`, `-D`: dump intermediate LLVM IR code at different stages. Only useful
        to debug the Volvox compiler itself. Consider `-emit-llvm` instead.
- `-M <mainfn>`: use `<mainfn> for statements outside of function definitions
        (default: `main). Useful in combination with `-c` when manual
        linking is done
- `-f<flag>`: the following flags are supported:
    - `-fPIC`: generate position independent code suitable to create a shared
      object (`.so.*`)
    - `-fdiv-floored`: signed integer division is floored; the remainder gets
      sign of the divisor (this is the default with Volvox). Example:  
      `-13 / 4` &rarr; `-4`  
      `-13 % 4` &rarr; `3`  
      `13 / -4` &rarr; `-4`  
      `13 % -4` &rarr; `-3`
    - `-fdiv-c99`: signed division rounds towards 0, the remainder gets the
      sign of the divident (this is the default with C99 and later — with C89
      the behaviour was undefined). Example:  
      `-13 / 4` &rarr; `-3`  
      `-13 % 4` &rarr; `-1`  
      `13 / -4` &rarr; `-3`  
      `13 % -4` &rarr; `1`  

      In all cases (i.e. with both `-fdiv-floored` and `-fdiv-c99`) the
      following equation is always fulfilled:  
      `a = b * (a/b) + (a%b)`  
      In practice integer divisions should not involve negative numbers
      to avoid confusions.
