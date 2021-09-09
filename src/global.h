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
extern double NumVal;             // Filled in if tok_number

// AST

llvm::raw_ostream &indent(llvm::raw_ostream &O, int size);
// Parser

extern int CurTok;
extern int getNextToken();
extern std::map<char, int> BinopPrecedence;
extern std::unique_ptr<ExprAST> LogError(const char *Str);
extern std::unique_ptr<FunctionAST> ParseDefinition();
extern std::unique_ptr<FunctionAST> ParseTopLevelExpr();
extern std::unique_ptr<PrototypeAST> ParseExtern();

// Token

class Token {
public:
	TokenType type;
	Token(TokenType type) : type(type) {}
	virtual ~Token() = default;
	virtual std::string tokName() const;
	virtual std::string str() const { return this->tokName(); }
};

enum num_type_t {
	num_u8,
	num_u16,
	num_u32,
	num_u64,
	num_uint,
	num_i8,
	num_i16,
	num_i32,
	num_i64,
	num_int,
	num_f32,
	num_f64,
	num_invalid,
};

class NumToken : public Token {
public:
	NumToken(char** s_ptr);
	~NumToken() = default;
	num_type_t num_type;
	union {
		uint64_t uint_val;
		int64_t int_val;
		double float_val;
	};
	// std::string str() const;
};
	
class Lexer {
public:
	Lexer(FILE* input = stdin, size_t bufsize=100)
		: input(input), bufsize(bufsize), linebuf((char*)malloc(bufsize)), linelen(0) {}
	~Lexer() { free(linebuf); }
	int advance();
	int gettok();
	FILE* input;
	ssize_t linelen;
	size_t bufsize;
	char* linebuf;
	std::string IdentifierStr; // Filled in if tok_identifier
};
