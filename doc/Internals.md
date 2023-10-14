# Internals and C compatibility
## Shared Heap Array
- p: address in pointer referring the object
- b: pointer size in bytes (8 on 64-Bit- and 4 on 32-Bit-Systems)
- n: Array size in bytes
- s: Array Length including terminating '\\0', i.e. n+1
- \|x\|: x aligned to pointer size by setting last bits to 0, i.e x-(x%b) or x&~(b-1)

Address | Size | Function
:---: | :---: | ---
<span style="color: #227744;">\|p-s\|-2b</span> | <span style="color: #227744;">b</span> | <span style="color: #227744;">Atomic Reference Counter</span>
<span style="color: #227744;">\|p-s\|-b</span> | <span style="color: #227744;">b</span> | <span style="color: #227744;">Mutex</span>
\|p-s\| | s | Array Data
\|p-s\|+s | s%b | Padding
p | b | n Array Size, i.e. Number of Elements
<span style="color: #227744;">p+b</span> | <span style="color: #227744;">b</span> | <span style="color: #227744;">Address of Reference Counter,  Address to free()</span>

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

Address | Size | Function
:---: | :---: | ---
<span style="color: #227744;">\|p-s\|-b</span> | <span style="color: #227744;">b</span> | <span style="color: #227744;">Reference Counter</span>
\|p-s\| | n | String Characters
\|p-s\|+n | 1 | '\\0' String Terminator
\|p-s\|+s | s%b | Padding
p | b | n+1, i.e. Size of Array
<span style="color: #227744;">p+b</span> | <span style="color: #227744;">b</span> | <span style="color: #227744;">Address of Reference Counter,  Address to free()</span>

The Address of the reference counter may be lower if the string is part of a larger structure. In any case it is the base address of the heap memory block.

## Resizable Shared Heap Arrays


There is text $\textcolor{red}{\mathrm{text}}$ $\textcolor{green}{\mathsf{Even \small More Text}}$ $x^2$ $\sqrt{x^5}$
