# ![Volvox](doc/volvox.svg) Volvox Programming Language

### Key Features:

* Simple and concise syntax — designed for humans: colons, semicolons, braces and parentheses only where really unavoidable
* Indentation not significant for compiler/interpreter but helpful for humans
* The same source code can be run in the JIT-interpreter or be compiled to a native executable
* Statically typed and polymorphic

### Examples

#### Simple Arithmetic — Integer ("`int`")
```volvox
45 / 6
```
`7`

Result of expressions are implicitly printed in interactive mode. Otherwise use "`echo`" to do this explicitly.

#### Simple Arithmetic — Floating Point ("`real`")
```volvox
echo (45. / 6)
```
`7.5`

#### Call Library Functions

```volvox
from math import pi, sin

alpha = pi / 6
echo sin alpha
```
`0.4999999999999999`

Function calls with one argument don't need parentheses and have right-to-left associativity, i.e. "`echo sin alpha`" has the same meaning as "`echo(sin(alpha))`"

#### For Loop and String Interpolation

Use "`echon`" to suppress implicit quotation marks (and newlines) — similar to "`echo -n`" in `bash`.

```volvox
for n in 1..3
	echon "$n * $n = ${n * n}\n"
end
```
`1 * 1 = 1`  
`2 * 2 = 4`  
`3 * 3 = 9`

#### Multi Level Break

The break condition is placed right behind the `brk` command(s). The default indentation indicates to which level the jump will go.

```volvox
s = 0

for n in 1..7
	for m in 3..5
		s += n
brk brk m == 2*n
		s += m
end end
# multi level break will jump here

echo s
```
`22`

A hash sign (`#`) marks the rest of the line as comment.

#### Conditions

```volvox
x = 23.25

if x < 10
	x += 2
elif x <= 15
	x += 12.75
elif x < 20
	x *= 5
else
	x += 1.5
end

echo x
```
`24.75`

Similar to `bash` — but without semicolon or "`then`".
