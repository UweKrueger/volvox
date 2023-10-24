# Internals
## Heap Object
### Heap Object Types

Volvox supports four kinds of heap objects:

1. `unique` objects: There is only one pointer at any time that points
   to the object &mdash; corresponding to C++'s
   `std::unique_ptr<type>`. When the pointer gets out of scope the
   automatically called destructor frees the memory space. Neither a
   mutex nor a reference counter are needed. A unique object may be
   transferred to another thread. It may also be passed to a C function
   that expects a pointer to a `struct`, but a distiction must be made between
   a *borrow* and a *move* (see below).
2. $\textcolor{green}{\texttt{obj}}$: Several pointers may refer to the object
   but only in one thread.
   When a pointer gets out of scope a reference counter is decremented
   decremented. If it was zero[^1] the object is freed.
3. $\textcolor{red}{\texttt{const}}$ objects: “create once, read everywhere” &mdash; a const object
   may be transferred to another thread. The reference counter is modified
   atomically. 
4. $\textcolor{violet}{\texttt{shared}}$ objects: like `obj` but with an additional mutex that has to be
   locked when the object is accessed. The reference counter is atomic.

[^1]: The reference counter starts with 0 when there is only one pointer. This way `atomic_sub_value()` returns 0 when the last destructor is called indicating that the memory block must be freed.

### C Interoperability

It is possible to pass an object declared `unique`, `const`, `obj` or `shared` to
a C function that expects a pointer to a `struct`:

```C
// Declaration C Header File

typedef struct S {
	double d;
	int i;
} S;

extern int f(S* x);
extern int g(const S* x);

// This function will call "free()" for the passed pointer
// In C++ this would correspond to passing a std::unique_ptr<S> as argument
extern int h(S* x);
```

To use these functions in Volvox code we have to declare the type `S` and
functions `f` and `g` with a matching signatures:

```Volvox
# Declare type

type S {
	d f64
	i int
}

# Declare external C Functions

cdecl f(&x S) int
cdecl g(const x S)

# Create some heap objects

unique a = S{}
obj b = S{}
const c = S{ d: 12.5, i: 3 }
shared d = S{}

# Call functions "borrowing" the objects, i.e. reference counters are not touched

w = f(a)
# "a" is still valid here
x = f(b)
# For the const object c we can only call g 
y = g(c)
# The C function does no know about the mutex so we have to lock it
z = lock d
	f(d)
end
```

The function `h()` can only be called with `a` as argument:

```Volvox
cdecl h(unique S x)

v = h(a)
# a has been moved and is not valid any more
```

For the following tables of detailed data layouts definitions are used:

- $a$ &ndash; address referring the object
- $b$ &ndash; pointer size in bytes (8 on 64-Bit- and 4 on 32-Bit-Systems)
- $n$ &ndash; number of array elements; string length (number of ASCII
  characters &mdash; UTF-8 multi byte characters count as number of bytes they
  consist of)
- $d$ &ndash; aligned size of possible additional data
- $s$ &ndash; array size in bytes - string length including terminating '\\0',
  i.e. $n+1$
- $\lfloor x\rfloor$ &ndash; value $x$ aligned to pointer size by setting last
  bits to $0$, i.e. `x - (x % b)` which can be calculated more efficiently
  using bitwise logic as `x & ~(b-1)`

### Struct Objects

Address | Size | Function
:---: | :---: | ---
$\textcolor{blue}{\textcolor{violet}{\textcolor{green}{a-2\cdot b}-b}-d}$ | $\textcolor{green}{b}$ | $\textsf{\textcolor{red}{Atomic}\ \textcolor{green}{Reference\ Counter}}$
$\textcolor{blue}{\textcolor{violet}{a-2\cdot b}-d}$ | $\textcolor{violet}{b}$ | $\textcolor{violet}{\mathsf{Mutex}}$
$\textcolor{blue}{a-b-d}$ | $\textcolor{blue}{d}$ | $\textcolor{blue}{\mathsf{Other\ Data}}$
$\textcolor{green}{a-b}$ | $\textcolor{green}{b}$ | $\textcolor{green}{\mathsf{Address\ of\ Reference\ Counter,\ Address\ to}\ \mathtt{free()}}$
$a$ | $s$ | Struct Data

### Constant Size Heap Array

Address | Size | Function
:---: | :---: | ---
$\textcolor{blue}{\textcolor{green}{\lfloor a-s\rfloor-\textcolor{violet}{2\cdot \textcolor{green}{b}}}-d}$ | $\textcolor{green}{b}$ | $\textsf{\textcolor{red}{Atomic}\ \textcolor{green}{Reference\ Counter}}$
$\textcolor{blue}{\textcolor{violet}{\lfloor a-s\rfloor -b}-d}$ | $\textcolor{violet}{b}$ | $\textcolor{violet}{\mathsf{Mutex}}$
$\textcolor{blue}{\lfloor a-s\rfloor-d}$ | $\textcolor{blue}{d}$ | $\textcolor{blue}{\mathsf{Other\ Data}}$
$\lfloor a-s\rfloor$ | $s$ | Array Data
$\lfloor a-s\rfloor +s$ | $-s\mod b$ | Padding
$a$ | $b$ | Array Size $s$, i.e. Number of Elements
$\textcolor{green}{a+b}$ | $\textcolor{green}{b}$ | $\textcolor{green}{\mathsf{Address\ of\ Reference\ Counter,\ Address\ to}\ \mathtt{free()}}$

A unique heap array does not include the reference counter nor the mutex or the address of the reference counter (the parts marked green). This allows it to be moved to a C function (as address of the array data) and be freed by that.

## String
There are two types of strings:

- `cstring`: a simple memory pointer that should only be used for C interoperability
- `string`: a Volvox native string — pointer to the size field of the data structure below

## Resizable Shared Heap Arrays

### Control Block

Address | Size | Function
:---: | :---: | ---
$\textcolor{blue}{\textcolor{green}{a-\textcolor{violet}{2\cdot \textcolor{green}{b}}}-d}$ | $\textcolor{green}{b}$ | $\textsf{\textcolor{red}{Atomic}\ \textcolor{green}{Reference\ Counter}}$
$\textcolor{blue}{\textcolor{violet}{a-b}-d}$ | $\textcolor{violet}{b}$ | $\textcolor{violet}{\mathsf{Mutex}}$
$\textcolor{blue}{a-d}$ | $\textcolor{blue}{d}$ | $\textcolor{blue}{\mathsf{Other\ Data}}$
$a$ | $b$ | Pointer to Memory Block
$a+b$ | $b$ | Array Size $s$, i.e. Number of Elements
$a+2\cdot b$ | $b$ | Capacity of Memory Block
$\textcolor{green}{a+3\cdot b}$ | $\textcolor{green}{b}$ | $\textcolor{green}{\mathsf{Address\ of\ Reference\ Counter,\ Address\ to}\ \mathtt{free()}}$


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
