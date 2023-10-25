/*
 * Copyright © Uwe Krüger 2021, 2022, 2023
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"

/* we want to use NetBSD's libedit and not GNU readline because the latter is GPL licensed
   (not LGPL!). On some platforms there are <readline/readline.h> for GNU readline and
   <editline/readline.h> for libedit - but not on all. So we include <readline.h> here (if
   available) and make sure the correct version is used by setting CPPEXTRAFLAGS in Makefile
 */
#if defined(USE_EDITLINE)
#include <editline.h>
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
std::vector<const char*> SourceFileNames; // for SourceLocations to remain valid after files have been processed

#ifdef MONOCHROME_PROMPT
// OpenBSD's version of editline does not support colors
#define VOLVOX_PROMPT "%04d> "
#elif defined(_WIN32)
// our patched version of wineditline recognizes ANSI escape sequences
#define VOLVOX_PROMPT "\033[38;5;%" PRIu8 "m\033[48;5;%" PRIu8 "m% 4d\033[38;5;%" PRIu8 "m>\033[0m "
#else
// mainstream BSD libedit uses '\001' to toggle character counting
#define VOLVOX_PROMPT "\001\033[38;5;%" PRIu8 "m\033[48;5;%" PRIu8 "m\001% 4d\001\033[38;5;%" PRIu8 "m\001>\001\033[0m\001 "
#endif

