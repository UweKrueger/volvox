#pragma once
#if defined (_MSC_VER)
#include "../include/volvox-13.hh"
#endif
extern "C" {
#include "../lib/map.h"
}

class PrototypeAST;
class FunctionAST;
class VariableExprAST;
class CallExprAST;
class ExprAST;
class IfExprAST;
class ForExprAST;
class VarExprAST;
class UnaryExprAST;
class BinaryExprAST;

// The lexer returns tokens [0-255] if it is an unknown character, otherwise one
// of these for known things.
enum TokenKind {

	// operators - ordered by priority
	tok_assign = -2, // = (possibly multiple assignees, result(s): old value(s), right binding)
	tok_comma = -3,
	tok_arrow = -4, // <-
	tok_or = -5, // |, ^, ! (between bool or int, result: bool or int)
	tok_and = -6, // & (between bool or int, result: bool or int)
	// the following operators can be redfined for user types
	tok_cmp = -7, // >=, >, ==, !=, <, <=, <=>
	tok_add = -8, // +, -, ~
	tok_mult = -9, // *, /, %, <<, >>
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
	tok_return = -56,
	tok_end = -57,
	tok_leave = -58, // indicator that branch does not continue (i.e. last expr is return)

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

extern const char* input_file_name;
extern llvm::orc::ThreadSafeContext Context;
extern SourceLocation CurLoc;
extern bool inside_function;

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
	unsigned is_signed : 1; // signed int?
};

struct gen_val_type_t {
	llvm::Type::TypeID ID : 8; // base type
	unsigned SubclassData : 24;
};

struct FullType {
	llvm::Type* type;
	unsigned type_attr;
	int nelem; // nelem: -1 = flex-array, 0 = no array
	const char* type_name; // maybe NULL for anonymous types
	std::vector<std::pair<const char*, FullType>> struct_elems;
};

struct FullVar {
	FullType ft;
	llvm::Value* val;
};

struct ArgType {
	llvm::Type* type;
	char* f_name; // field name - or NULL for anonymous embedding (?)
	char* t_name; // type name
	unsigned type_attr;
	std::vector<ArgType> struct_elems; // for anonymous structs
};

// small hack to access protected method
class genType : protected llvm::Type {
public:
	unsigned SubClassData() const { return getSubclassData(); }
};

class TypeTable {
public:
	TypeTable() : name_table(map_string_new_map()) {}
	unsigned add(const char* name, llvm::Type* type, llvm::DIType* ditype, bool is_signed = false, int nelem = 0, std::vector<std::pair<const char*, FullType>> struct_elem = {}) {
		bool is_int = type->isIntegerTy();
		if (is_signed && !is_int)
			LogError("non-int type %s cannot be signed", name);
		MapValue val = {
			.src_ptr = is_signed ? (llvm::Type*)((uintptr_t)type | A_signed) : type
		};
		bool is_new = map_string_insert(&name_table, name, val, 0);
		if (is_new && !nelem && struct_elem.empty()) {
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
			printf("inserted %u %p %s\n", key, type, name);
			if (is_signed)
				typeptr_table[(llvm::Type*)((uintptr_t)type | A_signed)] = { name, ditype };
			else
				typeptr_table[type] = { name, ditype };
			return key;
		} else {
			return 0;
		}
	}
	llvm::Type* get_raw(const char* name) {
		MapValue* val = map_string_get(name_table, name);
		return val ? (llvm::Type*)val->src_ptr : nullptr;
	}
	llvm::Type* get(const char* name) {
		llvm::Type* raw_type = get_raw(name);
		return (llvm::Type*)((uintptr_t)raw_type & ~(uintptr_t)A_signed);
	}
	bool is_signed(const char* name) {
		llvm::Type* raw_type = get_raw(name);
		return (bool)((uintptr_t)raw_type & A_signed);
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
	FullType get_full(const char* name) {
		llvm::Type* raw_type = get_raw(name);
		return { (llvm::Type*)((uintptr_t)raw_type & ~0x01ULL), (bool)((uintptr_t)raw_type & A_signed) };
	}
	FullType get_full(unsigned _key) {
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
		if (!type) return nullptr;
		auto it = typeptr_table.find(type);
		return it == typeptr_table.end() ? nullptr : it->second.first;
	}
	const char* get_name(llvm::Type* type, bool is_signed) {
		if (!type) return nullptr;
		return get_name((llvm::Type*)((uintptr_t)type | (is_signed ? A_signed : 0)));
	}
	llvm::DIType* get_diType(llvm::Type* type) {
		if (!type) return nullptr;
		auto it = typeptr_table.find(type);
		return it == typeptr_table.end() ? nullptr : it->second.second;
	}
	llvm::DIType* get_diType(llvm::Type* type, bool is_signed) {
		if (!type) return nullptr;
		return get_diType((llvm::Type*)((uintptr_t)type | (is_signed ? A_signed : 0)));
	}
	~TypeTable() {
		map_destroy(name_table);
	}
protected:
	MapNode* name_table;
	std::map<unsigned, llvm::Type*> key32_table;
	std::map<llvm::Type*, std::pair<const char*, llvm::DIType*>> typeptr_table;
};

extern TypeTable type_table;
extern std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;

class VarTable {
protected:
	MapNode* table;
public:
	VarTable() : table(map_string_new_map()) {}
	~VarTable() { map_destroy(table); }
	void clear() {
		map_destroy(table);
		table = map_string_new_map();
	}
	bool insert(const char* key, const FullVar& value) {
		MapValue mv = { .src_ptr = const_cast<FullVar*>(&value) };
		auto res = map_string_insert(&table, key, mv, sizeof(FullVar));
		return res;
	}
	FullVar* operator[](const char* key) {
		MapValue* node = map_string_get(table, key);
		return node ? (FullVar*)((char*)node + node->offset) : nullptr;
	}
	bool erase(const char* name) {
		return map_string_delete(&table, name);
	}
};

extern VarTable globals_table;
extern std::vector<VarTable> locals_table; // including function arguments

// look up var and return if it's global
inline std::pair<FullVar*, bool> lookup_var(const char* Name) {
	for (int i = locals_table.size() - 1; i >= 0; i--) {
		FullVar* full_var = locals_table[i][Name];
		if (full_var)
			return { full_var, false };
	}
	return { globals_table[Name], true };
}

class ExprAST : public FullType {
public:
	SourceLocation Loc;

