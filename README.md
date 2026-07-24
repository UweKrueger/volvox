# ![Volvox](doc/volvox.svg) Volvox Programming Language

Volvox is a modern programming language designed for clarity, conciseness, and flexibility.

### Key Features:

* **Human-Centric Syntax**: A clean design where punctuation (colons, semicolons, braces, and parentheses) is used only where strictly necessary.
* **Flexible Layout**: Indentation is ignored by the compiler and interpreter, serving as a visual aid for developers rather than a syntactic requirement.
* **Dual Execution Mode**: Run code interactively in a REPL or execute source files directly. For production use, compile source files into high-performance native executables.
* **Strong Static Typing**: Ensures type safety at compile time while supporting polymorphism for flexibility.

### Examples

#### Call Library Functions

```volvox
from math import pi, sin

alpha = pi / 6
echo sin alpha
```

*Output:*  
`0.4999999999999999`

Function calls do not require parentheses (unless needed to resolve ambiguities). `echo sin alpha` is equivalent to `echo(sin(alpha))`.

#### For Loop and String Interpolation

```volvox
for n in 1..3
	echo "$n * $n = ${n * n}"
end
```

*Output:*  
`1 * 1 = 1`  
`2 * 2 = 4`  
`3 * 3 = 9`

#### Multi Level Break

The `brk` keyword supports multi-level breaks, allowing you to exit multiple nested loops at once and eliminating the need for `goto` statements. The break condition follows the `brk` keyword(s). With the recommended indentation — supported by the interactive interpreter and various editors — the leftmost `brk` aligns with the loop delimiters for clarity. Example (comments are marked with `#`):


```volvox
s = 0

for n in 1..7
	for m in 3..5
		s += n

		# equivalent to: if m == 2*n then break two levels,
		# i.e., jump to the end of the outer for loop

brk brk m == 2*n

		s += m
end end
# multi level break will jump here

echo s
```

*Output:*  
`22`


#### Conditions and Initialization

The conditional syntax is streamlined, eliminating the need for `then` keywords or semicolons.

```volvox
import rand

# get random floating point number from [0..10)
h = rand.real 10

if h < 5
	if h < 2.5
		x = 2
	else
		x = 4
	end
else
	x = 8
end

# `x` has been initialized in all branches
# so we can read its value
echo x
```

*Output:*  
`4` (or `2` or `8`)

A variable may be declared in conditional branches and used afterward, provided it is assigned in every branch.

### Quick Start

To get started, see [Installation Guide](doc/Install.md) and
[First Steps](doc/FirstSteps.md).  
For information about the `volvox` command line options see
[Invocation](doc/Invocation.md).  
There is also a more detailed [Specification](doc/Specification.md) of the language.
