#pragma once
#if defined (_MSC_VER)
#include "../include/volvox-14.hh"
#include <windows.h>
#include <synchapi.h>
#include <processthreadsapi.h>
#endif
#include "../lib/map.h"
#include "../lib/types.h"
#include "../lib/str.h"

#if defined(_MSC_VER)
/* The volvox run time library uses Itanium/GNU mangling which are not
 * supported by MSVC. To use those library functions inside the compiler
 * we have to provide fake "C" declarations */
extern "C" {
	union MapValue {
		union {
			unsigned long long int u64;
			long long int i64;
			unsigned int u32;
			int i32;
			float f32;
			double f64;
			struct {
				unsigned int offset;
				unsigned int size;
			};
			void* src_ptr; // to pass generic value to `insert()`
		};
	};

	union MapKey {
		unsigned long long int u64;
		long long int i64;
		unsigned int u32;
		int i32;
		float f32;
		double f64;
		char string[8]; // will expand dynamically
	};

	struct MapNode {
		union {
			struct Node* parent;
			int bf : 2;
			unsigned u_bf : 2;
		};
		struct Node* leftChild;
		struct Node* rightChild;
		MapValue value;
		MapKey key;
	};
#define map_string_new_map _ZN6volvox3map11num_new_mapEv
#define map_string_insert _ZN6volvox3map13string_insertEPPNS0_4NodeEPKcNS0_5ValueEib
#define map_string_tag_insert _ZN6volvox3map17string_tag_insertEPPNS0_4NodeEPKcjNS0_5ValueEib
#define map_string_get _ZN6volvox3map10string_getEPNS0_4NodeEPKc
#define map_destroy _ZN6volvox3map7destroyEPNS0_4NodeE
#define map_iter_up _ZN6volvox3map7iter_upEPNS0_4NodeE
#define map_string_delete _ZN6volvox3map13string_deleteEPPNS0_4NodeEPKc
#define map_min _ZN6volvox3map3MinEPNS0_4NodeE
	_DECL MapNode* map_string_new_map();
	_DECL MapNode* map_string_insert(MapNode** root_ptr, const char* key, MapValue value, int value_size, bool allow_replace);
	_DECL MapNode* map_string_tag_insert(MapNode** root_ptr, const char* key, unsigned tag, MapValue value, int value_size, bool allow_replace);
	_DECL MapValue* map_string_get(MapNode* root, const char* key);
	_DECL void map_destroy(MapNode* root);
	_DECL MapNode* map_iter_up(MapNode* elem);
	_DECL bool map_string_delete(MapNode** root_ptr, const char* key);
	_DECL MapNode* map_min(MapNode* node);
}
#else
#define MapNode volvox::map::Node
#define MapValue volvox::map::Value
#define map_string_new_map volvox::map::string_new_map
#define map_string_insert volvox::map::string_insert
#define map_string_tag_insert volvox::map::string_tag_insert
#define map_string_get volvox::map::string_get
#define map_destroy volvox::map::destroy
#define map_iter_up volvox::map::iter_up
#define map_string_delete volvox::map::string_delete
#define map_min volvox::map::Min
#endif

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

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a)/sizeof(a[0]))
#endif

// The lexer returns tokens [0-255] if it is an unknown character, otherwise one
// of these for known things.
#define TOKEN(x) , tok_##x
#define TOKBEGIN token_beg
#define TOKEND token_end
#include "token.def"
enum TokenKind : int { tok_1st_keyword = 2 + TOKBEGIN - TOKEND
                 TOKENS
};

extern const char* tokens[];

#define SHARE_KIND_MASK (7 << 1)

