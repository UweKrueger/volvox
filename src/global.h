#pragma once
/*
 * Copyright © Uwe Krüger 2021, 2022
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#if defined (_WIN32)
#include "../include/volvox-15.hh"
#include <windows.h>
#include <synchapi.h>
#include <processthreadsapi.h>
#endif
#include "../lib/map.h"
#include "../lib/types.h"
#include "../lib/str.h"
#if defined(__NetBSD__)
#define alloca __builtin_alloca
#endif
#if defined(_MSC_VER)
/* The volvox run time library uses Itanium/GNU mangling which are not
 * supported by MSVC. To use those library functions inside the compiler
 * we have to provide fake "C" declarations */
extern "C" {
	union MapValue {
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
#define map_string_insert _ZN6volvox3map13string_insertEPPNS0_4NodeEPKcNS0_5ValueEiRS2_
#define map_string_tag_insert _ZN6volvox3map17string_tag_insertEPPNS0_4NodeEPKcjNS0_5ValueEiRS2_
#define map_string_get _ZN6volvox3map10string_getEPNS0_4NodeEPKc
#define map_destroy _ZN6volvox3map7destroyEPNS0_4NodeEPFvPNS0_5ValueEE
#define map_iter_up _ZN6volvox3map7iter_upEPNS0_4NodeE
#define map_iter_down _ZN6volvox3map9iter_downEPNS0_4NodeE
#define map_string_delete _ZN6volvox3map13string_deleteEPPNS0_4NodeEPKcPFvPNS0_5ValueEE
#define map_min _ZN6volvox3map3MinEPNS0_4NodeE
#define map_max _ZN6volvox3map3MaxEPNS0_4NodeE
	_DECL MapNode* map_string_new_map();
	_DECL MapNode* map_string_insert(MapNode** root_ptr, const char* key, MapValue value, int value_size, MapNode*& replace);
	_DECL MapNode* map_string_tag_insert(MapNode** root_ptr, const char* key, unsigned tag, MapValue value, int value_size, MapNode*& replace);
	_DECL MapValue* map_string_get(MapNode* root, const char* key);
	_DECL void map_destroy(MapNode* root, void (*destruct)(MapValue* ptr));
	_DECL MapNode* map_iter_up(MapNode* elem);
	_DECL MapNode* map_iter_down(MapNode* elem);
	_DECL bool map_string_delete(MapNode** root_ptr, const char* key, void (*destruct)(MapValue* ptr));
	_DECL MapNode* map_min(MapNode* node);
	_DECL MapNode* map_max(MapNode* node);
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
#define map_iter_down volvox::map::iter_down
#define map_string_delete volvox::map::string_delete
#define map_min volvox::map::Min
#define map_max volvox::map::Max
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

extern std::string IdentifierStr; // string parsed as CurTok
extern std::unique_ptr<llvm::Module> TheModule;
extern std::unique_ptr<llvm::IRBuilder<>> Builder;
extern std::unique_ptr<llvm::MDBuilder> MDBuilder;
extern std::unique_ptr<llvm::DIBuilder> DBuilder;
#ifdef LEGACY_PASS_MANAGER
extern std::unique_ptr<llvm::legacy::FunctionPassManager> TheFPM;
#endif
enum idiv_modes {
	idiv_mode_undef = 0,
	idiv_mode_floored,
	idiv_mode_c99
};
extern idiv_modes idiv_mode;
extern bool support_fp80;
extern bool needs_libm;
extern bool have_return;
extern int return_value;
extern std::unique_ptr<llvm::orc::ThreadSafeContext> TS_Context;
#define Context *TS_Context->getContext()
extern SourceLocation CurLoc;
extern bool inside_function;
extern int prompt_indent;
extern uint64_t stacksize;
extern const char* last_defined_type;
extern llvm::CodeGenOpt::Level codegenopt; // backend optimization level
#ifndef LEGACY_PASS_MANAGER
extern llvm::OptimizationLevel optimization_level; // IR optimization level
extern llvm::LoopAnalysisManager LAM;
extern llvm::FunctionAnalysisManager FAM;
extern llvm::CGSCCAnalysisManager CGAM;
extern llvm::ModuleAnalysisManager MAM;
extern llvm::PipelineTuningOptions PTO;
extern llvm::PassBuilder PB;
extern llvm::TargetMachine* TheTargetMachine;
#endif

namespace volvoxc {
	struct FullType;
}

class StructFieldType {
public:
	MapNode* node;
	StructFieldType(MapNode* _node) : node(_node) {}
	inline StructFieldType& operator++() { node = map_iter_up(node); return *this; }
	inline StructFieldType& operator--() { node = map_iter_down(node); return *this; }
	operator bool() { return !(!(node)); }
	MapValue* getRawValue() { return &node->value; }
	unsigned getIndex() {
		MapValue* mv = getRawValue();
		return *(unsigned*)((char*)mv + mv->offset);
	}
	volvoxc::FullType* getFt() {
		volvoxc::FullType* ft;
		MapValue* mv = getRawValue();
		char* adr = (char*)mv + mv->offset + 4;
		memcpy(&ft, adr, sizeof(void*));
		return ft;
	}
	const char* getKey() { return node->key.string; }
};

namespace volvoxc {

	struct FullType {
		llvm::Type* type = nullptr; // used by compiler
		unsigned type_attr = 0; // signed, atomic, shared, iso, ref, num_indices
		const char* mangled_name = nullptr; // maybe NULL for anonymous types
		llvm::DIType* ditype = nullptr;
		union {
			FullType* elem_type = nullptr; // for array or tuples
			//PrototypeAST* proto; // for functions
			std::vector<std::unique_ptr<PrototypeAST>>* Protos; // for overloaded functions
			MapNode* fields;     // for structs
		};
		void dump(int fd = 2);
		// iterate over struct fields
		StructFieldType first() { return StructFieldType(map_min(fields)); }
		StructFieldType last() { return StructFieldType(map_max(fields)); }
	};

	/* Named types can be kept in a map using the name as key.
	   anonymous types must be kept too - in a way that allows freeing
	   them when not needed anymore.
	   This can be done in a single linked list */

	struct FTListElem {
		FTListElem* next = nullptr;
		FullType ft = {0};
	};

}

extern llvm::ArrayType* MakeInterfaceArrayType(llvm::ArrayType* array_type);

inline llvm::raw_ostream& operator<<(llvm::raw_ostream& out, SourceLocation& Loc) {
	return out << Loc.File << ":" << Loc.Line << ":" << Loc.Col;
}

struct FnArg {
	std::function<llvm::Value*(llvm::Value*)> Conv = nullptr;
	llvm::Type* argtype;
	unsigned argtype_attr;
	bool arg_unknown_type;
	bool is_anonymous_list;
	bool arg_signed() { return argtype_attr & A_signed; }
};

extern llvm::raw_ostream& print_ft(llvm::raw_ostream& out, llvm::Type* type, unsigned type_attr,
                                   volvoxc::FullType* ft_elem_type = nullptr);
// there is another "FullType" printing routine in mangler.cc - use reference here to distinguish
static inline llvm::raw_ostream& operator<<(llvm::raw_ostream& out, volvoxc::FullType& ft) {
	return print_ft(out, ft.type, ft.type_attr, ft.elem_type);
}

static inline llvm::raw_ostream& operator<<(llvm::raw_ostream& out, FnArg& ft) {
	return print_ft(out, ft.argtype, ft.argtype_attr);
}

// classification of binary operator with result type calculation in mind
enum OpClass : uint8_t {
	OpInvalid = 0,
	OpNormal,
	OpAssign,
	OpModAssign,
	OpDeclAssign,
	OpComparison,
	OpShift,
	OpLogical,
	OpBitwise,
	OpExponentiation,
	OpColon,
	OpComma
};

extern llvm::Value* Volvox2CStr(llvm::Value* v);
extern void ConversionErr(SourceLocation Loc, llvm::Type* expr_type, llvm::Type* desired_type,
                          bool expr_is_signed, bool desired_is_signed, const char* reason, bool is_explicit);
static inline std::nullptr_t AutoErr(SourceLocation Loc, llvm::Type* expr_type, llvm::Type* desired_type,
                                     bool expr_is_signed, bool desired_is_signed, const char* reason) {
	if (Loc.File)
		ConversionErr(Loc, expr_type, desired_type, expr_is_signed, desired_is_signed, reason, false);
	return nullptr;
}
static inline std::nullptr_t ExplicitErr(SourceLocation Loc, llvm::Type* expr_type, llvm::Type* desired_type,
                                     bool expr_is_signed, bool desired_is_signed, const char* reason) {
	if (Loc.File)
		ConversionErr(Loc, expr_type, desired_type, expr_is_signed, desired_is_signed, reason, true);
	return nullptr;
}
extern OpClass getOpClass(const char* Op);
extern std::function<llvm::Value*(llvm::Value*)> getConv(
	llvm::Type* expr_type, llvm::Type* desired_type, SourceLocation Loc = CurLoc, bool expr_is_signed = false,
	bool desired_is_signed = false, bool is_explicit = false, bool is_unknown_type = false, bool* exact_match = nullptr);
extern std::tuple<llvm::Type*, llvm::Type*, const char*> getDesiredTypes(
	llvm::Type* res_type, llvm::Type* desired_res,
	llvm::Type* left_type, llvm::Type* right_type, OpClass opclass, bool res_min_is_signed,
	bool left_is_signed, bool right_is_signed, bool left_is_unknown_type, bool right_is_unknown_type);
extern std::tuple<llvm::Type*, unsigned, bool, OpClass, const char*> getResType(
	llvm::Type* left_type, llvm::Type* right_type, const char* Op,
	unsigned left_attr, unsigned right_attr, bool left_is_unknown_type, bool right_is_unknown_type);
extern volvoxc::FullType* getCommonType(std::vector<ExprAST*>& valid_exprs);
extern llvm::Value* NoConversion(llvm::Value* v);
extern const char* getThisExePath();
extern const char* volvox_root();
extern const char* volvox_lib();
// often used types - for faster access
extern unsigned target_bytes; // size_t, pointer size in bytes
extern unsigned target_bits; // in bits
extern llvm::Type* llvm_int_type;
extern llvm::Type* llvm_size_type;
extern llvm::Type* llvm_bool_type;
extern llvm::Type* llvm_interface_type;
extern volvoxc::FullType* void_type;
extern volvoxc::FullType* bool_type;
extern volvoxc::FullType* char_type;
extern volvoxc::FullType* size_type;
extern volvoxc::FullType* integer_type;
extern volvoxc::FullType* interface_type;
extern std::unique_ptr<FunctionAST> MainFunction;
static inline llvm::ConstantInt* getSize(uint64_t n) {
	return llvm::ConstantInt::get(llvm::IntegerType::get(Context, target_bits), n);
}
extern const char* last_shadow_saver;
extern const char* last_shadow_restorer;

extern unsigned anon_struct_nr;
extern std::vector<const char*> module_names;
extern std::map<std::string,llvm::FunctionType*> Conversions;
extern std::map<std::string,std::pair<std::string,std::string>> AutoMethods;
extern llvm::Function* getConversion(std::string& mangled_name);
extern llvm::Function* getConstructorOrDestructor(volvoxc::FullType* ft, bool destructor = false);
extern llvm::SmallString<128> MangleBase(llvm::SmallString<128> buf, const std::vector<std::string>& path,
                                         const std::string& name, const char* receiver_type_name = nullptr,
                                         unsigned flags = 0);
extern llvm::SmallString<128> Mangle(const std::vector<std::string>& path, const std::string& name,
                                     std::vector<volvoxc::FullType*>& arg_types, unsigned flags = 0);
extern std::unique_ptr<FunctionAST> ParseDefinition(unsigned& share_kind);
extern std::unique_ptr<ExprAST> GetTopLevelExpression(unsigned sym_kind);
extern std::unique_ptr<FunctionAST> ParseTopLevelExpr(std::unique_ptr<ExprAST>, bool suppress_output = false);
extern std::unique_ptr<ExprAST> ParseExpression(int terminator = 0);
extern std::unique_ptr<ExprAST> ParseStructExpr(volvoxc::FullType* ft, int terminator = 0);
extern std::unique_ptr<PrototypeAST> ParseExtern(unsigned share_kind);
extern bool spawn_bool_expr(bool (*expr)());

// classification for next token
// in general how newline is translated
// eBinOp decides if a following operator is unary or binary
enum eXpect {
	eNone,
	eBinOp, // newline translates to ;
	eComma,
	eSemi,
	ePath,
	eType
};

// variable size main vars are "malloc()ed" in jit mode. On exit these blocks would be
// orphaned - so let's keep track of then to avoid memory leaks:
class MainVars {
public:
	std::vector<char*> vars;
	MainVars() : vars() {}
	void emplace_back(char* adr) {
		vars.emplace_back(adr);
	}
	~MainVars() {
		for (auto& v: vars) {
			free(v);
		}
	}
};

extern MainVars jit_main_variables;

extern bool Expect(int tok, eXpect expect = eNone, int terminator = 0);
extern volvoxc::FullType* ParseType(
	unsigned attribs = 0, eXpect expect = eComma,
	int terminator = 0, const char* tname = nullptr,
	std::vector<std::unique_ptr<ExprAST>>* exprs = nullptr,
	llvm::StructType* existing = nullptr,
	bool is_index = false, bool resolve_ref = false);
extern llvm::Constant* getRtType(volvoxc::FullType* ft);
extern std::pair<unsigned, bool> getBitWidth(llvm::Type* type);
extern void PrepareTestFramework();
extern const char* TestFunction;
extern bool do_test;
extern bool do_repl_test;
extern void finish_constructors_and_destructor();
extern llvm::AllocaInst* CreateEntryBlockAlloca(llvm::Type* type, const llvm::Twine& VarName = "",
                                                llvm::Function* TheFunction = nullptr);
extern llvm::Function* getFunction(PrototypeAST* FI);
extern llvm::DISubroutineType *CreateFunctionType(volvoxc::FullType* RetType, std::vector<volvoxc::FullType*>& ArgTypes, llvm::DIFile *Unit);
extern llvm::ExitOnError ExitOnErr;
extern llvm::DISubprogram *SP;
extern llvm::DIFile *Unit;
extern volvoxc::FullType* theFunction_ret_ft;

struct int_val_type_t {
	llvm::Type::TypeID ID : 8; // base type
	unsigned BitWidth : 23; // #bits for int types, 0 for default
	unsigned is_signed : 1; // signed int?
};

struct FullVar {
	union {
		llvm::Value* val = nullptr;
		llvm::Type* storage_type; // for global variables
	};
	const char* mangled_name = nullptr; // only for pub globals
	llvm::Function* destructor = nullptr;
	llvm::Instruction* constructor; // to erase in auto-conversion to move
	FullVar** possible_references = nullptr; // if 'this' is accessed, constructors of those can't be elided
	unsigned n_p_r = 0;
	unsigned c_p_r = 0;
	volvoxc::FullType ft = {0};
	bool may_reference(FullVar* v) {
		for (unsigned i=0; i<n_p_r; i++)
			if (possible_references[i] == v)
				return true;
		return false;
	}
	void mark_as_referencing(FullVar* v) {
		if (!may_reference(v)) {
			if (c_p_r <= n_p_r) {
				c_p_r = c_p_r + (c_p_r >> 1) + 4;
				possible_references = (FullVar**)realloc(possible_references, c_p_r*sizeof(FullVar*));
			}
			possible_references[++n_p_r] = v;
		}
	}
	void destroy() { // we cannot call it "~FullVar()" because it must not be called automatically
		free((void*)this->mangled_name);
		free((void*)this->possible_references);
	}
};

struct FVListElem {
	FVListElem* next = nullptr;
	FullVar fv = {0};
};

extern FVListElem* anon_fullvars;
extern FVListElem** anon_fullvars_end;
extern std::vector<FullVar> expr_temps;
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
	new_node->ft.mangled_name = nullptr; // it's an anonymous type
	new_node->ft.ditype = ditype;
	new_node->ft.elem_type = elem_type;
	*anon_types_end = new_node;
	anon_types_end = &new_node->next;
	return &new_node->ft;
}

inline volvoxc::FullType* new_FullType(const volvoxc::FullType& orig, unsigned additional = 0) {
	volvoxc::FTListElem* new_node = (volvoxc::FTListElem*)malloc(sizeof(volvoxc::FTListElem) + additional * sizeof(volvoxc::FullType));
	new_node->next = nullptr;
	new_node->ft = orig;
	*anon_types_end = new_node;
	anon_types_end = &new_node->next;
	return &new_node->ft;
}

class Table {
public:
	MapNode* table;
	Table() : table(map_string_new_map()) {}
	Table(MapNode* t) : table(t) {}
	// ~Table() { map_destroy(table, nullptr); }
	Table first() { return map_min(table); }
	Table last() { return map_max(table); }
	Table& operator++() { table = map_iter_up(table); return *this; }
	Table& operator--() { table = map_iter_down(table); return *this; }
	// iteration is started from first()/last() and finished when iterator becomes 'false':
	operator bool() { return !(!(table)); }
	const char* getKey() { return table->key.string; }
	MapValue* getValue() { return &table->value; }
	MapNode* getNode() { return table; }
	void clear(void (*destruct)(MapValue* ptr)) {
		map_destroy(table, destruct);
		table = map_string_new_map();
	}
};

class TypeTable : public Table {
public:
	TypeTable() = default;
	~TypeTable() { map_destroy(table, nullptr); }
	MapNode* add(const char* name, volvoxc::FullType* ft, MapNode*& target) {
		bool is_int = ft->type && ft->type->isIntegerTy();
		if ((ft->type_attr & A_signed) && !is_int) {
			errs() << "non-int type '" << name << "' aka " << *ft->type << " cannot be signed\n";
			return 0;
		}
		MapValue val = {
			.src_ptr = ft
		};
		MapNode* new_node = map_string_insert(&table, name, val, sizeof(volvoxc::FullType), target);
		if (target) {
			return new_node; // actually existing node
		}
		union {
			int_val_type_t int_type;
			VOLVOX_gen_val_type_t gen_type;
			unsigned key;
		};
		if (ft->type) {
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
		}
		return new_node;
	}
	// method for adding built-in types - no mangling is used
	MapNode* add(const char* name, llvm::Type* type, llvm::DIType* ditype, unsigned type_attr = 0, MapNode* fields = nullptr) {
		const char* existing_name = get_name(type);
		// keep only one "canonical name" in FullType - the first one used for this LLVM-type
		// no "strdup()" is necessary since this is always called with literal constant names
		const char* canonical_name = existing_name ? existing_name : name;
		volvoxc::FullType ft = {
			.type = type,
			.type_attr = type_attr,
			.mangled_name = canonical_name,
			.ditype = ditype,
			.fields = fields
		};
		MapNode* target = nullptr;
		return add(name, &ft, target);
		if (target) {
			errs() << "internal error - type '" << name << "' already exists\n";
			abort();
		}
	}
	llvm::Type* get(const char* name) {
		MapValue* val = map_string_get(table, name);
		return ((volvoxc::FullType*)((char*)val + val->offset))->type;
	}
	bool is_signed(const char* name) {
		MapValue* val = map_string_get(table, name);
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
		MapValue* val = map_string_get(table, name);
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
		if (it == key32_table.end())
			return nullptr;
		bool is_signed = (int_type.ID == llvm::Type::IntegerTyID && int_type.is_signed);
		return new_FullType(it->second, is_signed ? A_signed : 0);
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
protected:
	std::map<unsigned, llvm::Type*> key32_table;
	std::map<llvm::Type*, std::pair<const char*, llvm::DIType*>> typeptr_table;
};

extern MapNode* keyword_toks; // all language keywords like 'if', 'else', 'fn', ...
extern void init_token_map();

extern void destroy_FV(MapValue* mapval);
extern llvm::Function* getDestructor(volvoxc::FullType* ft, bool is_created = false, bool is_constructor = false);

class VarTable : public Table {
public:
	VarTable() = default;
	VarTable(VarTable&& o) { table = o.table; o.table = nullptr; }
	~VarTable() { map_destroy(table, destroy_FV); }
	VarTable& operator=(VarTable&& o) { table = o.table; o.table = nullptr; return *this; }
	void clear() {
		map_destroy(table, destroy_FV);
		table = map_string_new_map();
	}
	FullVar* insert(const char* key, const FullVar& value) {
		MapValue mv = { .src_ptr = const_cast<FullVar*>(&value) };
		MapNode* target = nullptr;
		MapNode* res = map_string_insert(&table, key, mv, sizeof(FullVar), target);
		if (target)
			return nullptr;
		auto fv = (FullVar*)((char*)&res->value + res->value.offset);
		if (!(fv->ft.type_attr & A_ref) && (fv->ft.type_attr & A_destructor))
			fv->destructor = getDestructor(&fv->ft);
		return fv;
	}
	FullVar* operator[](const char* key) {
		MapValue* val = map_string_get(table, key);
		return val ? (FullVar*)((char*)val + val->offset) : nullptr;
	}
	bool erase(const char* name) {
		return map_string_delete(&table, name, destroy_FV);
	}
};

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
extern std::vector<VarTable> locals_table; // including function arguments
extern std::vector<std::vector<std::string>> captured_variables;
extern unsigned condnesting;
extern VarTable* IfWhileVarTable;
extern llvm::Value* ret_ptr; // for sret
extern std::vector<std::unique_ptr<ExprAST>> GlobalExprList;

extern void InsertArrayConDestructor(
	llvm::Type* elem_type, volvoxc::FullType* array_elem_type, llvm::Value* val,
	llvm::Instruction* before = nullptr, bool is_constructor = false);
extern void InsertDestructors(VarTable& t, llvm::Value* retp);
extern void InsertDestructors(llvm::Value* retp);
extern void InsertDestructors(std::vector<FullVar>& t);
extern void InsertStringDestructor(llvm::Value* v, llvm::Instruction* before = nullptr);
extern void InsertMapDestructor(llvm::Value* v, llvm::Instruction* before = nullptr);

inline static void InsertArrayDestructor(FullVar* fv, llvm::Value* val, llvm::Instruction* before) {
	InsertArrayConDestructor(fv->ft.type, fv->ft.elem_type, val, before);
}

inline static void InsertSingleDestructor(FullVar* fv, llvm::Value* val, llvm::Instruction* before = nullptr) {
	if (fv->destructor) {
		llvm::BasicBlock* oldBB;
		if (before) {
			oldBB = Builder->GetInsertBlock();
			Builder->SetInsertPoint(before);
		}
		auto FT = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), { fv->ft.type->getPointerTo() }, false);
		Builder->CreateCall(FT, fv->destructor, val);
		if (before)
			Builder->SetInsertPoint(oldBB);
	} else if (fv->ft.type->isPointerTy()) {
		llvm::Value* v = (fv->ft.type_attr & A_rvalue) ? val : Builder->CreateLoad(llvm::Type::getInt8PtrTy(Context), val);
		if (fv->ft.type_attr & A_string) {
			InsertStringDestructor(v, before);
		} else if (fv->ft.type_attr & A_map) {
			InsertMapDestructor(v, before);
		}
	}
}