	llvm::Type* desired_type;
	unsigned desired_type_attr;
	int desired_nelem; // nelem: -1 = flex-array, 0 = no array
	const char* desired_type_name; // maybe NULL for anonymous types
	std::vector<std::pair<const char*, FullType>> desired_struct_elems;

	bool is_unknown_type;

	// construct from type and attributes
	ExprAST(SourceLocation Loc) : Loc(Loc) {}
	ExprAST(llvm::Type* type = llvm::Type::getDoubleTy(*Context.getContext()), unsigned type_attr = 0,
	        SourceLocation Loc = CurLoc, llvm::Type* desired_type = nullptr, unsigned desired_type_attr = 0,
	        bool is_unknown_type = false) :
		FullType(FullType{.type = type, .type_attr = type_attr}), Loc(Loc), desired_type(desired_type), desired_type_attr(desired_type_attr),
		is_unknown_type(is_unknown_type) {}
	ExprAST(std::pair<llvm::Type*, unsigned> p, SourceLocation Loc = CurLoc,
	        std::pair<llvm::Type*, unsigned> q = { nullptr, 0 }) :
		FullType(FullType{.type = p.first, .type_attr = p.second}), Loc(Loc), desired_type(q.first), desired_type_attr(q.second) {}
	// construct from key and attributes. The A_signed flag is already
	// looked up when the key is searched
	ExprAST(unsigned key, unsigned add_attr, SourceLocation Loc = CurLoc, bool is_unknown_type = false, llvm::Type* desired_type = nullptr,
	        unsigned desired_type_attr = 0) :
		FullType(type_table.get_full(key)), Loc(Loc), desired_type(desired_type), desired_type_attr(desired_type_attr), is_unknown_type(is_unknown_type)
		{
			type_attr |= add_attr;
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
	std::vector<llvm::DIScope *> LexicalBlocks;

	void emitLocation(ExprAST *AST);
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
extern std::unique_ptr<llvm::Module> TheModule;
extern std::unique_ptr<llvm::IRBuilder<>> Builder;
extern std::unique_ptr<llvm::DIBuilder> DBuilder;
extern std::unique_ptr<llvm::legacy::FunctionPassManager> TheFPM;

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
	int kind;
	bool is_unknown_type;
	Token(int kind = 0) : kind(kind) {}
	Token(char** s_ptr);
	Token(const std::string& str);
	Token(bool truth) : kind(tok_number) {
		Val.Uint = truth ? 1UL : 0UL;
		int_type = { .ID = llvm::Type::IntegerTyID, .BitWidth = 1, .is_signed = false };
	}
	static std::string tokName(int kind);
	std::string tokName() const { return tokName(kind); }
	union {
		int_val_type_t int_type;
		gen_val_type_t gen_type;
		unsigned key;
	};
	union LitValue Val;
	std::string str() const {
		switch (kind) {
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
				if (int_type.BitWidth == 1)
					if (Val.Uint & 1UL)
						return "true";
					else
						return "false";
				else if (int_type.is_signed)
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
extern Token purgeLine();

class Lexer {
public:
	Lexer(size_t bufsize = 0)
		: bufsize(bufsize), linebuf((char*)malloc(bufsize)), linelen(0) {}
	~Lexer() { free(linebuf); }
	int advance();
	Token gettok(bool expectBinary = false);
	Token purge_line();
	ssize_t linelen;
	size_t bufsize;
	char* linebuf;
};

extern Lexer lex;
extern FILE* input_file;
extern std::nullptr_t AutoErr(SourceLocation Loc, llvm::Type* expr_type, llvm::Type* desired_type,
                              unsigned expr_attr, unsigned desired_attr, const char* reason);
extern std::pair<bool, bool> analyze_types(std::pair<llvm::Type*, bool> a, std::pair<llvm::Type*, bool> b);
extern std::function<llvm::Value*(llvm::Value*)> getConv(
	llvm::Type* expr_type, llvm::Type* desired_type, unsigned expr_attr, unsigned desired_attr,
	SourceLocation Loc = CurLoc, bool is_explicit = false, bool is_unknown_type = false);
extern std::nullptr_t HandleGlobalVariable(BinaryExprAST* expr);
extern void InitializeModuleAndPassManager();
extern std::unique_ptr<llvm::orc::VolvoxJIT> TheJIT;
extern thread_local char* __volvox_jit_tls_ptr;
extern thread_local size_t __volvox_jit_tls_size;
extern char* __volvox_jit_tls_inits;
extern llvm::Function* PrepareFunctionBody(std::unique_ptr<PrototypeAST> Proto);
extern void FinishFunction(llvm::Function* TheFunction, llvm::Value* RetVal);
extern std::nullptr_t Error(SourceLocation Loc, const char *Str, ...);
extern std::tuple<llvm::Type*, std::function<llvm::Value*(llvm::Value*)>, bool> MakeType(llvm::Type* type, bool is_signed, bool is_unknown_type);
