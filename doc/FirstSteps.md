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

Implicit printing of expressions results can be suppressed by starting `volvox` 
