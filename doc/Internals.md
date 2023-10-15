# Internals and C compatibility
## Shared Heap Array
- p: address in pointer referring the object
- b: pointer size in bytes (8 on 64-Bit- and 4 on 32-Bit-Systems)
- n: Array size in bytes
- s: Array Length including terminating '\\0', i.e. n+1
- \|x\|: x aligned to pointer size by setting last bits to 0, i.e x-(x%b) or x&~(b-1)

Address | Size | Function
:---: | :---: | ---
$\textcolor{green}{\mathsf{\|p-s\|-2b}}$ | $\textcolor{green}{\mathsf{b}}$ | $\textcolor{green}{\mathsf{Atomic~Reference~Counter}}$
$\textcolor{green}{\mathsf{\|p-s\|-b}}$ | $\textcolor{green}{\mathsf{b}}$ | $\textcolor{green}{\mathsf{Mutex}}$
\|p-s\| | s | Array Data
\|p-s\|+s | s%b | Padding
p | b | n Array Size, i.e. Number of Elements
$\textcolor{green}{\mathsf{p+b}}$ | $\textcolor{green}{\mathsf{b}}$ | $\textcolor{green}{\mathsf{Address~of~Reference~Counter,~Address~to~free()}}$

A unique heap array does not include the reference counter nor the mutex or the address of the reference counter (the parts marked green). This allows it to be moved to a C function (as address of the array data) and be freed by that.

## String
There are two types of strings:

- `cstring`: a simple memory pointer that should only be used for C interoperability
- `string`: a Volvox native string — pointer to the size field of the data structure below

### Data Layout
- p: address in pointer referring the object
- b: pointer size in bytes (8 on 64-Bit- and 4 on 32-Bit-Systems)
- n: String Length
- s: Array Length including terminating '\\0', i.e. n+1
- \|x\|: x aligned to pointer size by setting last bits to 0, i.e x-(x%b) or x&~(b-1)

| Address | Size | Function |
| :---: | :---: | --- |
| $\textcolor{green}{\mathsf{\|p-s\|-b}}$ | $\textcolor{green}{\mathsf{b}}$ | $\textcolor{green}{\mathsf{Reference~Counter}}$ |
| \|p-s\| | n | String Characters |
| \|p-s\|+n | 1 | '\\0' String Terminator |
| \|p-s\|+s | s%b | Padding |
| p | b | n+1, i.e. Size of Array |
$\textcolor{green}{\mathsf{p+b}}$ | $\textcolor{green}{\mathsf{b}}$ | $\textcolor{green}{\mathsf{Address~of~Reference~Counter,~Address~to~free()}}$ |

The Address of the reference counter may be lower if the string is part of a larger structure. In any case it is the base address of the heap memory block.

## Resizable Shared Heap Arrays


There is text $\textcolor{red}{\mathrm{text}}$ $\textcolor{green}{More~Text}$

$\textcolor{green}{\mathsf{Even\ More~Text}}$

$x^2$ $\sqrt{x^5}$

$\textcolor{green}{\mathrm{Even\ More~Text}}$

```C++
#include <iostream>

int main(int argc, char* argv[]) {
	printf("Hello world!\n");
}
```
