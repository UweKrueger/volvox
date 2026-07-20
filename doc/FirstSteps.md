# First Steps

This file gives some idea of the language. See
[Specification](./Specification.md) for a more complete language
description.

#### Interactive Mode

To start the interactive interpreter just run `volvox` without
parameters. You will get an input prompt indicating the line
number. You can type the following example lines followed by Return.

#### Simple Arithmetic — Integer ("`int`")

```volvox
45 / 6
```

`7`

The results of expressions are implicitly printed in interactive
mode. Otherwise use "`echo`" to do this explicitly.

#### Simple Arithmetic — Floating Point ("`real`")

```volvox
echo (45. / 6)
```

`7.5`  
`4`

The second line "`4`" is the "result" of the `echo` command — the
number of characters printed including the newline character.

Implicit printing of expressions results in interactive mode can be
suppressed by starting `volvox` with the option `-fno-print-results`
or `-fno-pres`. On the other hand it can be enabled for
non-interactive modes with `-fprint-results` or `-fpres`.

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

Using Volvox interactively can be helpful to tests small fragment of
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

Functions are defined with the keyword `def` followed by the signature
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

It is not necessary to compile the file to an executable. You cat run
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
| `echo` | prints argument(s) to `stdout` and appends `newline` |
| `echon` | prints argument(s) to `stdout` without appending 'newline' (like "`echo -n`" in Bash) |
| `echoc` | prints argument(s) as *comma separated list* to `stdout` and appends `newline`. Strings are enclosed with quotation marks |
| `eecho` | like `echo`, but prints to `stderr` |
| `eechon` | like `echon`, but prints to `stderr` |

The output `stderr` is like `stdout` but uses file descriptor `2`
instead of `0` and thus can be redirected separately. These special
functions are defined in `builtin.vx`.

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

Until now we always printed our results to the console. For real world
applications it is necessary to redirect output to a named file like
"`results.txt`". In principle we could use shell redirection for
that. Let's go back to out first file `factorial.vx` and compile a
binary:

```bash
volvox factorial.vx
```

Then we can run this binary and use a greater sign to redirect stdout
(file descriptor `1`)to a file:

```bash
./factorial > results.txt
```

It is also possible to redirect `stderr` (file descriptor `2`) this way:

```bash
./factorial 2> errors.txt
```

This works on Windows, too, except that you should write "`.\`"
instead of "`./`".

While this approach is simple and in some way flexible, it is often
desirable to create a file from within a program. Let's have a look at
the next example:

```volvox
import file
from math import sqrt
from error import strerror

outfile = file.new "results.txt"
if outfile.err != 0
	eecho "Error creating new file \"results.txt\": ${strerror(outfile.err)}"
	exit 1
end

for a in 0.0 .. 10.0
	for b in 0.0 .. 10.0
		c = sqrt(a*a + b*b)
		outfile.write "$a² + $b² = $c²\n"
end end

return 0
```

Here we use the function `file.new` to create a new file named
"`results.txt`". We check for an error doing so and print a readable
error message otherwise. To get this message we call the system
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
function `hypot` to calculate the hypotenuse.

The file [`hypot4.vx`](examples/hypot4.vx) includes these changes.