inline static void InsertDestructor(FullVar* fv, llvm::Instruction* before = nullptr) {
	llvm::Value* V;
	if ((fv->ft.type_attr & A_mainvar) && comp_mode == comp_jit && !do_test || (fv->ft.type_attr & A_global)) { // global variable
		if (!fv->mangled_name) {
			errs() << "Global Destructors: no mangled name\n";
			return;
		}
		V = TheModule->getGlobalVariable(fv->mangled_name, true);
		if (!V)
			V = new llvm::GlobalVariable(*TheModule, fv->storage_type,
			                             false, llvm::GlobalValue::ExternalLinkage,
			                             nullptr, fv->mangled_name, nullptr,
			                             (fv->ft.type_attr & A_global) ?
			                             llvm::GlobalVariable::GeneralDynamicTLSModel :
			                             llvm::GlobalVariable::NotThreadLocal,
			                             0, true);
	} else {
		V = fv->val;
	}
	if (llvm::isa<llvm::ArrayType>(fv->ft.type))
		InsertArrayDestructor(fv, V, before);
	else
		InsertSingleDestructor(fv, V, before);
}

inline static llvm::Value* CheckTailCall(llvm::Value* V) {
	if (auto C = llvm::dyn_cast<llvm::CallInst>(V))
		C->setTailCall();
	return V;
}

