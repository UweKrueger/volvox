#include "../include/volvox.hh"
#include "global.h"

/* we want to use NetBSD's libedit and not GNU readline because the latter is GPL licensed
   (not LGPL!). On some platforms there are <readline/readline.h> for GNU readline and
   <editline/readline.h> for libedit - but not on all. So we include <readline.h> here (if
   available) and make sure the correct version is used by setting CPPEXTRAFLAGS in Makefile
 */
#if defined(__OpenBSD__)
// OpenBSD has no 'readline.h' header for libedit - so just declare the functions
// we need, here...
extern "C" char* readline(const char* p);
extern "C" int add_history(const char *line);
extern "C" int rl_initialize(void);
#else // Linux, NetBSD, Dragonfly BSD, Windows
// let CPP's '-I...' point to libedit's version of readline.h (see Makefile)
// make sure linker flags '-L... -l...' match the the same version... 
#include <readline.h>
#endif
#ifdef NEED_HISTORY_H
// usually this should not be needed with libedit...
#include <history.h>
#endif

// Windows has no CLOEXEC
#if !defined(O_CLOEXEC)
#define O_CLOEXEC 0
#endif

#if defined(_MSC_VER)
#define volvox_glob2 _ZN6volvox4globEPKcS1_
#define volvox_free_glob _ZN6volvox9free_globEP13volvox_glob_t
extern "C" volvox_glob_t volvox_glob2(const char* patbase, const char* pattail);
extern "C" void volvox_free_glob(volvox_glob_t* rets);
#else
#define volvox_glob2 volvox::glob
#define volvox_free_glob volvox::free_glob
#endif

//===----------------------------------------------------------------------===//
// Lexer
//===----------------------------------------------------------------------===//

static char prompt[1024];

#ifdef MONOCHROME_PROMPT
// OpendBSD's version of editline does not support colors
#define VOLVOX_PROMPT "%04d> "
#elif defined(_WIN32)
// our patched version of wineditline recognizes ANSI escape sequences
#define VOLVOX_PROMPT "\033[38;5;%" PRIu8 "m\033[48;5;%" PRIu8 "m% 4d\033[38;5;%" PRIu8 "m>\033[0m "
#else
// mainstream BSD libedit uses '\001' to toggle character counting
#define VOLVOX_PROMPT "\001\033[38;5;%" PRIu8 "m\033[48;5;%" PRIu8 "m\001% 4d\001\033[38;5;%" PRIu8 "m\001>\001\033[0m\001 "
#endif

