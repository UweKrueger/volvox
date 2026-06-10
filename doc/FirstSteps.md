# First Steps

#### Interactive Mode

To start the interactive interpreter just run `volvox` without parameters. You will get an input prompt indicating the line number. You can type the following example lines followed by Return.

#### Simple Arithmetic — Integer ("`int`")

```volvox
45 / 6
```

`7`

The results of expressions are implicitly printed in interactive mode. Otherwise use "`echo`" to do this explicitly.

#### Simple Arithmetic — Floating Point ("`real`")

```volvox
echo (45. / 6)
```

`7.5`  
`4`

The second line "`4`" is the "result" of the `echo` command — the number of characters printed including the newline character.

Implicit printing of expressions results in interactive mode can be suppressed by starting `volvox` with the option `-fno-print-results` or `-fno-pres`. On the other hand it can be enabled for non-interactive modes with `-fprint-results` or `-fpres`.

The default integer type is `int` (with alias `i32` — 32 bit signed); the default floating point type is `real` (with alias `f64` — 64 bit IEEE 754). In the next example we will explicitly use another type.

To leave the interactive interpreter type:

```volvox
return 0
```

It is also possible to enter an End-Of-File symbol (`Ctrl-D` on Linux / `Crtl-Z Return` on Windows).

### Interpret / Compile a File

Using Volvox interactively can be helpful to tests small fragment of code. For more complex problems it is more suitable to create a code file. Volvox files have the extension `.vx` — You can use your favourite UTF-8 capable text editor to write code. In the directory [editors] there are support files for [GNU Emacs](../editors/emacs) and [Kate](../editors/kate) that provide syntax highlighting and automatic indentation.

#### Functions

Create a file [`faculty.vx`](examples/faculty.vx) with the following content:

```volvox
def faculty(n u64) u64
	if n == 0
		return 1
	else
		return n * faculty(n - 1)
end end

echo faculty 20
```

Functions are defined with the keyword `def` followed by the signature and the return type. Here we use `u64`, i.e. a 64 bit unsigned to get a maximum available valid range for the results. The function implements the usual recursive algorithm to calculate a faculty.

#### Compilation

To compile the file type:

```bash
volvox faculty.vx
```

Prepend the relative or absolute path of your `volvox` binary if is not in your `PATH`.

The command should create an executable file named `faculty` (or `faculty.exe`). If you run the file the result `2432902008176640000` will be printed to `stdout`.

#### Non-Interactive Interpreter

It is not necessary to compile the file to an executable. You cat run the JIT-interpreter with

```bash
volvox -j faculty.vx
```

#### Interactive Interpreter

It is also possible to JIT-compile execute the commands of a file one by one and have a prompt to enter more commands interactively:


```bash
volvox -J faculty.vx
```

This mode can be helpful for debugging purposes — in particular the first lines of a file can be run while there are still errors in the following lines.