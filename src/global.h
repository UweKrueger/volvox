#pragma once

class PrototypeAST;
class ExprAST;
class FunctionAST;
class NumberExprAST;
class VariableExprAST;
class CallExprAST;
class IfExprAST;
class ForExprAST;
class VarExprAST;
class UnaryExprAST;

// The lexer returns tokens [0-255] if it is an unknown character, otherwise one
// of these for known things.
enum TokenType {
	tok_eof = -1,

	// commands
	tok_fn = -2,
	tok_extern = -3,

	// primary
	tok_identifier = -4,
	tok_number = -5,
	tok_str_lit = -30,

	// control
	tok_if = -6,
	tok_then = -7,
	tok_else = -8,
	tok_for = -9,
	tok_in = -10,

	// operators
	tok_binary = -11,
	tok_unary = -12,

	// var definition
	tok_var = -13,

	// built-in types
	tok_u8 = -14,
	tok_u16 = -15,
	tok_u32 = -16,
	tok_u64 = -17,
	tok_i8 = -18,
	tok_i16 = -19,
	tok_i32 = -20,
	tok_i64 = -21,
	tok_bool = -22,
	tok_uint = -23,
	tok_int = -24,
	tok_usize = -25,
	tok_ssize = -26,
	tok_voidptr = -27,
	tok_string = -28,
	tok_self = -29,

	// braces
	tok_lparen = -64,
	tok_rparen = -65,
	tok_lbrack = -66,
	tok_rbrack = -67,
	tok_lbrace = -68,
	tok_rbrace = -69,

	tok_colon = -72,
	tok_semicolon = -73,
	tok_comma = -74,
	tok_dot = -75,

	tok_space = -80,
	tok_newline = -81
};

struct DebugInfo {
	llvm::DICompileUnit *TheCU;
	llvm::DIType *DblTy;
	std::vector<llvm::DIScope *> LexicalBlocks;

	void emitLocation(ExprAST *AST);
	llvm::DIType *getDoubleTy();
};

extern DebugInfo KSDbgInfo;

struct SourceLocation {
	int Line;
	int Col;
};

enum CompModes {
	comp_jit,
	comp_obj,
	comp_dbg
};

extern CompModes comp_mode;
extern SourceLocation CurLoc;
extern SourceLocation LexLoc;
extern std::string IdentifierStr; // Filled in if tok_identifier

// AST

llvm::raw_ostream &indent(llvm::raw_ostream &O, int size);
// Parser

extern std::map<char, int> BinopPrecedence;
extern std::unique_ptr<ExprAST> LogErrorGen(const char *Str, va_list ap);
extern std::unique_ptr<ExprAST> LogError(const char *Str, ...);
extern std::unique_ptr<FunctionAST> ParseDefinition();
extern std::unique_ptr<FunctionAST> ParseTopLevelExpr();
extern std::unique_ptr<PrototypeAST> ParseExtern();

class TypeTable {
public:
	bool add(char* name, llvm::Type* type) {
		auto it = table.insert({name, type});
		return it.second;
	}
	llvm::Type* find(char* name) {
		auto it = table.find(name);
		return it == table.end() ? nullptr : it->second;
	}
	~TypeTable() {
		for (auto it = table.begin(); it != table.end(); it = table.erase(it))
			free(it->second);
	}
protected:
	std::map<char*, llvm::Type*> table;
};

extern TypeTable type_table;

// Token

enum val_type_t {
	val_u8,
	val_u16,
	val_u32,
	val_u64,
	val_uint,
	val_i8,
	val_i16,
	val_i32,
	val_i64,
	val_int,
	val_f32,
	val_f64,
	val_string,
	val_string_part,
	val_invalid,
};

class Token {
public:
	int type;
	Token(int type = 0) : type(type) {}
	Token(char** s_ptr);
	Token(const std::string& str);
	std::string tokName() const;
	val_type_t val_type;
	union {
		uint64_t uint_val;
		int64_t int_val;
		double float_val;
		char* str_val;
	};
	std::string str() const { return this->tokName(); }
};
	
extern Token CurTok;
extern Token getNextToken();

class Lexer {
public:
	Lexer(FILE* input = stdin, size_t bufsize=100)
		: input(input), bufsize(bufsize), linebuf((char*)malloc(bufsize)), linelen(0) {}
	~Lexer() { free(linebuf); }
	int advance();
	Token gettok();
	FILE* input;
	ssize_t linelen;
	size_t bufsize;
	char* linebuf;
};

// Types
extern llvm::Type* _f64;
extern llvm::Type* _string;
