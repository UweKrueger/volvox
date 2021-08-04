### Characters

```EBNF
Digit = "0" ... "9"
HexDigit = "0" ... "9" | "A" ... "F" | "a" ... "f"
UppercaseLetter = "A" ... "Z"
LowercaseLetter = "a" ... "z"
Space = ( " " | "\t" ) { " " | "\t" } /* no newline */
Sep = ( " " | "\t" | "\n" ) { " " | "\t" | "\n" }
Letter = "_" | Digit | UppercaseLetter | LowercaseLetter
Unicode_Letter = /* any Unicode code point */
```

### Idents

```EBNF
Ident = LowercaseLetter { Letter }
DecLiteral = Digit { Digit }
IntLiteral = [ "-" | "+" ] DecLiteral
HexLiteral = "0x" HexDigit { HexDigit }
UnsignedLiteral = DecLiteral "U" | HexLiteral
FloatNumber = [ "-" | "+" ] Digit { Digit } [ "." ] { Digit } [ "e" IntNumber ]

TypeIdent = UppercaseLetter Letter { Letter }
TypeSelection = Ident "." TypeTerm
```

### Type Specifier

```EBNF
Term = LowercaseLetter { Letter }
TypeTerm = UppercaseLetter Letter { Letter }
IdentSelection = Term "." Term | SelectorTerm
TypeSelection = Term "." TypeTerm

BuiltinType = "u8" | "u16" | "u32" | "u64"
            | "i8" | "i16" | "i32" | "i64"
            | "bool" | "int" | "uint" | "usize" | "ssize"
            | "voidptr" | "string" | "self"

Type = TypeTerm | TypeSelection | BuiltinType | MapType | ArrayType
     | FixedArrayType | ChannelType

MapType = "map" "[" BuiltinType "]" Type
ArrayType = "[" "]" Type
FixedArrayType = "[" Digit { Digit } "]" Type
ChannelType = "chan" "[" Type "]"
```

### Operators

```EBNF
UnaryOperator = "-" | "!" | "~" | "&"
BinaryOperator = "+" | "-" | "*" | "/"  | "%" | "**"
               | "&&" | "||" | "&" | "|" | "^"
               | "<<" | ">>" | "#" | "~"
               | ">=" | ">" | "==" | "<=" | "<" | "<=>"
DeclAssignOperator = ":="
AssignmentOperator = "=" | "+=" | "-=" |"*=" |"/=" |"%=" |"**=" | "&&=" | ""||="
                   | "&=" | "|=" | "^=" |"<<=" |">>=" |"#=" |"~="
```