enum SymbolKind : unsigned {
	is_private = (0 << 1), // variable that is only visible in main
	is_global = (1 << 1), // TLS variable, visible in all functions of module
	is_atomic = (2 << 1), // up to 64 bit SingleValue variables
	is_shared = (3 << 1), // mutex + pointer to heap allocated data
	is_unique = (4 << 1), // unique pointer to heap allocated data
	is_const = (5 << 1), // data segment, heap (if variable size), CT const
	is_pub = 1 << 4,
	is_fn = 1 << 5,
	is_decl = 1 << 6,
	is_c_api = 1 << 7,
	is_inline = 1 << 8
};

// Colors - we map those from llvm::raw_ostream but add boldness where needed
// Foreground Colors:
struct Colors {
	llvm::raw_ostream::Colors col;
	bool bold;
};

static constexpr Colors BLACK = { llvm::raw_ostream::Colors::BLACK, false };
static constexpr Colors GRAY = { llvm::raw_ostream::Colors::BLACK, true };
static constexpr Colors RED = { llvm::raw_ostream::Colors::RED, true };
static constexpr Colors DARKRED = { llvm::raw_ostream::Colors::RED, false };
static constexpr Colors GREEN = { llvm::raw_ostream::Colors::GREEN, true };
static constexpr Colors DARKGREEN = { llvm::raw_ostream::Colors::GREEN, false };
static constexpr Colors YELLOW = { llvm::raw_ostream::Colors::YELLOW, true };
static constexpr Colors BROWN = { llvm::raw_ostream::Colors::YELLOW, false };
static constexpr Colors BLUE = { llvm::raw_ostream::Colors::BLUE, true };
static constexpr Colors DARKBLUE = { llvm::raw_ostream::Colors::BLUE, false };
static constexpr Colors MAGENTA = { llvm::raw_ostream::Colors::MAGENTA, true };
static constexpr Colors DARKMAGENTA = { llvm::raw_ostream::Colors::MAGENTA, false };
static constexpr Colors CYAN = { llvm::raw_ostream::Colors::CYAN, true };
static constexpr Colors DARKCYAN = { llvm::raw_ostream::Colors::CYAN, false };
static constexpr Colors WHITE = { llvm::raw_ostream::Colors::WHITE, true };
static constexpr Colors DARKWHITE = { llvm::raw_ostream::Colors::WHITE, false };
static constexpr llvm::raw_ostream::Colors SAVEDCOLOR = llvm::raw_ostream::Colors::SAVEDCOLOR;
static constexpr llvm::raw_ostream::Colors RESET = llvm::raw_ostream::Colors::RESET;

inline llvm::raw_ostream& operator<<(llvm::raw_ostream& out, Colors color) {
	return out.changeColor(color.col, color.bold);
}

// some handy output stream definitions
// these just bring LLVM's definitions into the global namespace
inline llvm::raw_ostream& errs() {
	return llvm::errs();
}

inline llvm::raw_ostream& outs() {
	return llvm::outs();
}

// 3 colors from the ANSI 256 color palette
struct promptcolor_t {
	uint8_t number;
	uint8_t greater;
	uint8_t background;
};

extern promptcolor_t p_col;

// the following output streams only output on stderr if one or more '-v' options were given  
extern int verbosity;

// hints to possibe problems that might be interesting for debugging the program being compiled
inline llvm::raw_ostream& hints() {
	if (verbosity >= 1)
		return llvm::errs();
	else
		return llvm::nulls();
}

// infos that should not be relevant for finding problems
inline llvm::raw_ostream& infos() {
	if (verbosity >= 2)
		return llvm::errs();
	else
		return llvm::nulls();
}

// infos that are only useful for debugging the Volvox compiler
inline llvm::raw_ostream& dbgs() {
	if (verbosity >= 3)
		return llvm::errs();
	else
		return llvm::nulls();
}

struct SourceLocation {
	const char* File;
	int Line;
	int Col;
};

extern const char* input_file_name;
#if LLVM_VERSION_MAJOR >= 12
extern llvm::orc::ThreadSafeContext TS_Context;
#define Context *TS_Context.getContext()
#else
extern llvm::LLVMContext Context;
#endif
extern SourceLocation CurLoc;
extern bool inside_function;
extern int prompt_indent;

