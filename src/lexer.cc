#include "../include/volvox.hh"
#include "global.h"

//===----------------------------------------------------------------------===//
// Lexer
//===----------------------------------------------------------------------===//

std::string getTokName(int Tok) {
	switch (Tok) {
	case tok_eof:
		return "eof";
	case tok_fn:
		return "fn";
	case tok_extern:
		return "extern";
	case tok_identifier:
		return "identifier";
	case tok_number:
		return "number";
	case tok_if:
		return "if";
	case tok_then:
		return "then";
	case tok_else:
		return "else";
	case tok_for:
		return "for";
	case tok_in:
		return "in";
	case tok_binary:
		return "binary";
	case tok_unary:
		return "unary";
	case tok_var:
		return "var";
	case tok_u8:
		return "u8";
	case tok_u16:
		return "u16";
	case tok_u32:
		return "u32";
	case tok_u64:
		return "u64";
	case tok_i8:
		return "i8";
	case tok_i16:
		return "i16";
	case tok_i32:
		return "i32";
	case tok_i64:
		return "i64";
	case tok_bool:
		return "bool";
	case tok_int:
		return "int";
	case tok_uint:
		return "uint";
	case tok_usize:
		return "usize";
	case tok_ssize:
		return "ssize";
	case tok_voidptr:
		return "voidptr";
	case tok_string:
		return "string";
	case tok_self:
		return "self";
	case tok_lparen:
		return "(";
	case tok_rparen:
		return ")";
	case tok_lbrack:
		return "[";
	case tok_rbrack:
		return "]";
	case tok_lbrace:
		return "{";
	case tok_rbrace:
		return "}";
	case tok_colon:
		return ":";
	case tok_semicolon:
		return ";";
	case tok_comma:
		return ",";
	case tok_dot:
		return ".";
	case tok_space:
		return " ";
	case tok_newline:
		return "\n";
	}
	return std::string(1, (char)Tok);
}
SourceLocation CurLoc = {0, 0};
SourceLocation LexLoc = {1, 0};

int advance() {
	int LastChar = getchar();

	if (LastChar == '\n' || LastChar == '\r') {
		LexLoc.Line++;
		LexLoc.Col = 0;
	} else {
		LexLoc.Col++;
	}
	return LastChar;
}

std::string IdentifierStr; // Filled in if tok_identifier
double NumVal;             // Filled in if tok_number

/// gettok - Return the next token from standard input.
int gettok() {
	static int LastChar = ' ';

	// Skip any whitespace.
	while (isspace(LastChar))
		LastChar = advance();

	CurLoc = LexLoc;

	if (isalpha(LastChar)) { // identifier: [a-zA-Z][a-zA-Z0-9]*
		IdentifierStr = LastChar;
		while (isalnum((LastChar = advance())))
			IdentifierStr += LastChar;

		if (IdentifierStr == "fn")
			return tok_fn;
		if (IdentifierStr == "extern")
			return tok_extern;
		if (IdentifierStr == "if")
			return tok_if;
		if (IdentifierStr == "then")
			return tok_then;
		if (IdentifierStr == "else")
			return tok_else;
		if (IdentifierStr == "for")
			return tok_for;
		if (IdentifierStr == "in")
			return tok_in;
		if (IdentifierStr == "binary")
			return tok_binary;
		if (IdentifierStr == "unary")
			return tok_unary;
		if (IdentifierStr == "var")
			return tok_var;
		return tok_identifier;
	}

	if (isdigit(LastChar) || LastChar == '.') { // Number: [0-9.]+
		std::string NumStr;
		do {
			NumStr += LastChar;
			LastChar = advance();
		} while (isdigit(LastChar) || LastChar == '.');

		NumVal = strtod(NumStr.c_str(), nullptr);
		return tok_number;
	}

	if (LastChar == '#') {
		// Comment until end of line.
		do
			LastChar = advance();
		while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

		if (LastChar != EOF)
			return gettok();
	}

	// Check for end of file.  Don't eat the EOF.
	if (LastChar == EOF)
		return tok_eof;

	// Otherwise, just return the character as its ascii value.
	int ThisChar = LastChar;
	LastChar = advance();
	return ThisChar;
}