extern llvm::MaybeAlign getAlignment(size_t elem_size);
extern llvm::MaybeAlign getAlignment(llvm::Value* size);
extern llvm::Value* StoreValue(llvm::Value* val, volvoxc::FullType* ft,
                               llvm::Type* expected_type = nullptr, const llvm::Twine &Name = "");
extern llvm::Value* getInterfaceArrayOrStoreValue(llvm::Value* val, llvm::ArrayType* array_type,
                                                  llvm::ArrayType* expected_array_type = nullptr,
                                                  bool do_store = false, const llvm::Twine &Name = "");
extern llvm::Value* expandArrayInitializer(llvm::Value* initializer, llvm::ArrayType* ini_array_type,
                                           llvm::ArrayType* array_type);
extern llvm::Type* getArrayDims(llvm::Value* val, llvm::ArrayType* array_type,
                                std::vector<llvm::Value*>& Dims, std::vector<llvm::Value*>& returnDims,
                                llvm::ArrayType* expected_array_type = nullptr);

inline llvm::Value* getInterfaceArrayValue(llvm::Value* val, llvm::ArrayType* array_type,
                                           llvm::ArrayType* expected_array_type = nullptr) {
	return getInterfaceArrayOrStoreValue(val, array_type, expected_array_type, false);
}
extern llvm::Value* createStringConst(const char* str, const llvm::Twine &Name = "");
extern llvm::Value* createStringVal(const char* str, const llvm::Twine &Name = "");