static ssize_t fdgetline(char **lineptr, size_t *n) {
    if (!(*lineptr)) {
	    *n = 100;
	    *lineptr = (char*)malloc(*n);
    }
    size_t offset = 0;
    for (;;) {
	    int c;
	    do {
		    if (lex.use_readline) {
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
		    int m = read(lex.input_fd, &c, 1);
		    if (m != 1 || c == '\004' || c == '\032') { // different incarnations of "End of File"
			    static bool tests_prepared = false;
			    if (do_test && !tests_prepared) {
				    // This was just the initialization file for builtins
				    // now switch to real input
				    // this had to wait until definitions in 'builtin.vx' have been processed
				    PrepareTestFramework();
				    tests_prepared = true;
			    }
			    if (!lex.next_input_file()) {
				    c = EOF;
				    if (lex.input_fd == 0)
					    outs() << "\n";
				    return -1;
			    }
			    *n = 0;
			    if (comp_mode == comp_jit && lex.input_fd == 0) {
#ifdef MONOCHROME_PROMPT
				    sprintf(prompt, VOLVOX_PROMPT, LexLoc.Line + 1);
#else
				    sprintf(prompt, VOLVOX_PROMPT, p_col.number, p_col.background, lex.Loc.Line + 1, p_col.greater);
#endif
				    for (int i=0; i<prompt_indent && i<200; i++)
					    strcat(prompt, "    ");
				    lex.use_readline = true;
#ifndef _WIN32
				    rl_initialize();
#endif
			    }
			    c = '\r'; // abuse Windows logic to repeat read
		    }
	    } while (c == '\r');
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
static int CurChar = ' ';
static std::string KeepIdentifierStr = "";

/* pause the current lexer context and create a new one based on the given
   import path */
bool Lexer::push_state(std::vector<std::string> _import_path) {
	std::string patterntail = "";
	for (int j=0; j < _import_path.size(); j++) {
		patterntail += _import_path[j];
		patterntail += PATHDIRSEP;
	}
	patterntail += "*.vx";
	source_stack.emplace_back(this);
	auto new_module = Modules.try_emplace(patterntail, std::move(_import_path));
	if (new_module.second) {
		module = &new_module.first->second;
		linelen = 0;
		linebuf = (char*)malloc(bufsize);
		if (input_fd != builtin_input_fd)
			use_readline = false;
		if (module->import_path.size()) {
			source_files.push_back({});
			volvox_glob_t module_source_files = volvox_glob2(volvox_lib(), patterntail.c_str());
			for (int n = 0; n < module_source_files.size; n++)
				source_files.back().emplace_back(module_source_files.dirs[n]);
			volvox_free_glob(&module_source_files);
		}
		return next_input_file();
	} else {
		if (verbosity >= 2)
			errs() << "skipping import of " << patterntail << " - already processed\n";
		return true;
	}
}

void Lexer::pop_state() {
	if (source_stack.empty() || source_files.empty()) {
		errs() << "internal error: source stack is empty\n";
		abort();
	}
	free(linebuf);
	Loc = source_stack.back().Loc;
	module = source_stack.back().module;
	linelen = source_stack.back().linelen;
	bufsize = source_stack.back().bufsize ;
	linebuf = source_stack.back().linebuf;
	input_fd = source_stack.back().input_fd;
	use_readline = source_stack.back().use_readline;
	source_stack.pop_back();
	source_files.pop_back();
}

bool Lexer::next_input_file() {
	if (input_fd > 0) {
		if (input_fd == builtin_input_fd) {
			builtin_input_fd = -1;
			return push_state({});
		} else {
			close(input_fd);
		}
	}
	if (source_index.back() < source_files.back().size()) {
		Loc.File = source_files.back()[source_index.back()++].c_str();
		input_fd = open(Loc.File, O_CLOEXEC);
		if (input_fd < 0) {
			errs() << llvm::format("Cannot open input file \"%s\": %s\n", Loc.File, strerror(errno));
			exit(1);
		}
	} else if (source_stack.size() > 1) {
		pop_state();
		return true;
	} else if ((jit_repl || !source_index.back()) && input_fd != 0) {
		input_fd = 0;
		Loc.File = "<stdin>";
	} else {
		return false;
	}
	Loc.Line = Loc.Col = 0;
	return true;
}

int Lexer::advance() {
	// unfortunately readline does not return the trailing \n whereas
	// getline (and fdgetline from above) do. We catch this here by
	// handling line endings different when use_readline is set
	if (Loc.Col > linelen || !use_readline && Loc.Col >= linelen) {
		if (use_readline) {
#ifdef MONOCHROME_PROMPT
			sprintf(prompt, VOLVOX_PROMPT, Loc.Line + 1);
#else
			sprintf(prompt, VOLVOX_PROMPT, p_col.number, p_col.background, Loc.Line + 1, p_col.greater);
#endif
			for (int i=0; i<prompt_indent && i<200; i++)
				strcat(prompt, "    ");
		}
		linelen = fdgetline(&linebuf, &bufsize);
		if (linelen < 0 || !use_readline && linelen <= 0) {
			return EOF;
		}
		Loc.Line++;
		Loc.Col = 0;
	}
	int c = linebuf[Loc.Col++];
	if (!c && use_readline)
		c = '\n';
	return c;
}

// get next character in current line that belongs to a token - blanks are ignored
char Lexer::peek() {
	if (CurChar & 0xff00)
		return '\0';
	char c = CurChar & 0xff;
	if (isblank(c)) {
		int max_i = linelen - Loc.Col + (use_readline ? 1 : 0);
		for (int i = 0; i < max_i; i++) {
			c = linebuf[Loc.Col + i];
			if (!isblank(c))
				break;
		}
	}
	return c;
}

// get next character in current line - not treating blanks special
// i.e. the character might not belong to a token
// used to distinguish "[3]type" from "[3] vec2"
char Lexer::peek_strict() {
	if (CurChar & 0xff00)
		return '\0';
	else
		return CurChar & 0xff;
}

// get the character strictly before the current token in the same line
char Lexer::look_back_strict() {
	if (!CurLoc.Col)
		return '\0';
	else
		return linebuf[CurLoc.Col - 1];
}

std::string IdentifierStr; // Filled in if tok_identifier

Token Lexer::purge_line() {
	Loc.Col = linelen;
	CurLoc = Loc;
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
	bool have_preceding_space;
	// Skip any whitespace but recorgnize newline if it could be a separator
	if (expect == eNone ? isspace(CurChar) : isblank(CurChar)) {
		have_preceding_space = true;
		do {
			CurChar = advance();
		} while (expect == eNone ? isspace(CurChar) : isblank(CurChar));
	} else {
		have_preceding_space = false;
	}
	CurLoc = Loc;

	if (isalpha(CurChar) || CurChar == '_') { // identifier: [a-zA-Z_][a-zA-Z0-9_]*
		IdentifierStr = CurChar;
		while (isalnum((CurChar = advance())) || CurChar == '_')
			IdentifierStr += CurChar;
		if (auto tok_val = map_string_get(keyword_toks, IdentifierStr.c_str()))
			return Token(tok_val->i32);
		if (expect == eBinOp) {
			KeepIdentifierStr = IdentifierStr;
			IdentifierStr = "";
			return Token(tok_invisible);
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
			} else if (c0 == '>' && CurChar == '<') {
				IdentifierStr += CurChar;
				CurChar = advance();
				return tok_or;
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
			if (!have_preceding_space || CurChar == '(') {
				// function call or array index
				IdentifierStr = CurChar;
				// We do not advance here but return the empty (binary) operator.
				// The token is handled the next time when no binary is expected.
				// "tok_selector" has a very high priority so sin(x)^2 = (sin(x))^2
				// whereas sin x^2 = sin(x^2)
				return Token(tok_selector);
			}
		default:
			;
		}
	}
	// Number Literal
	if (isdigit(CurChar) || // [0-9]*
	    CurChar == '.' && isdigit(linebuf[Loc.Col]) || // .[0-9]*
	    (CurChar == '+' || CurChar == '-') &&
	    (isdigit(linebuf[Loc.Col]) || // [+-][0-9]*
	     linebuf[Loc.Col] == '.' && isdigit(linebuf[Loc.Col+1]))) { // [+-].[0-9]*
		if (expect == eBinOp) {
			IdentifierStr = "";
			return Token(tok_invisible);
		}
		char* n_ptr = linebuf + CurLoc.Col - 1;
		Token tok(&n_ptr);
		Loc.Col = (n_ptr - linebuf);
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
		case ePath:
			IdentifierStr = CurChar;
			return ';';
		default:
			errs() << "Internal lexer error\n";
		}
	case '"': {
		std::string StrLit = "";
		unsigned sum = 0;
		unsigned nexpect = 0;
		for (;;) {
			CurChar = advance();
			if (nexpect) {
				if (CurChar >= '0' && CurChar < '8') {
					sum = (sum << 3) | (CurChar - '0');
					if (!--nexpect)
						CurChar = advance();
					else
						continue;
				} else {
					nexpect = 0;
				}
				if (sum >= 0x100)
					errs() << "Illegal octal character sequence\n";
				else
					StrLit += (char)sum;
			}
			switch (CurChar) {
			case '\\':
				CurChar = advance();
				switch (CurChar) {
				case 'a':
					StrLit += '\a';
					continue;
				case 'b':
					StrLit += '\b';
					continue;
				case 'e':
					StrLit += '\033';
					continue;
				case 'f':
					StrLit += '\f';
					continue;
				case 'n':
					StrLit += '\n';
					continue;
				case 'r':
					StrLit += '\r';
					continue;
				case 't':
					StrLit += '\t';
					continue;
				case 'v':
					StrLit += '\v';
					continue;
				case '\\':
					StrLit += '\\';
					continue;
				case '0':
				case '1':
				case '2':
				case '3':
				case '4':
				case '5':
				case '6':
				case '7':
					// 3 octal digits
					sum = CurChar - '0';
					nexpect = 2;
					continue;
				default:
					goto add_letter;
				}
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
	case '&':
		if (expect == eType) {
			IdentifierStr = CurChar;
			CurChar = advance();
			return '&';
		}
	case '+':
	case '-':
	case '!':
	case '~':
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
			char nextchar = this->peek_strict();
			if (isalpha(nextchar) || nextchar == '_')
				return tok_selector;
			else
				return tok_end;
		}
	case '<':
		if (linebuf[Loc.Col] == '-') {
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
