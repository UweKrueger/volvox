### Characters

```EBNF
Digit = "0" ... "9"
HexDigit = "0" ... "9" | "A" ... "F" | "a" ... "f"
UppercaseLetter = "A" ... "Z"
LowercaseLetter = "a" ... "z"
Space = ( " " | "\t" ) { " " | "\t" } /* no newline */
Sep = ( " " | "\t" | "\n" ) { " " | "\t" | "\n" }
_ = { " " | "\t" | "\n" } /* optional separator */
Letter = "_" | Digit | UppercaseLetter | LowercaseLetter
Char = /* any ASCII character
Rune = /* any Unicode code point, special characters escaped: \" \n \t \\ \0 \uABCD \xAB */
```

### Idents

```EBNF
Ident = LowercaseLetter { Letter }
TypeIdent = UppercaseLetter Letter { Letter }
TypeSelection = Ident "." TypeTerm
```

### Type Specifier

```EBNF
Term = LowercaseLetter { Letter }
IdentSelection = Term "." Term | SelectorTerm
TypeSelection = Term "." TypeTerm

BuiltinType = "u8" | "u16" | "u32" | "u64"
            | "i8" | "i16" | "i32" | "i64"
            | "bool" | "int" | "uint" | "usize" | "ssize"
            | "voidptr" | "string" | "self"

Type = TypeIdent | TypeSelection | BuiltinType | MapType | ArrayType
     | FixedArrayType | ChannelType

MapType = "map" "[" BuiltinType "]" Type
ArrayType = "[]" Type
FixedArrayType = "[" DecLiteral "]" Type
ChannelType = "chan" "[" Type "]"
```

### Literals and Initialization Expressions

```EBNF
DecLiteral = Digit { Digit }
IntLiteral = [ "-" | "+" ] DecLiteral
HexLiteral = "0x" HexDigit { HexDigit }
UnsignedLiteral = DecLiteral "U" | HexLiteral
FloatLiteral = [ "-" | "+" ] Digit { Digit } [ "." ] { Digit } [ "e" IntNumber ]
StringLiteral = "\"" { Rune } "\""
Literal = IntLiteral | UnsignedLiteral | FloatLiteral | StringLiteral | CharLiteral | RuneLiteral

SimpleExpr = Ident [ "()" ] { "." ident [ "()" ] }
InterExpr = "$" "{" Expression [ ":" [ DecLiteral [ "." DecLiteral ] ] LowercaseLetter "}"
          | "$" SimpleExpr

StringInterLiteral = "\"" { Rune } InterExpr { { Rune } InterExpr } { Rune } "\"" 

PropInit = ( "cap" | "len" | "init" ) ":" _ Expression

ExprList = Expression { _ [ "," ] _ Expression }
ArrayInit = ArrayType "{" _ PropInit _ { "," _ PropInit _ } "}"
          | "[" _ ExpressionList _ "]"
		  
MapInit = MapType "{}"
        | "map" "{" _ Expression ":" _ Expression _ { "," _ Expression ":" _ Expression _ } "}"

ChanInit = "chan" "[" Type "]" "{" _ PropInit _ "}" /* only "cap: nElements" allowed */

StructInit = Type "{" _ { Term ":" Expression _ } "}"

InitExpr = Literal | ArrayInit | MapInit | ChanInit | StructInit

```

### Operators

```EBNF
UnaryOperator = "-" | "!" | "~" | "&"
BinaryOperator = "+" | "-" | "*" | "/" | "%" | "**"
               | "&&" | "||" | "&" | "|" | "^"
               | "<<" | ">>"
               | ">=" | "==" | "<=" | ">" | "<" | "!=" | "<=>"
DeclAssignOperator = ":="
AssignmentOperator = "+=" | "-=" | "*=" | "/=" | "**=" | "&&=" | "||="
                   | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>=" | "="
```

### Expressions

```EBNF
Expression = Ident | CallExpr | InfixExpr | PrefixExpr | SelectorExpr | CastExpr
           | InitExpr | StringInterLiteral | IndexExpr
LeftExpr = Ident | IndexExpr | SelectorExpr /* addressable objects that can be left from "=" */
```
