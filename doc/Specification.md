# ![Volvox](volvox.svg) Volvox Programming Language Specification

## Basic Syntax Elements

```EBNF
digit = "0" ... "9"
positive_digit = "1" ... "9"
upper-case = "A" ... "Z"
lower-case = "a" ... "z"
letter = upper-case | lower-case | "_"
alphanum = digit | letter

identifier = letter [alphanum*]

octal_digit   = "0" ... "7"
hex_digit     = digit | "A" ... "F" | "a" ... "f"

decimal_unsigned = "0" | (positive_digit [digit*])
decimal_signed = ["-"] decimal_unsigned

hexadecimal = "0x" hex_digit [hex_digit*]
octal =  "0" octal_digit [octal_digit*]

exponent = ("E"|"e") decimal_signed
real = decimal_signed ("." [decimal_unsigned] [exponent]) | exponent
(remark: second dot as in 12.. would be beginning of range)
float = real "f"
signed = decimal_signed
unsigned = decimmal_unsigned | hexadecimal | octal
```

## Operator Hierarchy

| Operator(s) | meaning | associativity |
| :--- | :--- | :--- |
| `.` | Selector (`struct.field`, `module.ident`) | left |
| unary `&` | Address (for C calls) | prefix |
| `++`, `--` | Increment, Decrement — return old value | postfix |
| `^` | power (e.g. `a^2`) | right |
| unary `+`, `-`, `!`, `~` | signs, logical / bitwise not | prefix |
|  | invisible operator in `echo sin x` | right |
| `*`, `/`, `%`, `<<`, `>>` | multiply, divide, modulo, bitwise shifts | left |
| `+`, `-` | binary plus, minus | left |
|  `>=`, `>`, `==`, `!=`, `<`, `<=` | Comparisons | left |
| `&` | bitwise and | left |
| `><` | bitwise exclusive or | left |
| `\|` | bitwise or | left |
| `&&` | logical and | left |
| `\|\|` | logical or | left |
| `..` | range (`for n in 1..3`) | left |
| `:` | field element assignment | left |
| `? :` | ternary expression | left |
| `,` | list element separator | left |
| `=`, `+=`, `-=`, `*=`, `/=` | assignments, return old value | right |

## Builtin Data Types
### Numeric Types
#### Integer Types

| Name (Alias) | Range / Spec | Suffix |
| :--- | :--- | :--- |
| `i8` | -128 .. 127 | `hh`, `HH` |
| `i16` | -32768 .. 32767 | `h`, `H` |
| `int`, `i32` | -2147483648 .. 2147483647 |  |
| `i64` | -9223372036854775808 .. 9223372036854775807 | `l`, `L` |
| `i128` | not implemented, yet | `ll`, `LL` |
| `ssize_t` | signed int large enough to hold pointer value | `z`, `Z` |
| `u8` | 0 .. 255 | `uhh`, `UHH` |
| `u16` | 0 .. 65535 | `uh`, `UH` |
| `unsigned`, `u32` | 0 .. 4294967295 | `u`, `U` |
| `u64` | 0 .. 18446744073709551615 | `ul`, `UL` |
| `u128` | not implemented, yet | `ull`, `ULL` |
| `size_t` | unsigned int large enough to hold pointer value | `uz`, `UZ` |

#### Floating Point Types

| Name (Alias) | Spec | Suffix |
| :--- | :--- | :--- |
| `real`, `f64` | IEEE-754 64 bit |  |
| `float`, `f32` | IEEE-754 32 bit | `f`, `F` |

#### Imaginary and Complex Types

| Name (Alias) | Spec | Suffix |
| :--- | :--- | :--- |
| `imaginary`, `j64` | 64 bit imaginary | `i`, `I` |
| `j32` | 32 bit imaginary | `fi`, `FI` |
| `complex`, `c64` | sum of `real` and `imaginary` | |
| `c32` | sum of `float` and `j32` | |

#### Logical Type

| Name | Values |
| :--- | :--- |
| `bool` | `false`, `true` |

#### Strings and Pointers

| Name | Syntax | Remarks |
| :--- | :--- | :--- |
| `string` | `"A String"` | Volvox string — automatically converted to `cstring` if necessary |
| `cstring` | | C stype string, i.e. address of 1st character  — for C interoperability — automatically converted to `string` if necessary |
| `voidptr` | `&a` | address of object — for C interoperability |

## Type Propagation

The basic targets of type propagation in Volvox are:
* do not lose precision but allow reinterpretation
	- results for small absolute values should be as expected by user
	- mixed signed / unsigned expressions are propagated to signed
* do early type propagation so intermediate results are not truncated
* automatically convert to larger significant bit sizes but not to smaller. Conversions should be reversible.
* allow explicit conversions to lower bit sizes ignoring overflow

#### Examples

* Early type propagation
```volvox
echo (2000000000 * 300000000)
```

`826015744`

The result remains of type `int` and an overflow occurs.

```volvox
echo (34L + 2000000000 * 300000000)
```

`600000000000000034`

The compiler recognizes (because of the 64 bit integer `34L`) that in the end a 64 bit result is required and the factors of the multiplication are propagated to 64 bits before the multiplication is performed.

C does the same for the case 16 bit -> 32 bit but not for 32 bit -> 64 bit.

## Libraries / Modules
### File Layout

Libraries (in the following also called modules) consist of one or more Volvox files in a directory underneath a library search path. Let's assume `/my/path/lib` is in this path then a Library named `mylib` and a sub-library `mylib.sublib` could consist of the following files:

```
/my/path/lib/mylib/10_basic.vx
/my/path/lib/mylib/20_impl.vx
/my/path/lib/mylib/30_compl.vx
/my/path/lib/mylib/sublib/a.vx
/my/path/lib/mylib/sublib/b.vx
```

The directory `mylib` is the only place where the library name is defined. This makes it easy to rename a library without touching the files — in particular if relative import paths are used between `mylib` and `mylib.sublib` (see [below](#relative-import-paths))

### Import

Identifiers from libraries can be imported in three ways:

1. Import of specific Identifiers:    
   `from mylib import id1, id2`    
   `id1` and `id2` can be used without qualifier
2. Import of complete library  
   `import mylib.sublib`  
   All identifiers of the library can be used but must be preceded by the qualifier `sublib.` (the last part of the full library name), i.e. `sublib.id3`, `sublib.id4`.
3. Import of complete library defining another qualifier    
   In case of name conflicts or just for more comfort it is possible to use a qualifier that is different from the last part of the full library name:  
   `import mylib as yourlib`  
   Now identifiers can be qualified like `yourlib.id1`, `yourlib.id2`.

#### Relative Import Paths

* Import from sub module  
  To import from `mylib.sublib` into `mylib` you can refer to `.sublib`:  
  `import .sublib`  
  This will provide `sublib.id3`, `sublib.id4`
* Import from higher level module  
  To import from `mylib` into `mylib.sublib`:  
  `from .. import id1, id2`  
  This will provide `id1` and `id2` without qualifier. For a qualified import the "`as`" syntax would by mandatory.