static ssize_t fdgetline(char **lineptr, size_t *n) {
	static char* kept_buf = nullptr;
	static ssize_t kept_bufsize = 0;
	size_t offset = 0;
    if (!(*lineptr)) {
	    *n = 100;
	    *lineptr = (char*)malloc(*n);
    }
    for (;;) {
	    int c;
	    do {
		    if (lex.use_readline) {
			    if (kept_buf) {
				    free(*lineptr);
			    } else {
				    kept_buf = *lineptr;
				    kept_bufsize = *n;
			    }
			    int max_fail = 2;
			    for(;;) {
#ifndef _WIN32
				    errno = 0;
#endif
				    *lineptr = readline(prompt);
				    if (*lineptr)
					    break;
#if !defined (_MSC_VER)
				    outs() << "\n";
#endif
				    if (
#ifndef _WIN32
					    errno == EINTR &&
#endif
					    max_fail > 0)
					    errs() << "Press Crtl-C " << max_fail-- << "x again to exit...\n";
				    else
					    return -1;
			    }
			    *n = strlen(*lineptr);
			    if (*n)
				    add_history(*lineptr);
			    (*n)++;
			    return *n - 1;
		    }
		    if (kept_buf) {
			    free(*lineptr);
			    *lineptr = kept_buf;
			    *n = kept_bufsize;
			    kept_buf = nullptr;
			    kept_bufsize = 0;
		    }
		    c = 0;
		    int m = read(lex.input_fd, &c, 1);
		    if (m != 1 || c == '\004' || c == '\032') { // different incarnations of "End of File"
			    static bool tests_prepared = false;
			    if ((do_test || do_repl_test) && !tests_prepared) {
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
			    if (comp_mode == comp_jit && lex.input_fd == 0) {
#ifdef MONOCHROME_PROMPT
				    sprintf(prompt, VOLVOX_PROMPT, lex.Loc.Line + 1);
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
	    if (offset + 1 >= *n) {
		    do
			    *n += 50 + *n / 2;
		    while (offset + 1 >= *n);
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
std::vector<std::string> modulestack;

/* pause the current lexer context and create a new one based on the given
   import path */
bool Lexer::push_state(std::vector<std::string> _import_path, std::string _as, std::map<std::string, SourceLocation> _fromlist) {
	if (MainFunction) {
		if (!MainFunction->process_body(GlobalExprList)) {
			errs() << CurLoc << ": unable to process accumulated expressions for main function\n";
			exit(1);
		}
		GlobalExprList.clear();
	}
	std::string patterntail;
	std::string new_mod;
	for (int j=0; j < _import_path.size(); j++) {
		patterntail += _import_path[j];
		new_mod += _import_path[j];
		patterntail += PATHDIRSEP;
		if (j+1 < _import_path.size())
			new_mod += '.';
	}
	for (auto& mod: modulestack) {
		if (new_mod == mod) {
			errs() << CurLoc << ": cyclic import:";
			for (auto& m: modulestack)
				errs() << m << " - ";
			errs() << new_mod << "\n";
			return false;
		}
	}
	patterntail += "*.vx";
	as = std::move(_as);
	fromlist = std::move(_fromlist);
	auto new_module = Modules.try_emplace(patterntail, std::move(_import_path));
	if (new_module.second) {
		modulestack.push_back(std::move(new_mod));
		int old_input_fd = input_fd;
		auto oldbs = bufsize;
		source_stack.emplace_back(this);
		module = &new_module.first->second;
		linelen = 0;
		bufsize = 100;
		linebuf = (char*)malloc(bufsize);
		if (old_input_fd != builtin_input_fd)
			use_readline = false;
		if (module->import_path.size()) {
			source_files.push_back({});
			source_index.push_back(0);
			volvox_glob_t module_source_files = volvox_glob2(volvox_lib(), patterntail.c_str());
			for (int n = 0; n < module_source_files.size; n++)
				source_files.back().push_back(SourceFileNames.emplace_back(strdup(module_source_files.dirs[n])));
			volvox_free_glob(&module_source_files);
			if (source_files.back().empty()) {
				errs() << CurLoc << ": module '";
				bool pdot = false;
				for (auto& dir: new_module.first->second.import_path) {
					if (pdot)
						errs() << '.';
					else
						pdot = true;
					errs() << dir;
				}
				errs() << "' does not refer to any valid source (*.vx) files\n";
			}
		}
		return next_input_file();
	} else {
		if (verbosity >= 2)
			errs() << "skipping import of " << patterntail << " - already processed\n";
		auto previously_processed_module = Modules.find(patterntail);
		if (previously_processed_module == Modules.end()) {
			errs() << "internal error\n";
			abort();
		}
		Module* import_module = &previously_processed_module->second;
		import_from_module(import_module);
		return true;
	}
}

void Lexer::import_from_module(Module* import_module) {
	bool is_from_import = !fromlist.empty();
	std::set<std::string> processed_symbols_from_from_list = {};
	if (!is_from_import) {
		module->ImportedSymbols[{ as, "" }] = SymbolRef(); // declare `as` as module prefix
	}
	for (auto& unmangled_protos: import_module->FunctionProtos) {
		for (auto proto = unmangled_protos.second.begin(); proto != unmangled_protos.second.end();)
			if (!((*proto)->visibility & A_pub))
				unmangled_protos.second.erase(proto);
			else
				proto++;
		if (!unmangled_protos.second.empty()) {
			bool success = true;
			if (is_from_import) {
				if (fromlist.contains(unmangled_protos.first)) {
					auto _success = module->ImportedSymbols.try_emplace({ "", unmangled_protos.first }, &unmangled_protos.second);
					success = _success.second;
					if (success)
						processed_symbols_from_from_list.insert(unmangled_protos.first);
				}
			} else {
				auto _success = module->ImportedSymbols.try_emplace({ as, unmangled_protos.first }, &unmangled_protos.second);
				success = _success.second;
			}
			if (!success) {
				errs() << CurLoc << "cannot import '" << ((is_from_import || as == "") ? "" : (as + "."))
				       << unmangled_protos.first << "()' - symbol aleady in use\n";
			}
		}
	}
	for (auto global = import_module->globals_table.first(); global; ++global) {
		auto var = (FullVar*)((char*)global.getValue() + global.getValue()->offset);
		if (var->ft.type_attr & A_pub) {
			bool success = true;
			if (is_from_import) {
				if (fromlist.contains(global.getKey())) {
					auto _success = module->ImportedSymbols.try_emplace({ "", global.getKey() }, var);
					success = _success.second;
					if (success)
						processed_symbols_from_from_list.insert(global.getKey());
				}
			} else {
				auto _success = module->ImportedSymbols.try_emplace({ as, global.getKey() }, var);
				success = _success.second;
			}
			if (!success) {
				errs() << CurLoc << "cannot import '" << ((is_from_import || as == "") ? "" : (as + "."))
				       << global.getKey() << "' - symbol aleady in use\n";
			}
		}
	}
	for (auto type = import_module->type_table.first(); type; ++type) {
		auto ft = (volvoxc::FullType*)((char*)type.getValue() + type.getValue()->offset);
		// types are always public - so no need to checking for A_pub
		bool success = true;
		if (is_from_import) {
			if (fromlist.contains(type.getKey())) {
				auto _success = module->ImportedSymbols.try_emplace({ "", type.getKey() }, ft);
				success = _success.second;
				if (success)
					processed_symbols_from_from_list.insert(type.getKey());
			}
		} else {
			auto _success = module->ImportedSymbols.try_emplace({ as, type.getKey() }, ft);
			success = _success.second;
		}
		if (!success) {
			errs() << CurLoc << "cannot import '" << ((is_from_import || as == "") ? "" : (as + "."))
			       << type.getKey() << "' - symbol aleady in use\n";
		}
	}
	// if 'from' list was provided check that every element has been used somehow
	if (fromlist.size() > processed_symbols_from_from_list.size()) {
		for (auto& symbol: fromlist) {
			if (!processed_symbols_from_from_list.contains(symbol.first)) {
				errs() << symbol.second << ": '" << symbol.first << "' could not be imported - ";
				// to get a better error message we try to figure out if the sysmol was at least
				// declared as non-pub
				bool non_pub = false;
				auto fn_proto = import_module->FunctionProtos.find(symbol.first);
				if (fn_proto != import_module->FunctionProtos.end() && fn_proto->second.size())
					non_pub = true;
				else if (import_module->globals_table[symbol.first.c_str()])
					non_pub = true;
				if (non_pub)
					errs() << "symbol is not declared as 'pub'";
				else
					errs() << "symbol does not exist";
				errs() << " in imported module '";
				bool print_dot = false;
				for (auto& dir: import_module->import_path) {
					if (print_dot)
						errs() << '.';
					else
						print_dot = true;
					errs() << dir;
				}
				errs() << "'\n";
			}
		}
	}
}

void Lexer::pop_state() {
	if (source_stack.empty() || source_files.empty() || modulestack.empty()) {
		errs() << "internal error: source stack is empty\n";
		abort();
	}
	if (MainFunction) {
		if (!MainFunction->process_body(GlobalExprList)) {
			errs() << CurLoc << ": unable to process accumulated expressions for main function\n";
			exit(1);
		}
		GlobalExprList.clear();
	}
	Module* processed_module = module;
	free(linebuf);
	Loc = source_stack.back().Loc;
	module = std::move(source_stack.back().module);
	linelen = source_stack.back().linelen;
	bufsize = source_stack.back().bufsize;
	linebuf = source_stack.back().linebuf;
	source_stack.back().linebuf = nullptr;
	input_fd = source_stack.back().input_fd;
	use_readline = source_stack.back().use_readline;
	as = std::move(source_stack.back().as);
	fromlist = std::move(source_stack.back().fromlist);
	source_stack.pop_back();
	source_files.pop_back();
	source_index.pop_back();
	modulestack.pop_back();
	import_from_module(processed_module);
}

bool Lexer::next_input_file() {
	last_defined_type = nullptr;
	if (input_fd > 0) {
		if (input_fd == builtin_input_fd) {
			builtin_input_fd = -1;
			auto keep_linebuf = linebuf;
			linebuf = nullptr;
			auto res = push_state({}, "", {});
			free(linebuf);
			linebuf = keep_linebuf;
			return res;
		} else {
			close(input_fd);
		}
	}
	if (source_index.back() < source_files.back().size()) {
		Loc.File = source_files.back()[source_index.back()++];
		input_fd = open(Loc.File, O_CLOEXEC);
		if (input_fd < 0) {
			errs() << llvm::format("Cannot open input file \"%s\": %s\n", Loc.File, strerror(errno));
			errs() << SourceFileNames[0] << ' ' << SourceFileNames[2] << source_files.size() << ' ' << source_files.back()[2] << ' ' << source_files.back().size() << '\n';
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
		end_plus = false;
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
// is_last_char (if != NULL) returns if the char is the last of the token
//
char Lexer::peek() {
	if (CurChar & 0xff00)
		return '\0';
	char c = CurChar & 0xff;
	if (!isblank(c))
		return c;
	int max_i = linelen - Loc.Col + (use_readline ? 1 : 0);
	for (int i = 0; i < max_i; i++) {
		c = linebuf[Loc.Col + i];
		if (!isblank(c))
			break;
	}
	return c;
}

// get next character in current line - not treating blanks special
// i.e. the character might not belong to a token
// used to distinguish "[3]type" from "[3] vec2"
char Lexer::peek_strict() {
	if (CurChar & 0xff00)
		return '\0';
	return CurChar & 0xff;
}

std::pair<char,bool> Lexer::peek2_strict() {
	if ((CurChar & 0xff00) || !CurChar || (linebuf[Loc.Col] & 0xff00) || !linebuf[Loc.Col])
		return { '\0', false };
	char c = linebuf[Loc.Col];
	bool is_last_char = c && !isalpha(linebuf[Loc.Col+1]) && linebuf[Loc.Col+1] != '_';
	return { c, is_last_char };
	
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

void Lexer::check_end_plus() {
	// special handling of "end +   +   +"
	int j = Loc.Col;
	end_plus = false;
	while (!(j > linelen || !use_readline && j >= linelen)) {
		if (linebuf[j] == '+') {
			end_plus = true;
			break;
		} else if (linebuf[j] != ' ' && linebuf[j] != '\t') {
			break;
		}
		j++;
	}
}

Token Lexer::gettok(eXpect expect, int terminator) {
	Expected = expect; // for error messages in parser, etc.
	if (KeepIdentifierStr != "") {
		IdentifierStr = KeepIdentifierStr;
		KeepIdentifierStr = "";
		return Token(tok_identifier);
	}
	bool have_preceding_space = false;
startanalysis:
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
		if (auto tok_val = map_string_get(keyword_toks, IdentifierStr.c_str())) {
			if (tok_val->i32 == tok_end)
				check_end_plus();
			return Token(tok_val->i32);
		}
		if (expect == eBinOp) {
			KeepIdentifierStr = IdentifierStr;
			IdentifierStr = "";
			return Token(tok_invisible);
		}
		return Token(tok_identifier);
	}
	// Binary Operators
	if (expect == eBinOp) {
	binopswitch:
		switch(CurChar) {
		case '\n':
			if (!terminator) {
				IdentifierStr = CurChar;
				return ';';
			} else {
				do {
					CurChar = advance();
				} while (isspace(CurChar));
				if (isalnum(CurChar) || CurChar == '_' || CurChar == '(' || CurChar == '{' || CurChar == '[') {
					IdentifierStr = ',';
					return tok_comma;
				} else if (CurChar == terminator) {
					IdentifierStr = CurChar;
					CurChar = advance();
					return terminator;
				} else {
					goto binopswitch;
				}
			}
		case '"':
			IdentifierStr = "";
			return Token(tok_invisible);
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
			} else if (CurChar == 'i') {
				auto [next_char, is_last] = peek2_strict();
				if (next_char == 'n' && is_last) {
					CurChar = advance();
					CurChar = advance();
					IdentifierStr = "!in";
					return tok_not_in;
				}
			}
			IdentifierStr = '!';
			// we were expecting a binary operator but got a unary one
			return tok_error;
		case '~':
			CurChar = advance();
if (CurChar == 'i') {
				auto [next_char, is_last] = peek2_strict();
				if (next_char == 'n' && is_last) {
					CurChar = advance();
					CurChar = advance();
					IdentifierStr = "~in";
					return tok_reverse_in;
				}
			}
			IdentifierStr = '~';
			// we were expecting a binary operator but got a unary one
			return tok_error;
		case '+':
			if (end_plus) {
				IdentifierStr = CurChar;
				check_end_plus();
				CurChar = advance();
				return tok_end;
			}
			// else fallthrough
		case '>':
		case '<':
		case '|':
		case '&':
		case '-':
		case '*':
		case '/':
		case '%':
		case '^':
		{
			auto c0 = CurChar;
			IdentifierStr = c0;
			CurChar = advance();
			bool doubled = false;
			switch (c0) {
			case '>':
			case '<':
			case '&':
			case '|':
			case '+':
			case '-':
				if (CurChar == c0 || c0 == '>' && CurChar == '<') {
					IdentifierStr += CurChar;
					doubled = true;
					CurChar = advance();
					if (c0 == '+' || c0 == '-')
						return tok_postfix; // x++, x--
				}
			default:
				;
			}
			if (CurChar == '=') { // <<=, >>=
				IdentifierStr += CurChar;
				CurChar = advance();
				if (!doubled && (c0 == '>' || c0 == '<'))
					return tok_cmp; // >=, <=
				return tok_assign; // +=, <<=, ...
			}
			if (doubled) {
				switch (c0) {
				case '>':
					if (IdentifierStr[1] == '<')
						return tok_xor;
				case '<':
					return tok_mult;
				case '+':
				case '-':
					return tok_postfix;
				case '&':
					return tok_and;
				case '|':
					return tok_or;
				default:
					;
				}
			}
			// single character binary operators
			switch (c0) {
			case '>':
			case '<':
				return tok_cmp;
			case '|':
				return tok_bitor;
			case '&':
				return tok_bitand;
			case '+':
			case '-':
				return tok_add;
			case '*':
			case '/':
			case '%':
				return tok_mult;
			case '^':
				return tok_pow;
			default:
				abort();
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
		IdentifierStr = CurChar;
		switch (expect) {
		case eComma:
			return ',';
		case eSemi:
		case ePath:
			return ';';
		default:
			errs() << "Internal lexer error\n";
		}
	case '*':
		switch (expect) {
		case ePath:
			IdentifierStr = CurChar;
			CurChar = advance();
			return tok_star;
		default:
			errs() << "Invalid '*' in context\n";
			CurChar = advance();
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
		       && CurChar != '\n');

		if (CurChar != EOF
#if defined (_MSC_VER)
		       && CurChar != 26
#endif
			) {
			goto startanalysis;
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
		IdentifierStr = CurChar;
		CurChar = advance();
		return tok_ref;
	case '?':
		IdentifierStr = CurChar;
		CurChar = advance();
		return tok_optional;
	case '+':
		if (end_plus) {
			IdentifierStr = CurChar;
			check_end_plus();
			CurChar = advance();
			return tok_end;
		}
		// else fallthrough
	case '-':
	case '!':
	case '~':
		IdentifierStr = CurChar;
		CurChar = advance();
		return tok_unary;
	case '.':
		IdentifierStr = CurChar;
		CurChar = advance();
		if (CurChar == '.' && expect != ePath) {
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

llvm::raw_ostream& operator<<(llvm::raw_ostream& out, eXpect expect) {
	switch (expect) {
	case eNone:
		return out << "valid token";
	case eBinOp:
		return out << "binary operator";
	case eComma:
		return out << "comma";
	case eSemi:
		return out << "semicolon";
	case ePath:
		return out << "path";
	case eType:
		return out << "type specifier";
	default: // make gcc happy - we'll never get here for valid values of eXpect
		return out << "### internal error ###";
	}
}