/// PrototypeAST - This class represents the "prototype" for a function,
/// which captures its name, and its argument names (thus implicitly the number
/// of arguments the function takes), as well as if it is an operator.
class PrototypeAST {

public:
	std::vector<std::string> Args;
	std::vector<volvoxc::FullType*> ArgTypes = {};
	std::vector<llvm::Type*> LLVMArgTypes = {}; // to get LLVM function type
	std::vector<llvm::AttributeSet> ArgAttrs = {};
	std::vector<SourceLocation> ArgPos;
	volvoxc::FullType* RetType = nullptr;
	SourceLocation retLoc;
	llvm::FunctionType* FT = nullptr;
	llvm::Constant* const_result = nullptr;
	bool IsVarArgs = false;
	bool IsOperator = false;
	bool IsStructRet = false; // 1st arg is pointer to allocated mem to return the struct using call by reference
	unsigned visibility = 0;
	llvm::GlobalValue::LinkageTypes link_type;
	int Line;
	std::string Name;
	PrototypeAST(const PrototypeAST& proto) = default;
	PrototypeAST(SourceLocation Loc, const std::string &Name,
	             std::vector<std::string> Args, unsigned visibility = 0, SourceLocation retLoc = CurLoc,
	             bool IsOperator = false, volvoxc::FullType* RetType_ = nullptr,
	             std::vector<volvoxc::FullType*> ArgTypes = {},
	             std::vector<SourceLocation> _ArgPos = {}, bool IsVarArgs = false);
	llvm::Function *codegen();
	const std::string &getName() const { return Name; }

