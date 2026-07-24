# First Steps

This file gives some idea of the language. See
[Specification](./Specification.md) for a more complete language
description.

#### Interactive Mode

To start the interactive interpreter open a terminal (or Command Prompt) and
run `volvox` without parameters. (If the location of the volvox-executable is
not included in your PATH environment variable you might have to specify the
directory, e.g. `bin/volvox`, `/my/path/to/bin/volvox` or
`C:\Users\myname\volvox\bin\volvox.exe`)

You will get an input prompt indicating the line number. Type any of the
following example lines followed by Return.

#### Simple Arithmetic — Integer ("`int`")

```volvox
31 / 4
```

*Output:*  
`7`

The results of expressions are implicitly printed in interactive
mode. (see flags `-fprint results` and `-fno-print-results` in
[Invocation](./Invocation.md). Otherwise the "`echo`" command can be used to do this
explicitly as in the following example. The value itself (`7`) is calculated
using integer division, i.e. using an implicit `floor` function ⌊31 / 4⌋.
You might want to look at the flags `-fdiv-floored` and `-fdiv-c99` in 
[Invocation](./Invocation.md) for more details.

#### Simple Arithmetic — Floating Point ("`real`")

If at least one of the operands can be recognized as *floating point number*
the result is calculated with decimal places:

```volvox
echo 31. / 4
```

*Output:*  
`7.75`  
`5`

The second line "`5`" is the "result" of the `echo` command — the
number of characters printed including the newline character.

The default integer type is `int` (with alias `i32` — 32 bit signed);
the default floating point type is `real` (with alias `f64` — 64 bit
IEEE 754). In the next example we will explicitly use another type.

To leave the interactive interpreter type:

```volvox
return 0
```

It is also possible to enter an End-Of-File symbol (`Ctrl-D` on Linux
/ `Crtl-Z Return` on Windows).

### Interpret / Compile a File

Using Volvox interactively can be helpful to test small fragments of
code. For more complex problems it is more suitable to create a code
file. Volvox files have the extension `.vx` — You can use your
favourite UTF-8 capable text editor to write code. In the directory
[editors] there are support files for [GNU Emacs](../editors/emacs)
and [Kate](../editors/kate) that provide syntax highlighting and
automatic indentation.

#### Functions

Create a file [`factorial.vx`](examples/factorial.vx) with the
following content:

```volvox
def factorial(n u64) u64
	if n == 0
		return 1
	else
		return n * factorial(n - 1)
end end

echo factorial 20
```

Functions are defined with the keyword `def` followed by the function name, the signature
and the return type. Here we use `u64`, i.e. a 64 bit unsigned to get
a maximum available valid range for the results. The function
implements the usual recursive algorithm to calculate a factorial.

#### Compilation

To compile the file type:

```bash
volvox factorial.vx
```

Prepend the relative or absolute path of your `volvox` binary if is
not in your `PATH`.

The command should create an executable file named `factorial` (or
`factorial.exe`). If you run the file the result `2432902008176640000`
will be printed to `stdout`.

#### Non-Interactive Interpreter

It is not necessary to compile the file to an executable. You can run
the JIT-interpreter with

```bash
volvox -j factorial.vx
```

#### Interactive Interpreter

It is also possible to JIT-compile execute the commands of a file one
by one and have a prompt to enter more commands interactively:


```bash
volvox -J factorial.vx
```

This mode can be helpful for debugging purposes — in particular the
first lines of a file can be run while there are still errors in the
following lines.

Unless your program finishes with a `return` or `exit` command you are
left with an interactive prompt to enter further commands.

### Input / Output

#### Builtin Functions

In the above examples we have used `echo` to print results to `stdout`
("Standard Output"), i.e. the terminal window. The `echo` function is
a bit special in the sense that it's implementation requires external
C-code.

Besides `echo` there are some similar "special output functions". Here
is a short list:

| Function | Description |
| :--- | :--- |
| `echo` | prints argument(s) to `stdout` separated by spaces and appends `newline` |
| `echon` | prints argument(s) to `stdout` without spaces and without appending 'newline' (like "`echo -n`" in Bash) |
| `echoc` | prints argument(s) as *comma separated list* to `stdout` and appends `newline`. Strings are enclosed with quotation marks |
| `fecho` | like `echo`, but prints to file descriptor specified as first argument |
| `fechoc` | like `echoc`, but prints to file descriptor specified as first argument |
| `fechon` | like `echon`, but prints to file descriptor specified as first argument |

On all supported operating systems `stderr` ("standard error") is associated with file descriptor `2`, i.e. to print an error message you can write:

```volvox
fecho 2, "This is an error!"
```

If you run a program you can redirect `stdout` ("standard output", file descriptor `1`) and `stderr` to separate files:

```bash
my_program >output.txt 2>errors.log
```

To write to a file without redirection you can create a file descriptor using functions from the library `io`:

```volvox
import io

x = 12.75
y = x^2

MyFile = io.create "myfile.txt"
fecho MyFile.fd, "Hello"
fechon MyFile.fd, "$x² = $y\n"
MyFile.close
```

Running this program creates a file `myfile.txt`. You can see the content of this file by typing `cat myfile.txt` (Unix) or `type myfile.txt` (Windows):

```
Hello
12.75² = 162.5625
```

File descriptors represent so called *unbuffered I/O*, i.e. input and output is performed immediatelly[^1]. While this might be what is intended in many situations it can be inefficient for large amounts of data consisting of small chunks. So there is also *buffered I/O* — that's what the next chapter is about.

[^1]: "unbuffered" in this context means that there is no userspace buffering. However there *is* buffering done by the kernel — and hardware devices like hard disks usually have internal buffers ("cache"), too.

#### Functions from Library `file`

To get access to more sophisticated I/O functions you should have a
look into the files in the directory `lib/file`. For our next example
we import types and functions from there:

```volvox
from file import File, stdin, stdout
import math

stdout.write "Program to calculate the hypotenuse of a right-angled triangle\n"
stdout.write "Enter 0 as length of one of the legs to end program.\n\n"

while true
	# prompt for value without appending `newline`
	stdout.write "1. leg: "
	# read `real` value
	a = stdin.readr
brk a == 0
	stdout.write "2. leg: "
	b = stdin.readr
brk b == 0
	c = math.sqrt(a*a + b*b)
	stdout.write "length of hypotenuse: $c\n"
end
```

We have also imported the whole library `math` which contains a large
number of useful mathematical functions. Since we haven't declared
which identifiers to import we must prepend the `sqrt` function with
its library name as qualifier: `math.sqrt`.

Until now we have printed our results to the console. For real world
applications it is necessary to send output to a named file like
"`results.txt`". We have done this before using unbuffered I/O (see [above](#builtin-functions)). This time we use functions from the library `file` for *buffered I/O*:

```volvox
import file
from math import sqrt
from error import strerror

outfile = file.new "results.txt"
if outfile.err != 0
	fecho 2, "Error creating new file \"results.txt\":",
	"${strerror(outfile.err)}"
	exit 1
end

for a in 0.0 .. 10.0
	for b in 0.0 .. 10.0
		c = sqrt(a*a + b*b)
		outfile.write "$a² + $b² = $c²\n"
end end

return 0
```

We use the function `file.new` to create a file named
"`results.txt`". The function name `new` indicates that an error is created if a file with that name already exists. We check for any error and print a readable
error message if there is one. To get this message we call the system
function `strerror` inside the string interpolation. If for example a
file with that has existed before we get a message like

```
Error creating new file "results.txt": File exists
```

It might be desirable to overwrite existing files. In this case we can
use "`file.create`" instead of "`file.new`".

Furthermore calculating the sum of squares might overflow (i.e. become
greater than the maximum supported `real` value) even if none of the
three sides of the triangle would. For this reason there is a special
function `hypot` to calculate the hypotenuse that avoids this overflow.

The file [`hypot4.vx`](examples/hypot4.vx) includes these changes.
