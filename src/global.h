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

	// operators - ordered by priority
	tok_assign = -1, // = (possibly multiple assignees, result(s): old value(s), right binding)
	tok_comma = -2,
	tok_arrow = -3, // <-
	tok_or = -5, // || (between bool, result: bool)
	tok_and = -6, // && (between bool, result: bool)
	// the following operators can be redfined for user types
	tok_cmp = -7, // >=, >, ==, !=, <, <=, <=>
	tok_add = -8, // +, -, |, ^, !, ~
	tok_mult = -9, // *, /, %, <<, >>, &
	tok_unary = -10, // +, -, !, ~, &, <-
	tok_pow = -11, // **
	tok_postfix = -12, // ++, -- (return old result)
	tok_colon = -13,

	tok_eof = -20,

	// commands
	tok_fn = -30,
	tok_extern = -31,

	// primary
	tok_identifier = -40,
	tok_number = -41,
	tok_str_lit = -42,

	// control
	tok_if = -50,
	tok_then = -51,
	tok_else = -52,
	tok_for = -53,
	tok_in = -54,
	// var definition
	tok_var = -55,

	// built-in type attributes
	tok_atomic = -60,
	tok_shared = -61,
	tok_iso = -62,
	tok_const = -63,
	
	tok_self = -70,
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


// small hack to access protected method
class genType : protected llvm::Type {
public:
	unsigned SubClassData() const { return getSubclassData(); }
};

class TypeTable {
public:
	bool add(const char* name, llvm::Type* type, bool is_signed = false) {
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
	llvm::Type* get_raw(const char* name) {
		auto it = name_table.find(name);
		return it == name_table.end() ? nullptr : it->second;
	}
	llvm::Type* get(const char* name) {
		auto it = name_table.find(name);
		return it == name_table.end() ? nullptr : (llvm::Type*)((uintptr_t)it->second & ~0x01ULL);
	}
	bool is_signed(const char* name) {
		auto it = name_table.find(name);
		return ((uintptr_t)it->second & 0x01ULL) != 0;
	}
	std::pair<bool, llvm::Type*> get_full(char* name) {
		auto it = name_table.find(name);
		return { ((uintptr_t)it->second & 0x01ULL) != 0, it == name_table.end() ? nullptr : (llvm::Type*)((uintptr_t)it->second & ~0x01ULL) };
	}
	~TypeTable() = default;
protected:
	std::map<const char*, llvm::Type*> name_table;
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
	static std::string tokName(int type);
	std::string tokName() const { return tokName(type); }
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
extern Token getNextToken(bool expectBinary = false);

class Lexer {
public:
	Lexer(FILE* input = stdin, size_t bufsize=100)
		: input(input), bufsize(bufsize), linebuf((char*)malloc(bufsize)), linelen(0) {}
	~Lexer() { free(linebuf); }
	int advance();
	Token gettok(bool expectBinary = false);
	FILE* input;
	ssize_t linelen;
	size_t bufsize;
	char* linebuf;
};

extern Lexer lex;

// Types
extern llvm::Type* _f64;
extern llvm::Type* _string;
