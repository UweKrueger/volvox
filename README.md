# ![Volvox](doc/volvox.svg) Volvox Programming Language

### Key Features:

* Simple and concise syntax — designed for humans
* The same source code can be run in the JIT-interpreter or be compiled to a native executable
* Statically typed and polymorphic

### Examples

#### Simple Arithmetic - Integer ("`int`")
```volvox
45 / 6
```
`7`

#### Floating Point ("`real`")
```volvox
45. / 6
```
`7.5`

#### Call Library Functions

```volvox
from math import pi, sin

alpha = pi / 6
sin alpha
```
`0.4999999999999999`

#### For Loop and String Interpolation

```volvox
for n in 1..3
	printn "$n * $n = ${n * n}\n"
end
```
`1 * 1 = 1`  
`2 * 2 = 4`  
`3 * 3 = 9`

#### Multi Level Break

```volvox
s = 0
for n in 1..7
	for m in 3..5
		s += n
brk brk m == 2*n
		s += m
end end
print s
```
`22`