	bool isUnaryOp() const { return IsOperator && Args.size() == 1; }
	bool isBinaryOp() const { return IsOperator && Args.size() == 2; }

	char getOperatorName() const {
		assert(isUnaryOp() || isBinaryOp());
		return Name[Name.size() - 1];
	}

	int getLine() const { return Line; }
};

/* Prototypes of "normal" functions are stored as vectors in maps with mangled names as keys.
 * Anonymous functions and function references cannot use these so we maintain a separate list
 */
struct ProtoListElem {
	ProtoListElem* next = nullptr;
	std::vector<std::unique_ptr<PrototypeAST>> protos;
	ProtoListElem(std::unique_ptr<PrototypeAST> proto) {
		protos.emplace_back(std::move(proto));
	}
	~ProtoListElem() = default;
};

extern ProtoListElem* anon_protos;
extern ProtoListElem** anon_protos_end;

inline std::vector<std::unique_ptr<PrototypeAST>>* new_AnonProto(PrototypeAST* proto, SourceLocation Loc) {
	ProtoListElem* new_node = new ProtoListElem(std::make_unique<PrototypeAST>(*proto));
	new_node->protos[0]->retLoc = Loc;
	new_node->next = nullptr;
	*anon_protos_end = new_node;
	anon_protos_end = &new_node->next;
	return &new_node->protos;
};