namespace volvoxc {

	struct FullType {
		llvm::Type* type; // used by compiler
		unsigned type_attr; // signed, atomic, shared, iso, ref, num_indices
		SymbolKind kind;
		const char* type_name; // maybe NULL for anonymous types
		llvm::DIType* ditype;
		union {
			FullType* elem_type; // for array or tuples
			//PrototypeAST* proto; // for functions
			std::vector<std::unique_ptr<PrototypeAST>>* Protos; // for overloaded functions
			MapNode* fields;     // for structs
		};
		void dump(int fd = 2);
	};

	/* Named types can be kept in a map using the name as key.
	   anonymous types must be kept too - in a way that allows freeing
	   them when not needed anymore.
	   This can be done in a single linked list */

	struct FTListElem {
		FTListElem* next;
		FullType ft;
	};

}

extern llvm::ArrayType* MakeInterfaceArrayType(llvm::ArrayType* array_type);

inline llvm::raw_ostream& operator<<(llvm::raw_ostream& out, SourceLocation& Loc) {
	return out << Loc.File << ":" << Loc.Line << ":" << Loc.Col;
}

extern std::nullptr_t AutoErr(SourceLocation Loc, llvm::Type* expr_type, llvm::Type* desired_type,
                              unsigned expr_attr, unsigned desired_attr, const char* reason);
extern std::pair<bool, bool> analyze_types(std::pair<llvm::Type*, bool> a, std::pair<llvm::Type*, bool> b);
extern std::function<llvm::Value*(llvm::Value*)> getConv(
	llvm::Type* expr_type, llvm::Type* desired_type, unsigned expr_attr, unsigned desired_attr,
	SourceLocation Loc = CurLoc, bool is_explicit = false, bool is_unknown_type = false);
extern std::function<llvm::Value*(llvm::Value*)> getBestPreConv(SourceLocation Loc, llvm::Type* desired_type,
                                                                llvm::Type* min_type, llvm::Type* ideal_type,
                                                                std::function<llvm::Value*(llvm::Value*)> min_conv,
                                                                std::function<llvm::Value*(llvm::Value*)> ideal_conv,
                                                                bool is_signed);
extern llvm::Value* NoConversion(llvm::Value* v);
// often used types - for faster access
extern llvm::Type* llvm_int_type;
extern llvm::Type* llvm_size_type;
extern llvm::Type* llvm_bool_type;
extern volvoxc::FullType* void_type;
extern volvoxc::FullType* uintptr_type;
extern const char* last_shadow_saver;
extern const char* last_shadow_restorer;

extern unsigned anon_struct_nr;
extern llvm::SmallString<128> MangleBase(std::vector<const char*>& names);
extern llvm::SmallString<128> Mangle(std::vector<const char*>& names, std::vector<volvoxc::FullType*>& arg_types);
extern std::unique_ptr<FunctionAST> ParseDefinition(unsigned share_kind);
extern std::unique_ptr<ExprAST> GetTopLevelExpression();
extern std::unique_ptr<FunctionAST> ParseTopLevelExpr();
extern std::unique_ptr<ExprAST> ParseExpression();
extern std::unique_ptr<PrototypeAST> ParseExtern(unsigned share_kind);
extern bool spawn_bool_expr(bool (*expr)());

// classification for next token
// in general how newline is translated
// eBinOp decides if a following operator is unary or binary
enum eXpect {
	eNone,
	eBinOp, // newline translates to ;
	eComma,
	eColon,
	ePath,
	eType
};

extern volvoxc::FullType* ParseType(
	bool allow_attribute = false, eXpect expect = eComma,
	const char* tname = nullptr,
	std::vector<std::unique_ptr<ExprAST>>* exprs = nullptr,
	bool is_index = false);
