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
double NumVal;             // Filled in if tok_number

Token Lexer::gettok() {
	static int CurChar = ' ';

	// Skip any whitespace.
	while (isspace(CurChar))
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
		if (IdentifierStr == "binary")
			return Token(tok_binary);
		if (IdentifierStr == "unary")
			return Token(tok_unary);
		if (IdentifierStr == "var")
			return Token(tok_var);
		return Token(tok_identifier);
	}

	if (isdigit(CurChar) || CurChar == '.') { // Number: [0-9.]+
		std::string NumStr;
		do {
			NumStr += CurChar;
			CurChar = advance();
		} while (isdigit(CurChar) || CurChar == '.');
		NumVal = strtod(NumStr.c_str(), nullptr);
		return Token(tok_number);
	}

	if (CurChar == '#') {
		// Comment until end of line.
		do
			CurChar = advance();
		while (CurChar != EOF && CurChar != '\n' && CurChar != '\r');

		if (CurChar != EOF)
			return gettok();
	}

	// Check for end of file.  Don't eat the EOF.
	if (CurChar == EOF)
		return Token(tok_eof);

	// Otherwise, just return the character as its ascii value.
	int ThisChar = CurChar;
	CurChar = advance();
	return Token(ThisChar);
}
