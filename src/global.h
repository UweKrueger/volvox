#pragma once

class PrototypeAST;
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

struct SourceLocation {
	int Line;
	int Col;
};

extern llvm::Type* _f64;
extern SourceLocation CurLoc;

/// ExprAST - Base class for all expression nodes.
class ExprAST {
	SourceLocation Loc;

public:
	llvm::Type* type;
	ExprAST(llvm::Type* type = _f64, SourceLocation Loc = CurLoc) : Loc(Loc), type(type) {}
	virtual ~ExprAST() {}
	virtual llvm::Value *codegen() = 0;
	int getLine() const { return Loc.Line; }
	int getCol() const { return Loc.Col; }
	virtual llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) {
		return out << ':' << getLine() << ':' << getCol() << '\n';
	}
};

struct DebugInfo {
	llvm::DICompileUnit *TheCU;
	llvm::DIType *DblTy;
	std::vector<llvm::DIScope *> LexicalBlocks;

	void emitLocation(ExprAST *AST);
	llvm::DIType *getDoubleTy();
};

extern DebugInfo KSDbgInfo;

enum CompModes {
	comp_jit,
	comp_obj,
	comp_dbg
};

extern CompModes comp_mode;
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

struct int_val_type_t {
	llvm::Type::TypeID ID : 8; // base type
	unsigned BitWidth : 23; // #bits for int types, 0 for default
	bool is_signed : 1; // signed int?
};

struct gen_val_type_t {
	llvm::Type::TypeID ID : 8; // base type
	unsigned SubclassData : 24;
};

class genType : protected llvm::Type {
public:
	unsigned SubClassData() const { return getSubclassData(); }
};

class TypeTable {
public:
	bool add(char* name, llvm::Type* type, bool is_signed = false) {
		bool is_int = type->isIntegerTy();
		if (is_signed && !is_int)
			LogError("non-int type %s cannot be signed", name);
		unsigned char sign = (is_int && is_signed) ? 0x01 : 0x00;
		auto it = name_table.insert({name, (llvm::Type*)((uintptr_t)type | sign)});
		if (it.second) {
			union {
				int_val_type_t int_type;
				gen_val_type_t gen_type;
				unsigned key;
			};
			if (is_int) {
				int_type = { .ID = type->getTypeID(), .BitWidth = type->getIntegerBitWidth(), .is_signed = is_signed };
			} else {
				gen_type = { .ID = type->getTypeID(), .SubclassData = ((genType*)type)->SubClassData() };
			}
		}
		return it.second;
	}
	llvm::Type* get(char* name) {
		auto it = name_table.find(name);
		return it == name_table.end() ? nullptr : (llvm::Type*)((uintptr_t)it->second & ~0x01ULL);
	}
	bool is_signed(char* name) {
		auto it = name_table.find(name);
		return ((uintptr_t)it->second & 0x01ULL) != 0;
	}
	std::pair<bool, llvm::Type*> getfull(char* name) {
		auto it = name_table.find(name);
		return { ((uintptr_t)it->second & 0x01ULL) != 0, it == name_table.end() ? nullptr : (llvm::Type*)((uintptr_t)it->second & ~0x01ULL) };
	}
	~TypeTable() {
		for (auto it = name_table.begin(); it != name_table.end(); it = name_table.erase(it))
			free(it->second);
	}
protected:
	std::map<char*, llvm::Type*> name_table;
	std::map<unsigned, llvm::Type*> key32_table;
};

extern TypeTable type_table;

// Token

class Token {
public:
	int type;
	Token(int type = 0) : type(type) {}
	Token(char** s_ptr);
	Token(const std::string& str);
	std::string tokName() const;
	union {
		int_val_type_t int_type;
		gen_val_type_t gen_type;
		unsigned key;
	};
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
