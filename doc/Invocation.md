# ![Volvox](volvox.svg) Volvox Compiler / Interpreter Usage

### Invocation

The following assumes that the `volvox` executable file is located in a
directory that is included the `PATH` environment variable. Otherwise prepend
that directory, e.g. `./volvox` or `/my/path/volvox` (Unix) or
`C:\Users\myname\mydir\volvox.exe` (Windows).

```
volvox [-h] [-v] [-c] [-g] [-r] [-j] [-J] [-d] [-D] [-M <mainf>] [-f<flag>] \
       [-emit-llvm] [-O<n>[<m]] [-i <file>] [-o <file>] [-strip] [-s size] \
       [-no-<flag>] [-t] [-C <n,g,b>] <file> ...
```

- `-h`: print help summary

`-v`
: be verbose, i.e. show configuration information and invocations of external
tools like system compiler or linker