extern llvm::Constant* getRtType(volvoxc::FullType* ft);
extern llvm::Constant* getRtType(volvoxc::FullType* ft);
extern std::pair<llvm::Function*, PrototypeAST*> getFunction(
	std::string unmangledName, std::vector<volvoxc::FullType*>* ArgTypes);
extern std::pair<unsigned, bool> getBitWidth(llvm::Type* type);
extern void PrepareTestFramework();
extern const char* TestFunction;
extern bool do_test;

struct int_val_type_t {
	llvm::Type::TypeID ID : 8; // base type
	unsigned BitWidth : 23; // #bits for int types, 0 for default
	unsigned is_signed : 1; // signed int?
};

struct FullVar {
	union {
		llvm::Value* val;
		llvm::Type* storage_type; // for global variables
	};
	volvoxc::FullType ft;
};

struct FVListElem {
	FVListElem* next;
	FullVar fv;
};

extern FVListElem* anon_fullvars;
extern FVListElem** anon_fullvars_end;

inline FullVar* new_FullVar(llvm::Value* val, llvm::Type* type, unsigned type_attr,
                            const char* type_name = nullptr, llvm::DIType* ditype = nullptr,
                            volvoxc::FullType* elem_type = nullptr) {
	FVListElem* new_node = (FVListElem*)malloc(sizeof(FVListElem));
	new_node->next = nullptr;
	new_node->fv.val = val;
	new_node->fv.ft.type = type;
	new_node->fv.ft.type_attr = type_attr;
	new_node->fv.ft.type_name = type_name;
	new_node->fv.ft.ditype = ditype;
	new_node->fv.ft.elem_type = elem_type;
	*anon_fullvars_end = new_node;
	anon_fullvars_end = &new_node->next;
	return &new_node->fv;
};

// small hack to access protected method
class genType : protected llvm::Type {
public:
	unsigned SubClassData() const { return getSubclassData(); }
};

extern volvoxc::FTListElem* anon_types;
extern volvoxc::FTListElem** anon_types_end;

inline volvoxc::FullType* new_FullType(llvm::Type* type, unsigned type_attr, llvm::DIType* ditype = nullptr,
                                       volvoxc::FullType* elem_type = nullptr) {
	volvoxc::FTListElem* new_node = (volvoxc::FTListElem*)malloc(sizeof(volvoxc::FTListElem));
	new_node->next = nullptr;
	new_node->ft.type = type;
	new_node->ft.type_attr = type_attr;
	new_node->ft.type_name = nullptr; // it's an anonymous type
	new_node->ft.ditype = ditype;
	new_node->ft.elem_type = elem_type;
	*anon_types_end = new_node;
	anon_types_end = &new_node->next;
	return &new_node->ft;
}

inline volvoxc::FullType* new_FullType(const volvoxc::FullType& orig) {
	volvoxc::FTListElem* new_node = (volvoxc::FTListElem*)malloc(sizeof(volvoxc::FTListElem));
	new_node->next = nullptr;
	new_node->ft = orig;
	*anon_types_end = new_node;
	anon_types_end = &new_node->next;
	return &new_node->ft;
}

