#include "../include/volvox.hh"
#include "global.h"

//===----------------------------------------------------------------------===//
// Lexer
//===----------------------------------------------------------------------===//

SourceLocation CurLoc = {0, 0};
SourceLocation LexLoc = {0, 0};

int Lexer::advance() {
	if (LexLoc.Col >= linelen) {
		linelen = getline(&linebuf, &bufsize, input);
		if (linelen <= 0) {
			return EOF;
		}
		LexLoc.Line++;
		LexLoc.Col = 0;
	}
	return linebuf[LexLoc.Col++];
}

std::string IdentifierStr; // Filled in if tok_identifier

Token Lexer::gettok(bool expectBinary) {
	static int CurChar = ' ';

	// Skip any whitespace but recorgnize newline if it could be as separator
	while (expectBinary ? isblank(CurChar) : isspace(CurChar))
		CurChar = advance();
	CurLoc = LexLoc;

	if (isalpha(CurChar)) { // identifier: [a-zA-Z][a-zA-Z0-9]*
		IdentifierStr = CurChar;
		while (isalnum((CurChar = advance())))
			IdentifierStr += CurChar;
		if (IdentifierStr == "fn")
			return Token(tok_fn);
		if (IdentifierStr == "extern")
			return Token(tok_extern);
		if (IdentifierStr == "if")
			return Token(tok_if);
		if (IdentifierStr == "then")
			return Token(tok_then);
		if (IdentifierStr == "else")
			return Token(tok_else);
		if (IdentifierStr == "for")
			return Token(tok_for);
		if (IdentifierStr == "in")
			return Token(tok_in);
		if (IdentifierStr == "var")
			return Token(tok_var);
		return Token(tok_identifier);
	}
	// Binary Operators
	if (expectBinary) {
		switch(CurChar) {
		case ':':
			CurChar = advance();
			if (CurChar == '=') {
				IdentifierStr = ":=";
				CurChar = advance();
				return tok_assign;
			} else {
				IdentifierStr = ":";
				return tok_colon;
			}
		case ',':
			IdentifierStr = CurChar;
			CurChar = advance();
			return tok_comma;
		case '\n':
		case '\r':
		case ';':
			IdentifierStr = CurChar;
			return ';';
		case '=':
			CurChar = advance();
			if (CurChar == '=') {
				IdentifierStr = "==";
				CurChar = advance();
				return tok_cmp;
			} else {
				IdentifierStr = "=";
				return tok_assign;
			}
		case '>':
		case '<':
		{
			auto c0 = CurChar;
			IdentifierStr = c0;
			CurChar = advance();
			if (CurChar == c0) { // <<, >>
				IdentifierStr += CurChar;
				CurChar = advance();
				if (CurChar == '=') { // <<=, >>=
					IdentifierStr += CurChar;
					CurChar = advance();
					return tok_assign;
				} else {
					return tok_mult;
				}
			} else if (CurChar == '=') { // <=, >=
				IdentifierStr += CurChar;
				CurChar = advance();
				if (c0 == '<' && CurChar == '>') { // <=>
					IdentifierStr += CurChar;
					CurChar = advance();
				}
				return tok_cmp;
			} else if (CurChar == '-' && c0 == '<') { // <-
				IdentifierStr += CurChar;
				CurChar = advance();
				return tok_arrow;
			} else {
				return tok_cmp;
			}
		}
		case '+':
		case '-':
		case '|':
		case '^':
		case '!':
		case '~':
		{
			auto c0 = CurChar;
			IdentifierStr = c0;
			CurChar = advance();
			if (CurChar == c0) {
				IdentifierStr += CurChar;
				CurChar = advance();
				if (c0 == '|')  { // ||
					return tok_or;
				} else { // postfix ++, --, !!, ~~, ^^
					return tok_postfix;
				}
			} else {
				if (CurChar == '=') {
					IdentifierStr += CurChar;
					CurChar = advance();
					if (c0 == '!') { // !=
						return tok_cmp;
					} else { // +=, -=, |=, ^=, ~=
						return tok_assign;
					}
				} else { // +, -, |, ^, !, ~
					return tok_add;
				}
			}
		}
		case '*':
		case '/':
		case '%':
		case '&':
		{ // <<, >> already handled in <, > case	
			auto c0 = CurChar;
			IdentifierStr = c0;
			CurChar = advance();
			if (CurChar == c0) {
				int tok;
				if (c0 == '&') { // &&
					tok = tok_and;
				} else if (c0 == '*') { // **
					tok = tok_pow;
				} else { // error - catch in parser
					return tok_mult;
				}
				IdentifierStr += CurChar;
				CurChar = advance();
				return tok;
			} else {
				return tok_mult;
			}
		}
		}
	}
	// Number Literal
	if (isdigit(CurChar) || // [0-9]*
		CurChar == '.' && isdigit(linebuf[LexLoc.Col]) || // .[0-9]*
		(CurChar == '+' || CurChar == '-') &&
		(isdigit(linebuf[LexLoc.Col]) || // [+-][0-9]*
		 linebuf[LexLoc.Col] == '.' && isdigit(linebuf[LexLoc.Col+1]))) { // [+-].[0-9]*
		char* n_ptr = linebuf + CurLoc.Col - 1;
		Token tok(&n_ptr);
		LexLoc.Col = (n_ptr - linebuf);
		CurChar = advance();
		return tok;
	}

	switch (CurChar) {
	case '"': {
		std::string StrLit = "";
		for (;;) {
			CurChar = advance();
			switch (CurChar) {
			case '\\':
				CurChar = advance();
				goto add_letter;
			case '"':
				CurChar = advance();
				return Token(StrLit);
			case EOF:
				fprintf(stderr, "unexpected EOF in string literal\n");
				return EOF;
			default:
			add_letter:
				StrLit += CurChar;
			}
		}
	}
	case '#': {
		// Comment until end of line.
		do
			CurChar = advance();
		while (CurChar != EOF && CurChar != '\n' && CurChar != '\r');

		if (CurChar != EOF)
			return gettok();
		// passthough
	}
	// Check for end of file.  Don't eat the EOF.
	case EOF:
		return tok_eof;
	// unary operators
	case '+':
	case '-':
	case '!':
	case '~':
	case '&':
		IdentifierStr = CurChar;
		advance();
		return tok_unary;
	case '<':
		if (linebuf[LexLoc.Col] == '-') {
			IdentifierStr = "<-";
			advance();
			return tok_unary;
		}
		// else passthrough
	default:
		// Otherwise, just return the character as its ascii value.
		int ThisChar = CurChar;
		CurChar = advance();
		return Token(ThisChar);
	}
}
