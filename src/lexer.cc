#include "../include/volvox.hh"
#include "global.h"
#include <editline/readline.h>

//===----------------------------------------------------------------------===//
// Lexer
//===----------------------------------------------------------------------===//

static char prompt[1024];
bool use_readline = false;

static ssize_t fdgetline(char **lineptr, size_t *n) {
    if (!(*lineptr)) {
	    *n = 100;
	    *lineptr = (char*)malloc(*n);
    }
    size_t offset = 0;
    for (;;) {
	    int c;
	    do {
		    if (use_readline) {
			    free(*lineptr);
			    *lineptr = readline(prompt);
			    if (!*lineptr) {
#if !defined (_MSC_VER)
				    outs() << "\n";
#endif
				    return -1;
			    }
			    *n = strlen(*lineptr);
			    if (*n)
				    add_history(*lineptr);
			    return *n;
		    }
		    c = 0;
		    int m = read(cur_input_fd, &c, 1);
		    if (m != 1) {
			    if (cur_input_fd != input_fd) {
				    // This was just the initialization file for builtins
				    // now switch to real input
				    cur_input_fd = input_fd;
				    LexLoc = { input_file_name, 0, 0 };
				    if (comp_mode == comp_jit && cur_input_fd == 0) {
					    sprintf(prompt, "%03d> ", LexLoc.Line + 1);
					    for (int i=0; i<prompt_indent && i<200; i++)
						    strcat(prompt, "    ");
					    use_readline = true;
#if !defined (_MSC_VER)
					    rl_initialize();
#endif
					    *n = 0;
				    }
				    c = '\r'; // abuse Windows logic to repeat read
			    } else {
				    c = EOF;
			    }
		    }
	    } while (c == '\r');
	    if (c == EOF) {
		    if (cur_input_fd == 0)
			    outs() << "\n";
		    return -1;
	    }
	    if (offset >= (*n - 1)) {
		    *n += *n / 2;
		    *lineptr = (char*)realloc(*lineptr, *n);
	    }
	    *(*lineptr + offset++) = c;
	    if (c == '\n') {
		    break;
	    }
    }
    *(*lineptr + offset) = '\0';
    return offset;
}

SourceLocation CurLoc;
SourceLocation LexLoc;
static int CurChar = ' ';
static std::string KeepIdentifierStr = "";

int Lexer::advance() {
	// unfortunately readline does not return the trailing \n whereas
	// getline (and fdgetline from above) do. We catch this here by
	// handling line endings different when use_readline is set
	if (LexLoc.Col > linelen || !use_readline && LexLoc.Col >= linelen) {
		if (use_readline) {
			sprintf(prompt, "%03d> ", LexLoc.Line + 1);
			for (int i=0; i<prompt_indent && i<200; i++)
				strcat(prompt, "    ");
		}
		linelen = fdgetline(&linebuf, &bufsize);
		if (linelen < 0 || !use_readline && linelen <= 0) {
			return EOF;
		}
		LexLoc.Line++;
		LexLoc.Col = 0;
	}
	int c = linebuf[LexLoc.Col++];
	if (!c && use_readline)
		c = '\n';
	return c;
}

std::string IdentifierStr; // Filled in if tok_identifier

Token Lexer::purge_line() {
	LexLoc.Col = linelen;
	CurLoc = LexLoc;
	CurChar = '\n';
	IdentifierStr = ';';
	return ';';
}