inline std::vector<std::unique_ptr<PrototypeAST>>* new_AnonProto(std::unique_ptr<PrototypeAST> proto, SourceLocation Loc) {
	ProtoListElem* new_node = new ProtoListElem(std::move(proto));
	new_node->protos[0]->retLoc = Loc;
	new_node->next = nullptr;
	*anon_protos_end = new_node;
	anon_protos_end = &new_node->next;
	return &new_node->protos;
};

extern std::vector<std::unique_ptr<PrototypeAST>>* int_int_proto;
extern llvm::SmallString<16> createAnonFnName();

enum SymbolKind : uint8_t {
	SymbolType,
	SymbolFunction,
	SymbolVar,
	ModulePrefix
};

// representation of imported symbols
class SymbolRef {
	union {
		volvoxc::FullType* full_type = nullptr;
		FullVar* full_var;
		std::vector<std::unique_ptr<PrototypeAST>>* protos;
		std::nullptr_t module_prefix;
	};
	SymbolKind kind = (SymbolKind)0;
public:
	SymbolRef(volvoxc::FullType* _full_type) : full_type(_full_type), kind(SymbolType) {}
	SymbolRef(FullVar* _full_var) : full_var(_full_var), kind(SymbolVar) {}
	SymbolRef(std::vector<std::unique_ptr<PrototypeAST>>* _protos) : protos(_protos), kind(SymbolFunction) {}
	SymbolRef() : module_prefix(nullptr), kind(ModulePrefix) {}
	volvoxc::FullType* getFullType() { return (kind == SymbolType) ? full_type : nullptr; }
	FullVar* getFullVar() { return (kind == SymbolVar) ? full_var : nullptr; }
	std::vector<std::unique_ptr<PrototypeAST>>* getProtos() { return (kind == SymbolFunction) ? protos : nullptr; }
	bool isPrefix() { return (kind == ModulePrefix); }
};

class Module {
public:
	Module(std::vector<std::string> _import_path) :
		import_path(std::move(_import_path)) {}
	std::vector<std::string> import_path;
	TypeTable type_table;
	std::map<std::string, std::vector<std::unique_ptr<PrototypeAST>>> FunctionProtos;
	VarTable globals_table;
	// imported symbols are kept separate to be not re-exported in nested imports
	std::map<std::pair<std::string, std::string>, SymbolRef> ImportedSymbols;
};

extern std::map<std::string, Module> Modules;
extern std::map<std::pair<std::string,std::string>, std::vector<std::unique_ptr<PrototypeAST>>> MethodProtos;
extern std::map<std::string,volvoxc::FullType*> struct_mangled_ft;
extern unsigned dump_IR;
extern bool dump_opt;
extern bool dump_raw;

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
	Token(const Token& old) = delete;
	Token(Token&& old) : kind(old.kind), gen_type(old.gen_type), Val(old.Val), is_unknown_type(old.is_unknown_type) {
		old.Val.Ptr = nullptr;
	}
	Token& operator=(const Token& old) = delete;
	Token& operator=(Token&& old) {
		kind = old.kind;
		gen_type = old.gen_type;
		Val = old.Val;
		is_unknown_type = old.is_unknown_type;
		old.Val.Ptr = nullptr;
		return *this;
	}
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
	~Token() {
		if (kind == tok_str_lit)
			free((void*)Val.Ptr);
	}
	std::string str() const;
};

llvm::raw_ostream& operator<<(llvm::raw_ostream& out, TokenKind kind);
llvm::raw_ostream& operator<<(llvm::raw_ostream& out, Token& tok);
llvm::raw_ostream& operator<<(llvm::raw_ostream& out, eXpect expect);

extern Token CurTok;
extern bool parseOk;
extern Token& getNextToken(eXpect expect = eNone, int terminator = 0);
extern Token& purgeLine();