class TypeTable {
public:
	TypeTable() : name_table(map_string_new_map()) {}
	unsigned add(const char* name, volvoxc::FullType* ft) {
		bool is_int = ft->type->isIntegerTy();
		if ((ft->type_attr & A_signed) && !is_int) {
			errs() << "non-int type '" << name << "' aka " << *ft->type << " cannot be signed\n";
			return 0;
		}
		MapValue val = {
			.src_ptr = ft
		};
		MapNode* new_node = map_string_insert(&name_table, name, val, sizeof(volvoxc::FullType), false);
		if (new_node) {
			((volvoxc::FullType*)((char*)&(new_node->value) + new_node->value.offset))->type_name = new_node->key.string;
			union {
				int_val_type_t int_type;
				VOLVOX_gen_val_type_t gen_type;
				unsigned key;
			};
			if (is_int) {
				int_type = { .ID = ft->type->getTypeID(), .BitWidth = ft->type->getIntegerBitWidth(), .is_signed = (bool)(ft->type_attr & A_signed) };
			} else {
				gen_type = { .ID = (VOLVOX_TypeID)ft->type->getTypeID(), .SubclassData = ((genType*)ft->type)->SubClassData() };
			}
			key32_table[key] = ft->type;
			if (ft->type_attr & A_signed)
				typeptr_table[(llvm::Type*)((uintptr_t)ft->type | A_signed)] = { name, ft->ditype };
			else
				typeptr_table[ft->type] = { name, ft->ditype };
			return key;
		} else {
			errs() << "Cannot add new type '" << name << "' - name already exists\n";
			return 0;
		}
	}
	unsigned add(const char* name, llvm::Type* type, llvm::DIType* ditype, unsigned type_attr = 0, MapNode* fields = nullptr) {
		volvoxc::FullType ft = {
			.type = type,
			.type_attr = type_attr,
			.ditype = ditype,
			.fields = fields
		};
		return add(name, &ft);
	}
	llvm::Type* get(const char* name) {
		MapValue* val = map_string_get(name_table, name);
		return ((volvoxc::FullType*)((char*)val + val->offset))->type;
	}
	bool is_signed(const char* name) {
		MapValue* val = map_string_get(name_table, name);
		return (bool)(((volvoxc::FullType*)((char*)val + val->offset))->type_attr & A_signed);
	}
	static bool is_signed(unsigned _key) {
		union {
			int_val_type_t int_type;
			VOLVOX_gen_val_type_t gen_type;
			unsigned key;
		};
		key = _key;
		return int_type.ID == llvm::Type::IntegerTyID && int_type.is_signed;
	}
	volvoxc::FullType* get_full(const char* name) {
		MapValue* val = map_string_get(name_table, name);
		return (volvoxc::FullType*)(val ? (char*)val + val->offset : nullptr);
	}
	volvoxc::FullType* get_full(unsigned _key) {
		union {
			int_val_type_t int_type;
			VOLVOX_gen_val_type_t gen_type;
			unsigned key;
		};
		key = _key;
		auto it = key32_table.find(key);
		bool is_signed = (int_type.ID == llvm::Type::IntegerTyID && int_type.is_signed);
		return new_FullType(it == key32_table.end() ? nullptr : it->second, is_signed ? A_signed : 0);
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
extern std::map<std::string, std::vector<std::unique_ptr<PrototypeAST>>> FunctionProtos;
extern MapNode* keyword_toks;

extern void init_token_map();

class VarTable {
public:
	MapNode* table;
	VarTable() : table(map_string_new_map()) {}
	~VarTable() { map_destroy(table); }
	VarTable(VarTable&& o) { table = o.table; o.table = nullptr; }
	VarTable& operator=(VarTable&& o) { table = o.table; o.table = nullptr; return *this; }
	void clear() {
		map_destroy(table);
		table = map_string_new_map();
	}
	bool insert(const char* key, const FullVar& value) {
		MapValue mv = { .src_ptr = const_cast<FullVar*>(&value) };
		auto res = map_string_insert(&table, key, mv, sizeof(FullVar), false);
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
extern unsigned condnesting;
extern VarTable* IfWhileVarTable;

// look up var and return if it's global
inline std::pair<FullVar*, bool> lookup_var(const char* Name) {
	for (int i = locals_table.size() - 1; i >= 0; i--) {
		FullVar* full_var = locals_table[i][Name];
		if (full_var)
			return { full_var, false };
	}
	return { globals_table[Name], true };
}

enum NameKind {
	NK_Module,
	NK_Type,
	NK_FunctionAlias,
	NK_VariableAlias
};

struct NsItem {
	NameKind kind;
	union {
		volvoxc::FullType* ft;
		PrototypeAST* proto;
		FullVar* var;
	};
};

class NameTable {
protected:
		MapNode* table;
public:
	NameTable() : table(map_string_new_map()) {}
	~NameTable() { map_destroy(table); }
	void clear() {
		map_destroy(table);
		table = map_string_new_map();
	}
	bool insert(const char* key, const NsItem& value) {
		MapValue mv = { .src_ptr = const_cast<NsItem*>(&value) };
		auto res = map_string_insert(&table, key, mv, sizeof(NsItem), false);
		return res;
	}
	NsItem* operator[](const char* key) {
		MapValue* node = map_string_get(table, key);
		return node ? (NsItem*)((char*)node + node->offset) : nullptr;
	}
	bool erase(const char* name) {
		return map_string_delete(&table, name);
	}
};

extern NameTable name_table;

/// ExprAST - Base class for all expression nodes.
class ExprAST {
public:
	SourceLocation Loc;
	volvoxc::FullType* ft;
	llvm::Type* desired_type = nullptr;
	unsigned desired_type_attr = 0;
	const char* desired_type_name = nullptr; // maybe NULL for anonymous types
	MapNode* desired_elems = nullptr; // element-name -> { index, FullType }

	bool is_unknown_type = false;

	// construct from type and attributes
	ExprAST(SourceLocation Loc) : ft(new_FullType(nullptr, 0)), Loc(Loc) {}
	ExprAST(llvm::Type* type = llvm::Type::getVoidTy(Context), unsigned type_attr = 0,
	        SourceLocation Loc = CurLoc, bool is_unknown_type = false)
		: ft(new_FullType(type, type_attr)), Loc(Loc),
		  is_unknown_type(is_unknown_type) {}
	ExprAST(std::pair<llvm::Type*, unsigned> p, SourceLocation Loc = CurLoc)
		: ft(new_FullType(p.first, p.second)), Loc(Loc) {}
	// construct from key and attributes. The A_signed flag is already
	// looked up when the key is searched
	ExprAST(unsigned key, unsigned add_attr, SourceLocation Loc = CurLoc,
	        bool is_unknown_type = false)
		: ft(type_table.get_full(key)), Loc(Loc), is_unknown_type(is_unknown_type)
		{
			// abort(); - find out where this is used
			ft->type_attr |= add_attr;
		}
	ExprAST(volvoxc::FullType* full_type, SourceLocation Loc = CurLoc, bool is_unknown_type = false)
		: ft(full_type ? full_type : new_FullType(nullptr, 0)), Loc(Loc), is_unknown_type(is_unknown_type) {}
	virtual ~ExprAST() {}
	virtual llvm::Value *codegen_raw() = 0;
	llvm::Value* codegen() {
		auto rawV = codegen_raw();
		if (desired_type && rawV && !rawV->getType()->isVoidTy()) {
			auto postConv = getConv(rawV->getType(), desired_type, ft->type_attr, desired_type_attr,
			                        Loc, true, is_unknown_type);
			return postConv(rawV);
		} else {
			return rawV;
		}
	}	
	int getLine() const { return Loc.Line; }
	int getCol() const { return Loc.Col; }
#ifndef NDEBUG
	virtual llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) {
		return out << ':' << getLine() << ':' << getCol() << '\n';
	}
#endif
};

inline llvm::raw_ostream& operator<<(llvm::raw_ostream& out, std::unique_ptr<ExprAST>& expr) {
	if (expr)
		out << "expr"; // *expr->codegen();
	else
		out << "nil";
	return out;
}

template<typename T> llvm::raw_ostream& operator<<(llvm::raw_ostream& out, std::vector<T>& vec) {
	for (int i = 0; i < vec.size(); i++)
		out << (i ? ", " : "[ ") << vec[i]; 
	return out << " ]";
}

struct DebugInfo {
	llvm::DICompileUnit *TheCU;
	std::vector<llvm::DIScope *> LexicalBlocks;

	void emitLocation(ExprAST *AST);
};

extern DebugInfo KSDbgInfo;

enum CompModes {
	comp_undefined = 0,
	comp_jit,
	comp_obj,
	comp_dbg,
};

enum LinkModes {
	link_undefined = 0,
	do_link,
	dont_link,
};

extern CompModes comp_mode;
extern LinkModes link_mode;
extern unsigned dump_IR;

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
	void* Ptr;
};

class Token {
public:
	TokenKind kind; // often treated as int as positive values represent ASCII chars
	union {
		int_val_type_t int_type;
		VOLVOX_gen_val_type_t gen_type;
		unsigned key;
	};
	union LitValue Val;
	bool is_unknown_type = false;

	Token(int _kind = 0);
	Token(char** s_ptr);
	Token(void* ptr);
	Token(const std::string& str);
	Token(bool truth) : kind(tok_number) {
		Val.Uint = truth ? 1UL : 0UL;
		int_type = { .ID = llvm::Type::IntegerTyID, .BitWidth = 1, .is_signed = false };
	}
	Token(long long n) : kind(tok_number) {
		Val.Int = n;
		int_type = { .ID = llvm::Type::IntegerTyID, .BitWidth = 32, .is_signed = true };
		is_unknown_type = true;
	}
	Token(unsigned long long n) : kind(tok_number) {
		Val.Int = n;
		int_type = { .ID = llvm::Type::IntegerTyID, .BitWidth = 64, .is_signed = false };
	}
	Token(double x) : kind(tok_number) {
		Val.Float = x;
		gen_type = { .ID = VOLVOX_DoubleTyID };
		is_unknown_type = true;
	}
	std::string str() const;
};

llvm::raw_ostream& operator<<(llvm::raw_ostream& out, TokenKind kind);
llvm::raw_ostream& operator<<(llvm::raw_ostream& out, Token& tok);

extern Token CurTok;
extern Token getNextToken(eXpect expect = eNone);
extern Token purgeLine();

class Lexer {
public:
	Lexer(size_t bufsize = 100)
		: bufsize(bufsize), linebuf((char*)malloc(bufsize)), linelen(0) {}
	virtual ~Lexer() { free(linebuf); }
	int advance();
	Token gettok(eXpect expect = eNone);
	Token purge_line();
	char peek();
	char peek_strict();
	char look_back_strict();
	ssize_t linelen;
	size_t bufsize;
	char* linebuf;
	int CurChar = ' ';
	// c can be the last char of an expression so the following "[n]" is an index
	static bool is_expr_end(int c) {
		return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
			|| c == ')' || c == ']' || c == '}' || c == '_';
	}
	// c can be the first char of a type so the previous "[n]" is a dimension
	static bool is_type_start(int c) {
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
			|| c == '[' || c == '{' || c == '_';
	}
};

extern Lexer lex;
extern int builtin_input_fd;
extern int input_fd;
extern bool next_input_file();
extern std::nullptr_t HandleGlobalVariable(BinaryExprAST* expr);
extern void InitializeModuleAndPassManager();
extern std::unique_ptr<llvm::orc::VolvoxJIT> TheJIT;
extern llvm::Function* PrepareFunctionBody(std::unique_ptr<PrototypeAST> Proto);
extern void FinishFunction(llvm::Function* TheFunction, llvm::Value* RetVal);
extern std::nullptr_t Error(SourceLocation Loc, const char *Str, ...);
extern std::tuple<llvm::Type*, std::function<llvm::Value*(llvm::Value*)>, bool> MakeType(llvm::Type* type, bool is_signed, bool is_unknown_type);
extern volvoxc::FullType* MakeType(volvoxc::FullType* base, bool is_unknown_type);

struct global_var_shadow {
	struct global_var_shadow* next;
	void* adr;
	size_t size;
	char data[8]; // dynamically extended
};
