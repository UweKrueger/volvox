# ![Volvox](doc/volvox.svg) Volvox Programming Language

Volvox is a modern programming language designed for clarity, conciseness, and flexibility.

### Key Features:

* **Human-Centric Syntax**: A clean design where punctuation (colons, semicolons, braces, and parentheses) is used only where strictly necessary.
* **Flexible Layout**: Indentation is ignored by the compiler and interpreter, serving as a visual aid for developers rather than a syntactic requirement.
* **Dual Execution Mode**: Source files can be executed instantly via a JIT-interpreter or compiled into high-performance native executables.
* **Strong Static Typing**: Ensures type safety at compile time while supporting polymorphism for flexibility.

### Examples

#### Call Library Functions

```volvox
from math import pi, sin

alpha = pi / 6
echo sin alpha
```

`0.4999999999999999`

Function calls with a single argument do not require parentheses and are evaluated with right-to-left associativity, i.e. "`echo sin alpha`" has the same meaning as "`echo(sin(alpha))`"

#### For Loop and String Interpolation

```volvox
for n in 1..3
	echo "$n * $n = ${n * n}"
end
```

`1 * 1 = 1`  
`2 * 2 = 4`  
`3 * 3 = 9`

#### Multi Level Break

The break condition follows the `brk` command. Multiple `brk` keywords allow jumping across multiple nesting levels.

```volvox
s = 0

for n in 1..7
	for m in 3..5
		s += n
brk brk m == 2*n # Exit both loops when condition is met
		s += m
end end
# multi level break will jump here

echo s
```
`22`

Comments are marked with the hash sign (`#`).

#### Conditions and Initialisation

The conditional syntax is streamlined, removing the need for `then` keywords or semicolons. 

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

`4` (or `2` or `8`)

A variable can be initialized in conditional branches and accessed later — as long as it is initialized in *all* branches. 

### Quick Start

To get started, see [Installation Guide](doc/Install.md) and
[First Steps](doc/FirstSteps.md).  
For information about the `volvox` command line options see
[Invocation](doc/Invocation.md).