struct SourceLocState {
	SourceLocation Loc = {0};
	Module* module = nullptr;
	ssize_t linelen = 0;
	size_t bufsize = 0;
	char* linebuf = nullptr;
	int input_fd = 0;
	bool use_readline = false;
	std::string as = "";
	std::map<std::string, SourceLocation> fromlist = {};
	SourceLocState() = default;
	SourceLocState(SourceLocState* old):
		Loc(old->Loc), module(std::move(old->module)), linelen(old->linelen), bufsize(old->bufsize), linebuf(old->linebuf),
		input_fd(old->input_fd), use_readline(old->use_readline), as(std::move(old->as)), fromlist(std::move(old->fromlist))
		{
			old->linebuf = nullptr;
			old->bufsize = 0;
			old->as = "";
			old->fromlist = {};
			old->input_fd = -1;
		}
	SourceLocState(SourceLocation Loc, ssize_t linelen, size_t bufsize, char* linebuf, int* _input_fd = nullptr, bool use_readline = false) :
		Loc(Loc), linelen(linelen), bufsize(bufsize), linebuf(linebuf), input_fd(_input_fd ? *_input_fd : -1), use_readline(use_readline)
		{
			if (_input_fd)
				*_input_fd = -1; // to prevent 'next_input_file()' from calling 'close()'
		}
};

class Lexer : public SourceLocState {
public:
	std::vector<SourceLocState> source_stack = {};
	eXpect Expected; // only used for error messages
	Lexer() = default;
	Lexer(int* _inputfd, const char* _input_file_name, size_t _bufsize = 100)
		: SourceLocState(SourceLocation{ _input_file_name, 0, 0 }, 0, _bufsize,
		                 _bufsize ? nullptr : (char*)malloc(_bufsize), _inputfd), CurChar(' ')
		{
			std::string patterntail = "builtin.vx";
			std::vector<std::string> _import_path = {};
			auto new_module = Modules.try_emplace(patterntail, std::move(_import_path));
			if (new_module.second) {
				module = &new_module.first->second;
			} else {
				errs() << "cannot initialize main module\n";
				abort();
			}
		}
	~Lexer() {
		free(linebuf);
	}
	int advance();
	Token gettok(eXpect expect = eNone, int terminator = 0);
	Token purge_line();
	char peek();
	char peek_strict();
	char look_back_strict();
	bool next_input_file();
	bool push_state(std::vector<std::string> _import_path, std::string as, std::map<std::string, SourceLocation> fromlist);
	void pop_state();
	void import_from_module(Module* import_module);
	llvm::DIType* get_diType(llvm::Type* type) { return module->type_table.get_diType(type); }
	llvm::DIType* get_diType(llvm::Type* type, bool is_signed) { return module->type_table.get_diType(type, is_signed); }
	MapNode* add_type(const char* name, volvoxc::FullType* ft, MapNode*& target) { return module->type_table.add(name, ft, target); }
	MapNode* add_type(const char* name, llvm::Type* type, llvm::DIType* ditype, unsigned type_attr = 0, MapNode* fields = nullptr)
		{ return module->type_table.add(name, type, ditype, type_attr, fields); }
	/* the 'lookup' methods must search in both the current namespace
	   and the 'builtin' namespace aka 'source_stack.front()' */
	llvm::Type* get_type(const char* name) {
		auto t = module->type_table.get(name);
		if (!t && source_stack.size())
			t = source_stack.front().module->type_table.get(name);
		return t;
	}
	bool is_signed_type(const char* name) {
		if (source_stack.size())
			return source_stack.front().module->type_table.is_signed(name);
		else
			return module->type_table.is_signed(name);
	}
	volvoxc::FullType* get_full_type(const std::string& prefix, const std::string& unmangledName) {
		auto im = module->ImportedSymbols.find({ prefix, unmangledName });
		if (im != module->ImportedSymbols.end())
			return im->second.getFullType();
		return nullptr;
	}
	volvoxc::FullType* get_full_type(const char* name) {
		auto ft = module->type_table.get_full(name);
		if (!ft) {
			// try to look up in from-imported symbols
			ft = get_full_type("", name);
			if (!ft && source_stack.size())
				ft = source_stack.front().module->type_table.get_full(name);
		}
		return ft;
	}
	// reverse lookup, i.e. type -> name is only supported for built-in types like 'int', 'real', 'f32'
	// they are looked up only in the lowest module of source_stack
	volvoxc::FullType* get_full_type(unsigned _key) {
		auto ft = module->type_table.get_full(_key);
		if (!ft && source_stack.size())
			ft = source_stack.front().module->type_table.get_full(_key);
		return ft;
	}
	const char* get_type_name(llvm::Type* type) {
		auto name = module->type_table.get_name(type);
		if (!name && source_stack.size())
			name = source_stack.front().module->type_table.get_name(type);
		return name;
	}
	const char* get_type_name(llvm::Type* type, bool is_signed) {
		auto name = module->type_table.get_name(type, is_signed);
		if (!name && source_stack.size())
			name = source_stack.front().module->type_table.get_name(type, is_signed);
		return name;
	}
	std::vector<std::unique_ptr<PrototypeAST>>* findProtos(const std::string& prefix, const std::string& unmangledName) {
		auto im = module->ImportedSymbols.find({ prefix, unmangledName });
		if (im != module->ImportedSymbols.end())
			return im->second.getProtos();
		return nullptr;
	}
	std::vector<std::unique_ptr<PrototypeAST>>* findProtos(const std::string& unmangledName) {
		auto FI = module->FunctionProtos.find(unmangledName);
		if (FI == module->FunctionProtos.end() || !FI->second.size()) {
			if (source_stack.size()) {
				FI = source_stack.front().module->FunctionProtos.find(unmangledName);
				if (FI == source_stack.front().module->FunctionProtos.end() || !FI->second.size())
					return findProtos("", unmangledName);
			} else {
				return nullptr;
			}
		}
		return &FI->second;
	}
	int CurChar = '\0';
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

inline FullVar* lookup_var(const char* prefix, const char* unmangledName) {
	auto im = lex.module->ImportedSymbols.find({ prefix, unmangledName });
	if (im != lex.module->ImportedSymbols.end())
		return im->second.getFullVar();
	return nullptr;
}

// look up var and return if it's global
inline FullVar* lookup_var(const char* Name) {
	FullVar* full_var;
	for (int i = locals_table.size() - 1; i >= 0; i--) {
		full_var = locals_table[i][Name];
		if (full_var) {
			if (i < locals_table.size()-1 && !captured_variables.empty()) {
				locals_table.back().insert(Name, *full_var);
				captured_variables.back().emplace_back(Name);
			}
			return full_var;
		}
	}
	// it's no function local var - maybe a global one from this module
	full_var = lex.module->globals_table[Name];
	// or from an imported module
	if (!full_var || !(full_var->ft.type_attr & A_global) && inside_function)
		full_var = lookup_var("", Name);
	if (full_var && !(full_var->ft.type_attr & A_global) && inside_function)
		full_var = nullptr;
	if (!full_var && lex.source_stack.size())
		// search in "builtin" as last resort - lowest in source_stack
		full_var = lex.source_stack.front().module->globals_table[Name];
	if (full_var && !(full_var->ft.type_attr & A_global) && inside_function)
		full_var = nullptr;
	return full_var;
}

// avoid name conflics by collecting all syms - true: functinon, false: variable
extern std::map<std::string,bool> all_global_symbols;

enum ConversionKind : uint8_t {
	ConvImplicit = 0,
	ConvSigned,
	ConvUnsigned
};

/// ExprAST - Base class for all expression nodes.
class ExprAST {
public:
	SourceLocation Loc;
	volvoxc::FullType* ft = nullptr;
	llvm::Type* desired_type = nullptr;
	ConversionKind conv_kind = ConvImplicit;
	bool is_unknown_type = false;

