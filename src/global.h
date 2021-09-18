#pragma once

class PrototypeAST;
class FunctionAST;
class VariableExprAST;
class CallExprAST;
class ExprAST;
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
	tok_last_op = -14, // only used for comparisons to identify operators

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

// Types
extern unsigned stringkey;

extern llvm::LLVMContext TheContext;
extern SourceLocation CurLoc;

/// ExprAST - Base class for all expression nodes.

// Type Attributes
#define A_signed (1U<<0)
#define A_const  (1U<<1)
#define A_shared (1U<<2)
#define A_iso    (1U<<3)
#define A_atomic (1U<<4)

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
	unsigned add(const char* name, llvm::Type* type, bool is_signed = false) {
		bool is_int = type->isIntegerTy();
		if (is_signed && !is_int)
			LogError("non-int type %s cannot be signed", name);
		auto it = name_table.insert({ name, is_signed ? (llvm::Type*)((uintptr_t)type | A_signed) : type });
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
			key32_table[key] = type;
			if (!is_signed)
				type_table[type] = name;
			return key;
		} else {
			return 0;
		}
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
		return (bool)((uintptr_t)it->second & A_signed);
	}
	static bool is_signed(unsigned _key) {
		union {
			int_val_type_t int_type;
			gen_val_type_t gen_type;
			unsigned key;
		};
		key = _key;
		return int_type.ID == llvm::Type::IntegerTyID && int_type.is_signed;
	}
	std::pair<llvm::Type*, bool> get_full(const char* name) {
		auto it = name_table.find(name);
		return { it == name_table.end() ? nullptr : (llvm::Type*)((uintptr_t)it->second & ~0x01ULL), ((uintptr_t)it->second & 0x01ULL) != 0 };
	}
	std::pair<llvm::Type*, bool> get_full(unsigned _key) {
		union {
			int_val_type_t int_type;
			gen_val_type_t gen_type;
			unsigned key;
		};
		key = _key;
		auto it = key32_table.find(key);
		bool is_signed = (int_type.ID == llvm::Type::IntegerTyID && int_type.is_signed);
		return { it == key32_table.end() ? nullptr : it->second, is_signed };
	}
	const char* get_name(llvm::Type* type) {
		auto it = type_table.find(type);
		return it == type_table.end() ? nullptr : it->second;
	}
	~TypeTable() = default;
protected:
	std::map<const char*, llvm::Type*> name_table;
	std::map<unsigned, llvm::Type*> key32_table;
	std::map<llvm::Type*, const char*> type_table;
};

extern TypeTable type_table;
extern std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;

class ExprAST {
	SourceLocation Loc;

public:
	llvm::Type* type;
	unsigned type_attr;
	// construct from type and attributes
	ExprAST(llvm::Type* type = llvm::Type::getDoubleTy(TheContext), unsigned type_attr = 0, SourceLocation Loc = CurLoc) : Loc(Loc), type(type), type_attr(type_attr) {}
	ExprAST(std::pair<llvm::Type*, unsigned> p, SourceLocation Loc = CurLoc) : Loc(Loc), type(p.first), type_attr(p.second) {}
	// construct from key and attributes. The A_signed flag is already
	// looked up when the key is searched
	ExprAST(unsigned key, unsigned add_attr, SourceLocation Loc = CurLoc)  : Loc(Loc) {
		auto fulltype = type_table.get_full(key);
		type = fulltype.first;
		type_attr = (fulltype.second ? A_signed : 0) | add_attr;
	}
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


// Token

union LitValue {
	uint64_t Uint;
	int64_t Int;
	double Float;
	char* Str;
};

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
	union LitValue Val;
	std::string str() const {
		switch (type) {
		case tok_identifier:
		case tok_assign:
		case tok_cmp:
		case tok_add:
		case tok_mult:
		case tok_unary:
		case tok_postfix:
			return IdentifierStr;
		case tok_number:
			switch (int_type.ID) {
			case llvm::Type::IntegerTyID:
				if (int_type.is_signed)
					return std::to_string(Val.Int);
				else
					return std::to_string(Val.Uint);
			case llvm::Type::HalfTyID:
			case llvm::Type::BFloatTyID:
			case llvm::Type::FloatTyID:
			case llvm::Type::DoubleTyID:
				return std::to_string(Val.Float);
			default:
				fprintf(stderr, "internal compiler error: cannot print numeric literal of type %d\n", int_type.ID);
				return "";
			}
		case tok_str_lit:
			return Val.Str;
		default:
			return this->tokName();
		}
	}
};
	
extern Token CurTok;
extern Token getNextToken(bool expectBinary = false);

class Lexer {
public:
	Lexer(FILE* input = stdin, size_t bufsize = 0)
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