Token Lexer::gettok(eXpect expect) {
	if (KeepIdentifierStr != "") {
		IdentifierStr = KeepIdentifierStr;
		KeepIdentifierStr = "";
		return Token(tok_identifier);
	}
	// Skip any whitespace but recorgnize newline if it could be a separator
	while (expect == eNone ? isspace(CurChar) : isblank(CurChar))
		CurChar = advance();
	CurLoc = LexLoc;

	if (isalpha(CurChar) || CurChar == '_') { // identifier: [a-zA-Z_][a-zA-Z0-9_]*
		IdentifierStr = CurChar;
		while (isalnum((CurChar = advance())) || CurChar == '_')
			IdentifierStr += CurChar;
		if (IdentifierStr == "fn")
			return Token(tok_fn);
		if (IdentifierStr == "extern")
			return Token(tok_extern);
		if (IdentifierStr == "type")
			return Token(tok_type);
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
		if (IdentifierStr == "end")
			return Token(tok_end);
		if (IdentifierStr == "return")
			return Token(tok_return);
		if (IdentifierStr == "true")
			return Token(true);
		if (IdentifierStr == "false")
			return Token(false);
		if (IdentifierStr == "packed")
			return Token(tok_packed);
		if (IdentifierStr == "nullptr")
			return Token((void*)0);
		if (expect == eBinOp) {
			KeepIdentifierStr = IdentifierStr;
			IdentifierStr = "";
			return Token(tok_);
		}
		return Token(tok_identifier);
	}
	// Binary Operators
	if (expect == eBinOp) {
		switch(CurChar) {
		case '\n':
			IdentifierStr = CurChar;
			return ';';
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
		case '!':
			CurChar = advance();
			if (CurChar == '=') {
				IdentifierStr = "!=";
				CurChar = advance();
				return tok_cmp;
			} else {
				IdentifierStr = "!";
				return tok_or;
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
		case '|':
		case '^':
		{
			IdentifierStr = CurChar;
			CurChar = advance();
			return tok_or;
		}
		case '+':
		case '-':
		case '~':
		{
			auto c0 = CurChar;
			IdentifierStr = c0;
			CurChar = advance();
			if (CurChar == c0) {
				IdentifierStr += CurChar;
				CurChar = advance();
				// postfix ++, --, ~~
				return tok_postfix;
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
		case '&':
		{
			IdentifierStr = CurChar;
			CurChar = advance();
			return tok_and;
		}
		case '*':
		case '/':
		case '%':
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
		case '(':
		case '[':
			// function call
			IdentifierStr = CurChar;
			// We do not advance here but return the empty (binary) operator
			// the token is handled the nex time when no binary is expected.
			// "tok_selector" has a very high priority so sin(x)^2 = (sin(x))^2
			// whereas sin x^2 = sin(x^2)
			return Token(tok_selector);
		default:
			;
		}
	}
	// Number Literal
	if (isdigit(CurChar) || // [0-9]*
	    CurChar == '.' && isdigit(linebuf[LexLoc.Col]) || // .[0-9]*
	    (CurChar == '+' || CurChar == '-') &&
	    (isdigit(linebuf[LexLoc.Col]) || // [+-][0-9]*
	     linebuf[LexLoc.Col] == '.' && isdigit(linebuf[LexLoc.Col+1]))) { // [+-].[0-9]*
		if (expect == eBinOp) {
			IdentifierStr = "";
			return Token(tok_);
		}
		char* n_ptr = linebuf + CurLoc.Col - 1;
		Token tok(&n_ptr);
		LexLoc.Col = (n_ptr - linebuf);
		CurChar = advance();
		return tok;
	}

	switch (CurChar) {
	case '\n':
		switch (expect) {
		case eComma:
			IdentifierStr = CurChar;
			return ',';
		case eColon:
			IdentifierStr = CurChar;
			return ';';
		default:
			errs() << "Internal lexer error\n";
		}
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
#if defined (_MSC_VER)
			case 26:
#endif
				errs() << "unexpected EOF in string literal\n";
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
		while (CurChar != EOF
#if defined (_MSC_VER)
		       && CurChar != 26
#endif
		       && CurChar != '\n' && CurChar != '\r');

		if (CurChar != EOF
#if defined (_MSC_VER)
		       && CurChar != 26
#endif
			) {
			IdentifierStr = CurChar;
			return ';';
		}
	// passthough
	}
		// Check for end of file.  Don't eat the EOF.
	case EOF:
#if defined (_MSC_VER)
	case 26:
#endif
		return tok_eof;
		// unary operators
	case '+':
	case '-':
	case '!':
	case '~':
	case '&':
		IdentifierStr = CurChar;
		CurChar = advance();
		return tok_unary;
	case '.':
		IdentifierStr = CurChar;
		CurChar = advance();
		if (CurChar == '.') {
			IdentifierStr += CurChar;
			CurChar = advance();
			if (CurChar == '.') {
				IdentifierStr += CurChar;
				CurChar = advance();
				return tok_ellipsis;
			} else {
				return tok_range;
			}
		} else {
			return tok_selector;
		}
	case '<':
		if (linebuf[LexLoc.Col] == '-') {
			IdentifierStr = "<-";
			CurChar = advance();
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