	// construct from type and attributes
	ExprAST(const ExprAST& s) = default;
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
		: ft(lex.get_full_type(key)), Loc(Loc), is_unknown_type(is_unknown_type)
		{
			ft->type_attr |= add_attr;
		}
	ExprAST(volvoxc::FullType* full_type, SourceLocation Loc = CurLoc, bool is_unknown_type = false)
		: ft(full_type ? full_type : new_FullType(nullptr, 0)), Loc(Loc), is_unknown_type(is_unknown_type) {}
	virtual ~ExprAST() {}
	// generate an llvm::Value* for this expression. 'target' may be:
	// - a pointer value: in this case the generated value is directly stored and "void" is returned
	// - (void*)0: the generated value is returned. Since it is not stored (e.g. to a variable) it is assumed that it's
	//      an intermediate value (e.g. '(b + c)' in 'x = a * (b + c)' and a potential destructor call for the
	//      value is registred
	// - (void*)(-1): like '(void*)0' but no destructor call is registred. This is needed to create compile time const
	//      initializers for use with '::='
	virtual llvm::Value *codegen_raw(llvm::Value* target = nullptr) = 0; // target used by sret
	virtual bool needs_target() { return false; } // e.g. struct return in CallExpr
	llvm::Value* codegen() {
		auto rawV = codegen_raw();
		if (!rawV)
			return nullptr;
		if (desired_type && rawV && rawV->getType() != desired_type && !rawV->getType()->isVoidTy()) {
			auto postConv = getConv(rawV->getType(), desired_type, Loc, ft->type_attr & A_signed,
			                        conv_kind == ConvImplicit ? ft->type_attr & A_signed : conv_kind == ConvSigned,
			                        conv_kind != ConvImplicit, is_unknown_type);
			if (postConv)
				return postConv(rawV);
			auto raw_array_type = llvm::dyn_cast<llvm::ArrayType>(rawV->getType());
			auto desired_array_type = llvm::dyn_cast<llvm::ArrayType>(desired_type);
			if (!raw_array_type || !desired_array_type) {
				errs() << Loc << "cannot automatically convert " << *rawV->getType() << " to " << *desired_type << '\n';
				return nullptr;
			}
		}
		return rawV;
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
	llvm::DICompileUnit *TheCU = nullptr;
	std::vector<llvm::DIScope *> LexicalBlocks;

	void emitLocation(ExprAST *AST);
};

extern DebugInfo KSDbgInfo;
extern int builtin_input_fd;
extern std::nullptr_t HandleGlobalVariable(std::unique_ptr<BinaryExprAST> expr, unsigned sym_kind = 0);
extern bool DeclareGlobalConst(std::unique_ptr<ExprAST> expr, unsigned sym_kind);
extern void InitializeModuleAndPassManager();
extern void finishFunctionOrModule(llvm::Function* F = nullptr, unsigned dumpLevel = 1,
                                   bool finishModule = true, bool newModule = true);
extern std::unique_ptr<llvm::orc::VolvoxJIT> TheJIT;
extern llvm::Function* PrepareFunctionBody(std::unique_ptr<PrototypeAST> Proto);
extern void FinishFunction(llvm::Function* TheFunction, llvm::Value* RetVal);
extern std::nullptr_t Error(SourceLocation Loc, const char *Str, ...);
extern std::tuple<llvm::Type*, bool> MakeType(llvm::Type* type, bool is_signed, bool is_unknown_type);
extern volvoxc::FullType* MakeType(volvoxc::FullType* base, bool is_unknown_type);
extern std::vector<std::vector<const char*>> source_files;
extern std::vector<int> source_index;
extern std::vector<const char*> SourceFileNames;
extern bool jit_repl;
extern int builtin_input_fd;
extern void CallGlobalDestructorsJIT();
