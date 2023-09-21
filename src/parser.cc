/*
 * Copyright © Uwe Krüger 2021, 2022, 2023
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"

//===----------------------------------------------------------------------===//
// Parser
//===----------------------------------------------------------------------===//

/// CurTok/getNextToken - Provide a simple token buffer.  CurTok is the current
/// token the parser is looking at.  getNextToken reads another token from the
/// lexer and updates CurTok with its results.
Lexer lex;
Token CurTok;
std::vector<const char*> module_names = {};
std::map<std::string,llvm::FunctionType*> Conversions;
std::map<std::string,std::pair<std::string,std::string>> AutoMethods;

// methods table - keys: { mangled_type_name, method_name }
std::map<std::pair<std::string,std::string>, std::vector<std::unique_ptr<PrototypeAST>>> MethodProtos;
std::vector<std::unique_ptr<PrototypeAST>>* int_int_proto = nullptr;
FVListElem* anon_fullvars = nullptr;
FVListElem** anon_fullvars_end = &anon_fullvars;
ProtoListElem* anon_protos = nullptr;
ProtoListElem** anon_protos_end = &anon_protos;
bool local_var_may_shadow_func_and_mod = true;

extern llvm::ExitOnError ExitOnErr;
bool parseOk = true;
bool inside_branch = false;

Token& getNextToken(eXpect expect, int terminator) {
	CurTok = lex.gettok(expect, terminator);
	if (CurTok.kind == tok_error)
		parseOk = false;
	return CurTok;
}

Token& purgeLine() {
	parseOk = true;
	CurTok = lex.purge_line();
	return CurTok;
}

/// GetTokPrecedence - Get the precedence of the pending binary operator token.
static inline int GetTokPrecedence() {
	return (CurTok.kind < 0 && CurTok.kind > tok_last_op) ? (-CurTok.kind) << 8 : -256;
}

// same as above but take into account that some operators are right binding
static inline int NextTokPrecedence() {
	int prec = GetTokPrecedence();
	// assignments and the invisible operator are right binding
	if (CurTok.kind == tok_assign || CurTok.kind == tok_invisible)
		prec++;
	return prec;
}

bool Expect(int tok, eXpect expect, int terminator) {
	bool res = CurTok.kind == tok;
	if (res) {
		getNextToken(expect, terminator);
	} else {
		errs() << CurLoc << ": unexpected " << CurTok << " - expected " << TokenKind(tok) << '\n';
	}
	return res;
}

static void Eat(int tok, eXpect expect = eNone, int terminator = 0) {
	if (CurTok.kind == tok) {
		getNextToken(expect, terminator);
	}
}

static std::vector<std::unique_ptr<ExprAST>> SplitExprList(std::unique_ptr<ExprAST> Arg) {
	std::vector<std::unique_ptr<ExprAST>> Args;
	// If we have an EmptyExprAST just return the empty list
	if (auto empty_expr = dynamic_cast<EmptyExprAST*>(Arg.get())) {
		return Args;
	}
	// The arguments are parsed as a tree of binary expressions (Op=',') where
	// all objects are in the leaves.
	// Due to operator precedence rules the tree is stricly left-heavy and can be
	// processed right to left without the need for recursions. We just have to iterate
	// through the binary nodes and front-push each right leave (RHS) to the Args list.
	while (auto bin_expr = dynamic_cast<BinaryExprAST*>(Arg.get())) {
		if (bin_expr->Op[0] == ',') {
			Args.insert(Args.begin(), std::move(bin_expr->RHS));
			Arg = std::move(bin_expr->LHS);
		} else {
			break;
		}
	}
	Args.insert(Args.begin(), std::move(Arg));
	return Args;
}

static std::pair<std::string,volvoxc::FullType*> ParseTypedIdent(int terminator, bool resolve_ref);

/* parse a type - this function may be called by ParseAggregateExpr()
   for the initial part of "type{...}" literals
   it "type" starts with '[' there is an ambiguity
   - "[n] x" - vector expression with 1 element "n" multiplied with "x"
     in general this could be "[n, m, ...] x"
   - "[n]x" - array type for "n" elements of type "x"
     this could in general be "[n][m]x"

   in both cases n, m, ... are returned in 'exprs' but - in the first
   case ParseType() returns nullptr

   when no literal is parsed ParseType with exprs = nullptr and only
   the second case is considered

   'existing' allows passing an opaque struct type that is completed

   when resolve_ref is true a pointer type is created if preceded with '&'
   (this is needed for struct declarations) - otherwise A_ref is set
 */
volvoxc::FullType* ParseType(unsigned attribs, eXpect expect, int terminator,
                             const char* tname,
                             std::vector<std::unique_ptr<ExprAST>>* exprs,
                             llvm::StructType* existing,
                             bool is_index, bool resolve_ref) {
	std::vector<uint64_t> lens = {};
	bool is_packed = false;
	while (CurTok.kind != tok_identifier) {
		if (CurTok.kind == tok_packed) {
			if (is_packed) {
				errs() << CurLoc << ": superfluous 'packed'\n";
				return nullptr;
			}
			is_packed = true;
			getNextToken(eType);
		}
		if (CurTok.kind == tok_union) {
			if (attribs & A_union) {
				errs() << CurLoc << ": superfluous 'union'\n";
				return nullptr;
			}
			attribs |= A_union;
			getNextToken(eType);
		}
		if (is_packed && (attribs & A_union)) {
			errs() << CurLoc << ": 'packed' and 'union' are mutually exclusive\n";
			return nullptr;
		}
		switch ((int)CurTok.kind) {
		case '[': {
			do {
				getNextToken();
				if (CurTok.kind == ']') {
					getNextToken(eType);
					if (exprs)
						exprs->push_back(nullptr);
					else
						lens.push_back(0);
				} else {
					if (auto e = ParseExpression(']')) {
						if (exprs) {
							if (!exprs->size() && (is_index || !Lexer::is_type_start(lex.peek_strict()))) {
								// this is a vector - just return elements
								if (!Expect(']', expect, terminator)) {
									exprs->clear();
									return nullptr;
								}
								*exprs = SplitExprList(std::move(e));
								return nullptr;
							} else {
								exprs->push_back(std::move(e));
							}
						} else {
							// we are parsing a pure type - like "[5][][7]f64"
							// in this case the dimensions ("5", "7") must be compile time consts
							// and we can calculate their values now
							auto VLen = e->codegen();
							if (!VLen) {
								errs() << "cannot generate code for dimension\n";
								return nullptr;
							}
							if (auto Len = llvm::dyn_cast<llvm::ConstantInt>(VLen)) {
								int64_t len = Len->getSExtValue();
								if (len <= 0) {
									errs() << "dimension must be a positive int (not " << len << ")\n";
									return nullptr;
								} else {
									lens.push_back((uint64_t)len);
								}
							} else {
								errs() << "dimension must be constant int\n";
								return nullptr;
							}
						}
						if (!Expect(']', eType)) {
							return nullptr;
						}
					} else {
						errs() << "cannot parse dimension expression\n";
						return nullptr;
					}
				}
			} while (CurTok.kind == '[');
			auto elem_type = ParseType(0, expect, terminator);
			if (!elem_type) {
				errs() << CurLoc << ": type specifier expected\n";
				if (exprs)
					*exprs = std::vector<std::unique_ptr<ExprAST>>{};
				return nullptr;
			}
			llvm::Type* array_type = elem_type->type;
			if (int i = lens.size())
				do
					array_type = llvm::ArrayType::get(array_type, lens[--i]);
				while (i > 0);
			else
				for (int i = exprs->size(); i > 0; i--)
					array_type = llvm::ArrayType::get(array_type, 0);
			return new_FullType(array_type, attribs, nullptr, elem_type);
		}
			break;
		case '{': {
			// struct type
			getNextToken();
			std::vector<std::string> FieldNames;
			std::vector<volvoxc::FullType*> FieldTypes;
			std::vector<llvm::Type*> LLVMFieldTypes;
			for (;;) {
				auto [name, ft] = ParseTypedIdent('}', true);
				if (!ft)
					return nullptr;
				FieldNames.push_back(name);
				auto type = (volvoxc::FullType*)((uintptr_t)(ft) & ~1ULL);
				FieldTypes.push_back(type);
				LLVMFieldTypes.push_back(type->type);
				if (CurTok.kind != '}')
					if (!Expect(','))
						return nullptr;
				if (CurTok.kind == '}')
					break;
			}
			getNextToken(eSemi);
			if (attribs & A_union) {
				unsigned idx = LLVMFieldTypes.size();
				if (!idx) {
					errs() << CurLoc << "union must have elements\n";
					return nullptr;
				}
				idx--;
				size_t sz = TheModule->getDataLayout().getTypeAllocSize(LLVMFieldTypes[idx]);
				while (idx) {
					// backwards loop - move largest element to index 0 and pop upper elements
					size_t sz2 = TheModule->getDataLayout().getTypeAllocSize(LLVMFieldTypes[idx-1]);
					if (sz > sz2)
						LLVMFieldTypes[idx-1] = LLVMFieldTypes[idx];
					else
						sz = sz2;
					LLVMFieldTypes.pop_back();
					idx--;
				}
			}
			llvm::Type* struct_type = existing;
			if (existing)
				existing->setBody(LLVMFieldTypes, is_packed);
			else
				if (tname)
					struct_type = llvm::StructType::create(Context, LLVMFieldTypes, tname, is_packed);
				else
					struct_type = llvm::StructType::get(Context, LLVMFieldTypes, is_packed);
			MapNode* fields = map_string_new_map();
			for (int i=0; i<FieldNames.size(); i++) {
				MapNode* replace = nullptr;
				MapNode* new_node = map_string_tag_insert(&fields, FieldNames[i].c_str(), i, MapValue{ .src_ptr = &FieldTypes[i] }, sizeof(void*), replace);
				if (replace) {
					errs() << "Duplicate field name '" << FieldNames[i] << "' in struct declaration\n";
					return nullptr;
				}
			}
				
			return new_FullType(struct_type, attribs, nullptr /*DIType*/, (volvoxc::FullType*)fields);
		}
			break;
		case tok_map:
		case tok_set: {
			bool is_set = CurTok.kind == tok_set;
			getNextToken();
			if (!Expect('[', eType))
				return nullptr;
			auto KeyLoc = CurLoc;
			auto key_ft = ParseType(0, eNone, ']');
			if (!key_ft) {
				errs() << KeyLoc << ": type (of map key) expected\n";
				return nullptr;
			}
			if (!Expect(']', eType))
				return nullptr;
			auto val_ft = ParseType(0, expect, terminator);
			if (!val_ft) {
				errs() << KeyLoc << ": type (of map value) expected\n";
				return nullptr;
			}
			auto ftpair = new_FullType(*key_ft, 1); // reserve space for 1 additional FullType
			ftpair[1] = *val_ft;
			auto ft = new_FullType(llvm::Type::getInt8PtrTy(Context), A_map, nullptr, ftpair);
			return ft;
		}
		default:
			errs() << CurLoc << ": unexpected '" << CurTok << "' - type name expected\n";
			return nullptr;
		}
		getNextToken(expect);
	}
	volvoxc::FullType* type;
	if (lex.peek() == '.') {
		auto mod = IdentifierStr;
		getNextToken();
		getNextToken();
		if (CurTok.kind != tok_identifier) {
			errs() << CurLoc << "identifier expected after '.'\n";
			return nullptr;
		}
		type = lex.get_full_type(mod.c_str(), IdentifierStr.c_str());
	} else {
		type = lex.get_full_type(IdentifierStr.c_str());
	}
	if (!type) {
		errs() << CurLoc << ": unknown type '" << IdentifierStr << "'\n";
		return nullptr;
	}
	getNextToken(expect);
	bool is_ptr = resolve_ref && (attribs & A_ref);
	if (is_ptr)
		attribs &= ~A_ref;
	if (attribs != type->type_attr) {
		type = new_FullType(*type);
		type->type_attr |= attribs;
	}
	if (is_ptr) {
		llvm::Type* ptr_type = type->type->getPointerTo();
		type = new_FullType(ptr_type, 0, nullptr, type);
	}
	return type;
}

// parse argument of function prototype or or element in struct declaration
// typically something like "x type[,)}\n]" - 'x' can be omitted in which case
// the type name is used as name and the lowes bit of FullType* is set to
// flag this condition
static std::pair<std::string,volvoxc::FullType*> ParseTypedIdent(int terminator, bool resolve_ref) {
	unsigned attribs = 0;
	switch (CurTok.kind) {
	case tok_atomic:
		attribs |= A_atomic;
		attribs |= A_ref;
		break;
	case tok_shared:
		attribs |= A_shared;
		attribs |= A_ref;
		break;
	case tok_unique:
		attribs |= A_unique;
		attribs |= A_ref;
		break;
	case tok_const:
		attribs |= A_const;
		attribs |= A_ref;
		break;
	case tok_ref:
		attribs |= A_ref;
		break;
	case tok_optional:
		attribs |= (A_ref | A_optional);
		break;
	default:
		goto no_attribute;;
	}
	getNextToken();
no_attribute:
	if (CurTok.kind != tok_identifier) {
		errs() << CurLoc << ": identifier expected\n";
		return { "", nullptr };
	}
	std::string name = IdentifierStr;
	getNextToken(eComma, terminator);
	if (CurTok.kind == ',' || CurTok.kind == terminator) {
		// type name as ident
		volvoxc::FullType* ft = lex.get_full_type(name.c_str());
		if (!ft) {
			errs() << CurLoc << ": type of '" << name << "' expected\n";
			return { "", nullptr };
		}
		// set lowest bit of 'ft' to indicat 'type as name' condition
		// TODO: handle resolve_ref in this case
		return { name, (volvoxc::FullType*)((uintptr_t)ft | 0x01) };
	}
	return { name, ParseType(attribs, eComma, terminator, nullptr, nullptr, nullptr, false, resolve_ref) };
}

/// numberexpr ::= number
static std::unique_ptr<ExprAST> ParseNumberExpr(int terminator = 0) {
	auto Result = std::make_unique<LiteralExprAST>(std::move(CurTok));
	getNextToken(eBinOp, terminator); // consume the number
	return Result;
}

static std::unique_ptr<ExprAST> ParseStringExpr(int terminator = 0) {
	auto Result = std::make_unique<LiteralExprAST>(std::move(CurTok));
	getNextToken(eBinOp, terminator); // consume the string
	return Result;
}

static std::unique_ptr<ExprAST> ParsePointerExpr(int terminator = 0) {
	auto Result = std::make_unique<LiteralExprAST>(std::move(CurTok));
	getNextToken(eBinOp, terminator); // consume the pointer
	return Result;
}

/// parenexpr ::= '(' expression ')'
static std::unique_ptr<ExprAST> ParseParenExpr(int terminator = 0) {
	getNextToken(); // eat (.
	auto V = ParseExpression(')');
	if (!V)
		return nullptr;

	Eat(')', eBinOp, terminator);
	return V;
}

static std::unique_ptr<ExprAST> ParseIdent(int terminator = 0) {
	std::string Id = IdentifierStr;
	getNextToken(eBinOp, terminator); // eat identifier.
	SourceLocation IdLoc = CurLoc;
	return std::make_unique<IdentExprAST>(IdLoc, std::move(Id));
}

/// identifierexpr
///   ::= identifier
///   ::= identifier '(' expression* ')'
static std::unique_ptr<ExprAST> ParseIdentifierExpr(int terminator = 0) {
	std::string IdName = IdentifierStr;

	SourceLocation LitLoc = CurLoc;

	getNextToken(eBinOp, terminator); // eat identifier.
	// first lookup variable name
	if (auto fv = lookup_var(IdName.c_str()))
		return std::make_unique<VariableExprAST>(LitLoc, IdName, fv);
	// maybe it's a function with this name
	auto protos = lex.findProtos(IdName);
	if (protos) {
		return std::make_unique<FunctionExprAST>(LitLoc, IdName, protos);
	}
	// or a module prefix
	auto im = lex.module->ImportedSymbols.find({ IdName, "" });
	if (im != lex.module->ImportedSymbols.end())
		if (im->second.isPrefix())
			return std::make_unique<ModuleExprAST>(LitLoc, std::move(IdName));
	// or a type name
	if (auto type = lex.get_full_type(IdName.c_str())) {
		if (CurTok.kind == '{')
			if (auto s = ParseStructExpr(type, terminator))
				return s;
		return std::make_unique<TypeExprAST>(LitLoc, std::move(IdName), type);
	}
	// last resort: yet undeclared variable name - used in declaration "x := ..."
	return std::make_unique<VariableExprAST>(LitLoc, IdName);
}

static const char* aggr_kind_str(int kind) {
	switch (kind) {
	case '[':
		return "fixed array";
	case '{':
		return "dynamic array";
	case tok_map:
		return "map";
	case tok_set:
		return "set";
	case tok_chan:
		return "chan";
	case tok_identifier: // struct literal
		return "struct";
	default:
		// we should never get here
		abort();
	}
}

static void aggr_prop_err(SourceLocation Loc, const char* prop, int kind) {
	errs() << Loc << ": \"" << prop << "\" is not a valid property for a " << aggr_kind_str(kind) << '\n';
}

static void aggr_prop_redefinition(SourceLocation Loc, const char* prop) {
	errs() << Loc << ": property \"" << prop << "\" already defined\n";
}

static std::unique_ptr<ListExprAST> ParseListExpr(int terminator = 0) {
	SourceLocation loc = CurLoc;
	if (!Expect('{', eNone))
		return nullptr;
	if (CurTok.kind == '}') {
		getNextToken(eBinOp, terminator);
		return std::make_unique<ListExprAST>(loc);
	}
	auto Elem = ParseExpression('}');
	if (!Expect('}', eBinOp, terminator))
		return nullptr;
	if (!Elem)
		return nullptr;
	auto Elems = SplitExprList(std::move(Elem));
	return std::make_unique<ListExprAST>(loc, std::move(Elems));
}

/*
  Fixed Size (Stack) Arrays
  =========================
  [3, 7, 8]                        # [3]int - type from 1st element
  []f64{init: -1, len: n + 3}      # explicit type - size determined at run time - deprecated
  [n]i32{-4, 2, 7: b}              # size: n - index/key always compile time const
  []f64{}                          # empty fixed array - only useful as function parameter
  
  Variable Size Arrays
  ====================
  {2, 7, 9}                        # {}int - type from 1st element
  {}u64{55, 3, 5: 9, 13: 8}
  {}f64{len: 5, cap: 100, init: 1.5}
  {}f64{}                          # empty array - to be enlarged

  Maps
  ====
  map{"abc": 12.3, "xyz": 9.5}     # map[string]f64 - types from 1st element
  map[u64]string{2: "qw", 54: "tz"}
  map[string]f64{}                 # empty map

  Sets
  ====
  set{12.5, -11, 5.75}             # set of f64 - type from 1st element
  set[u64]{3, 12, 7}               # set of integers - map of keys without values
  set[i32]{}                       # empty set of integers

  Channels
  ========
  chan[f64]{}                      # unbuffered channel
  chan[u64]{cap: 5}                # buffer size 5
*/
static std::unique_ptr<ExprAST> ParseAggregateExpr(bool is_index = false, int terminator = 0) {
	int kind = CurTok.kind;
	// "[3]" can be a fixed array, an index ("a[3]", "f(x)[3]" or "a[4][3]") or
	// part of the type of an aggregate literal ("[3]i32{...}" or [3][3]f64{...}".
	// so spaces before and/or after "[3]" do matter:
	// a [3] [4]f64{...} <---> a[3][4] <---> a[3] [4]f64{...}
	llvm::Type* key_type = nullptr;
	bool key_is_signed = false;
	std::vector<std::unique_ptr<ExprAST>> Dims = {};
	std::vector<std::unique_ptr<ExprAST>> Elems = {};
	volvoxc::FullType* ft;
	SourceLocation loc = CurLoc;
	if ((CurTok.kind == tok_map || CurTok.kind == tok_set) &&  lex.peek() == '{') {
		ft = new_FullType(llvm::Type::getInt8PtrTy(Context), A_map);
		getNextToken();
	}
	else
		ft = ParseType(0, eBinOp, terminator, nullptr, &Dims, nullptr, is_index);
	std::unique_ptr<ExprAST> Init = nullptr;
	std::unique_ptr<ExprAST> Cap = nullptr;
	std::vector<SourceLocation> LenLocs;
	std::unique_ptr<ListExprAST> init_list = nullptr;
	if (ft) {
		for (auto& dim: Dims)
			LenLocs.push_back(dim ? dim->Loc : SourceLocation{0});
		if (CurTok.kind != '{') {
			errs() << CurLoc << ": expression list ('{...}') expected\n";
			return nullptr;
		}
		init_list = ParseListExpr(terminator);
		switch (ft->type->getTypeID()) {
		case llvm::Type::ArrayTyID:
			key_type = llvm::Type::getInt64Ty(Context);
			break;
		case llvm::Type::StructTyID:
			errs() << CurLoc << ": struct literals not implementd, yet\n";
			return nullptr;
		case llvm::Type::PointerTyID:
			if (ft->type_attr & A_map) {
				auto map_ast = std::make_unique<MapExprAST>(loc, ft, std::move(init_list->Elements));
				if (!map_ast->ft || !map_ast->ft->type)
					return nullptr;
				return map_ast;
			}
			return nullptr;
		default:
			errs() << CurLoc << ": " << *ft->type << " as arrgegate type not implemented\n";
			return nullptr;
		}
		if (!init_list->getNumElements()) {
			bool is_valid = (Dims.size() > 0);
			for (int j = 0; j < Dims.size(); j++)
				is_valid = is_valid && Dims[j];
			if (!is_valid) { // TODO: handle struct, map, ...
				errs() << CurLoc << ": empty initialization requires explicit dimension\n";
				return nullptr;
			}
			switch (kind) {
			case '[':
				return std::make_unique<FixedArrayExprAST>(loc, ft, std::vector<std::unique_ptr<ExprAST>>{}, std::vector<ExprAST*>{}, std::vector<unsigned>(Dims.size(), 0), std::move(Dims));
			case '{':
			case tok_map:
			case tok_set:
			case tok_chan:
			case tok_identifier: // struct literal
			default:
				abort();
			}
		}
		Elems = std::move(init_list->Elements);
	} else {
		Elems = std::move(Dims);
		Dims.clear();
	}
	auto iter = ExprListIterator(std::move(Dims));
	std::vector<std::unique_ptr<ExprAST>> Elements = iter.prepare_list(std::move(Elems), 0);
	if (iter.struct_error())
		return nullptr;
	if (ft)
		return std::make_unique<FixedArrayExprAST>(loc, ft, std::move(Elements), std::move(iter.valid_exprs), std::move(iter.LitDims), std::move(iter.Dims), LenLocs);
	else
		return std::make_unique<FixedArrayExprAST>(loc, std::move(Elements), std::move(iter.valid_exprs), std::move(iter.LitDims), nullptr, std::move(iter. Dims), LenLocs);
}

std::unique_ptr<ExprAST> ParseStructExpr(volvoxc::FullType* ft, int terminator) {
	auto Loc = CurLoc;
	auto list = ParseListExpr(terminator);
	if (!list)
		return nullptr;
	if ((ft->type_attr & A_union) && list->getNumElements() > 1) {
		errs() << list->Elements[1]->Loc << ": union literals can have at most 1 element\n";
		return nullptr;
	}
	return std::make_unique<StructExprAST>(Loc, ft, std::move(list));
}

std::vector<std::unique_ptr<ExprAST>> ExprListIterator::prepare_list(std::vector<std::unique_ptr<ExprAST>> Elems, unsigned depth) {
	std::vector<std::unique_ptr<ExprAST>> Elements = {};
	unsigned idx = 0;
	if (struct_err)
		return {};
	if (depth >= Dims.size()) {
		if (explicit_order) {
			struct_err = true;
			errs() << (Elems.size() ? Elems[0]->Loc : CurLoc) << ": array structure invalid - level " << depth << " sublist conflics with array order " << Dims.size() << " given by type\n";
			return {};
		}
		if (valid_exprs.size()) {
			struct_err = true;
			errs() << (Elems.size() ? Elems[0]->Loc : CurLoc) << ": array structure invalid - level " << depth << " sublist conflics with previous non-list elements of lower level\n";
			return {};
		}
		do
			Dims.push_back(nullptr);
		while (Dims.size() <= depth);
	}
	if (LitDims.size() < Dims.size()) {
		LitDims.reserve(Dims.size());
		do
			LitDims.push_back(0);
		while (LitDims.size() < Dims.size());
	}
	for (auto& elem: Elems) {
		if (auto sublist = dynamic_cast<ListExprAST*>(elem.get())) {
			Elements.push_back(std::make_unique<ListExprAST>(CurLoc, prepare_list(std::move(sublist->Elements), depth + 1)));
			idx++;
			if (idx > LitDims[depth])
				LitDims[depth] = idx;
			continue;
		} else if (auto bin_expr = dynamic_cast<BinaryExprAST*>(elem.get())) {
			if (bin_expr->Op[0] == ':' && !bin_expr->Op[1]) { // struct or map
				if (auto ident = dynamic_cast<VariableExprAST*>(bin_expr->LHS.get())) {
					if (kind != tok_identifier) { // no struct, i.e. no field name - so it must be a special built-in
						if (ident->Name == "len") {
							if (kind != '[' && kind != '{') { // no array
								aggr_prop_err(bin_expr->RHS->Loc, "len", kind);
								struct_err = true;
								return {};
							}
							if (Dims[depth]) {
								errs() << bin_expr->RHS->Loc << ": redefinition of array dimension\n"
								       << Dims[depth]->Loc << ": dimension (depth " << depth << ") already defined here\n";
								struct_err = true;
								return {};
							}
							Dims[depth] = std::move(bin_expr->RHS);
						} else if(ident->Name == "cap") {
							if (kind != '{' && kind != tok_chan) { // neither dynamic array nor channel
								aggr_prop_err(ident->Loc, "cap", kind);
								struct_err = true;
								return {};
							}
							if (Cap) {
								aggr_prop_redefinition(ident->Loc, "cap");
								struct_err = true;
								return {};
							}
							Cap = std::move(bin_expr->RHS);
						} else if(ident->Name == "ini") {
							if (kind != '[' && kind != '{') { // no array
								aggr_prop_err(ident->Loc, "ini", kind);
								struct_err = true;
								return {};
							}
							if (Init) {
								aggr_prop_redefinition(ident->Loc, "ini");
								struct_err = true;
								return {};
							}
							Init = std::move(bin_expr->RHS);
						} else {
							aggr_prop_err(ident->Loc, ident->Name.c_str(), kind);
							struct_err = true;
							return {};
						}
					} else {
						; // handle struct element initializer
					}
				} else {
					// LHS of a:b is a general expression - we require it to be a compile time const
					if (llvm::Value* Key = bin_expr->LHS->codegen()) {
						unsigned keyTID = Key->getType()->getTypeID();
						if (auto key = llvm::dyn_cast<llvm::ConstantInt>(Key)) {
							switch ((int)kind) {
							case '[':
							case '{':
								idx = key->getZExtValue();
								if (idx < Elements.size()) {
									errs() << elem->Loc << ": value for index " << idx << " already defined\n";
									struct_err = true;
									return {};
								} else {
									if (!dynamic_cast<ListExprAST*>(bin_expr->RHS.get()) && depth != Dims.size() - 1) {
										struct_err = true;
										errs() << Elements[idx]->Loc << ": array structure invalid - level " << depth << " non-list element conflics with previous deeper sublists\n";
										return {};
									}
									while (Elements.size() < idx)
										Elements.push_back(nullptr);
									if (auto sublist = dynamic_cast<ListExprAST*>(bin_expr->RHS.get()))
										Elements.push_back(std::make_unique<ListExprAST>(CurLoc, prepare_list(std::move(sublist->Elements), depth + 1)));
									else {
										Elements.push_back(std::move(bin_expr->RHS));
									    valid_exprs.push_back(Elements[idx].get());
									}
									idx++;
									if (idx > LitDims[depth])
										LitDims[depth] = idx;
								}
								break;
							default:
								errs() << aggr_kind_str(kind) << " not implemented, yet\n";
								struct_err = true;
								return {};
							}
						} else if (auto key = llvm::dyn_cast<llvm::ConstantFP>(Key)) {
							switch (keyTID) {
							case llvm::Type::FloatTyID:
								break;
							case llvm::Type::DoubleTyID:
								break;
							default:
								;
							}
						} else if (auto key = llvm::dyn_cast<llvm::Constant>(Key)) {
							switch (keyTID) {
							case llvm::Type::PointerTyID:
								break;
							default:
								errs() << bin_expr->LHS->Loc << ": unsupported type " << *bin_expr->LHS->ft->type << " for aggregate key\n";
								struct_err = true;
								return {};
							}
						} else {
							errs() << bin_expr->LHS->Loc << ": key in aggregate literals must be a compile time const\n";
							struct_err = true;
							return {};
						}
					} else {
						errs() << bin_expr->LHS->Loc << ": cannot generate code for key\n";
						struct_err = true;
						return {};
					}
				}
				// element had ':', special treatment was done - continue with next element
				continue;
			} else {
				// element has no ':' - just happens to be a binary expression - get out of nested if
				goto element_without_key;
			}
		}
		// simple element without key
	element_without_key:
		switch ((int)kind) {
		case '[':
		case '{':
			if (idx < Elements.size()) {
				errs() << elem->Loc << ": value for index " << idx << " already defined\n";
				struct_err = true;
				return {};
			} else {
				while (Elements.size() < idx)
					Elements.push_back(nullptr);
				Elements.push_back(std::move(elem));
				if (depth != Dims.size() - 1) {
					struct_err = true;
					errs() << Elements[idx]->Loc << ": array structure invalid - level " << depth << " non-list element conflics with previously determined order " << Dims.size() << '\n';
					return {};
				}
				valid_exprs.push_back(Elements[idx].get());
				idx++;
				if (idx > LitDims[depth])
					LitDims[depth] = idx;
			}
			break;
		default:
			errs() << elem->Loc << ": initialization element for " << aggr_kind_str(kind) << " requires \"key:\"\n";
			struct_err = true;
			return {};
		}
	}
	return Elements;
}

static std::pair<std::vector<std::unique_ptr<ExprAST>>, int> ParseExprList();

inline std::unique_ptr<ExprAST> ParseCondition(TokenKind kind, int terminator = 0) {
	if (kind == tok_until)
		prompt_indent--;
	auto Cond = ParseExpression(terminator);
	if (!Cond)
		return nullptr;
	if (kind != tok_repeat) {
		auto condclose = TokenKind(';');
		if (!Expect(condclose)) {
			errs() << CurLoc << ": <newline> or ';' expected after "
			       << ((kind == tok_in) ? "iterator\n" : "condition\n");
			return nullptr;
		}
	}
	if (kind == tok_if || kind == tok_while || kind == tok_in)
		prompt_indent++;
	return Cond;
}

static std::tuple<std::pair<std::vector<std::unique_ptr<ExprAST>>,int>,VarTable,bool,bool> ParseElse(
	VarTable& then_locals_table, SourceLocation& Loc, TokenKind kind, int ThenEndkind);

/// if..., elif..., while...[elif...]else...end, repeat...until
static std::unique_ptr<ExprAST> ParseIfExpr(int terminator = 0) {
	SourceLocation IfLoc = CurLoc;
	auto kind = TokenKind(CurTok.kind); // to remember if it's 'if', 'while' or 'repeat'
	getNextToken(); // eat the if/while.
	// condition - expect bool.
	std::unique_ptr<ExprAST> Cond;
	if (kind == tok_if || kind == tok_elif || kind == tok_while) {
		Cond = ParseCondition(kind);
		if (!Cond)
			return nullptr;
	}
	locals_table.emplace_back();
	auto old_inside_branch = inside_branch;
	inside_branch = true;
	auto Then = ParseExprList();
	if (!Then.second && Then.first.empty())
		return nullptr;
	inside_branch = old_inside_branch;
	VarTable then_locals_table = std::move(locals_table.back());
	locals_table.pop_back();
	auto [Else, else_locals_table, have_else, success] = ParseElse(then_locals_table, IfLoc, kind, Then.second);
	if (!success)
		return nullptr;
	if (kind == tok_repeat) {
		if (!Expect(tok_until))
			return nullptr;
		Cond = ParseCondition(kind, terminator);
		if (!Cond)
			return nullptr;
	} else {
		if (have_else && Else.second == tok_end && Then.second != tok_elif || !have_else && Then.second == tok_end)
			if (!Expect(tok_end, eBinOp)) {
				errs() << CurLoc << ": 'end' expected\n";
				return nullptr;
			}
	}
	bool always_return = Then.second == tok_return && have_else && Else.second == tok_return;
	auto res_t = ((kind == tok_if || kind == tok_elif) && Else.first.size() && Else.first.back()->ft->type &&
	              !Else.first.back()->ft->type->isVoidTy() && Then.first.back()->ft->type &&
	              !Then.first.back()->ft->type->isVoidTy() &&
	              !(Then.second == tok_return || have_else && Else.second == tok_return)) ?
		getResType(Then.first.back()->ft->type, Else.first.back()->ft->type, "if",
		           Then.first.back()->ft->type_attr, Else.first.back()->ft->type_attr,
		           Then.first.back()->is_unknown_type, Else.first.back()->is_unknown_type)
		: std::tuple<llvm::Type*, unsigned, bool, OpClass, const char*>{ llvm::Type::getVoidTy(Context),
		                                                                 0, false, OpNormal, nullptr };
	return std::make_unique<IfExprAST>(IfLoc, std::move(Cond), std::move(Then.first),
	                                   std::move(Else.first), Then.second, Else.second, std::move(then_locals_table), std::move(else_locals_table), res_t, kind == tok_elif ? tok_if : kind, always_return);
}

static std::tuple<std::pair<std::vector<std::unique_ptr<ExprAST>>,int>,VarTable,bool,bool> ParseElse(
	VarTable& then_locals_table, SourceLocation& Loc, TokenKind kind, int ThenEndkind)
{
	std::pair<std::vector<std::unique_ptr<ExprAST>>, int> Else;
	bool have_else = false;
	if (ThenEndkind == tok_else || ThenEndkind == tok_elif) {
		have_else = true;
		if (ThenEndkind != tok_elif)
			getNextToken();
	} else if (ThenEndkind == tok_return) {
		while (CurTok.kind == ';')
			getNextToken();
		if (CurTok.kind == tok_end) {
			getNextToken();
			have_else = false;
		} else if (CurTok.kind == tok_else || CurTok.kind == tok_elif) {
			if (CurTok.kind != tok_elif)
				getNextToken();
			have_else = true;
		} else {
			errs() << CurLoc << ": 'else', 'elif' or 'end' expected\n";
			return { std::pair<std::vector<std::unique_ptr<ExprAST>>,int>{
					std::vector<std::unique_ptr<ExprAST>>(), 0 }, VarTable{}, false, false };
		}
	}
	if (have_else) {
		if (kind == tok_repeat) {
			errs() << CurLoc << ": 'else' not allowed with 'repeat'\n";
			return { std::pair<std::vector<std::unique_ptr<ExprAST>>,int>{
					std::vector<std::unique_ptr<ExprAST>>(), 0 }, VarTable{}, false, false };
		}
		locals_table.emplace_back();
		auto old_inside_branch = inside_branch;
		inside_branch = true;
		if (CurTok.kind == tok_elif) {
			auto elif_expr = ParseIfExpr();
			auto elifif_expr = dynamic_cast<IfExprAST*>(elif_expr.get());
			if (!elifif_expr) {
				errs() << CurLoc << ": invalid 'if ... elif' structure\n";
				return { std::pair<std::vector<std::unique_ptr<ExprAST>>,int>{
						std::vector<std::unique_ptr<ExprAST>>(), 0 }, VarTable{}, false, false };
			}
			auto end_k = elifif_expr->always_return ? tok_return : tok_end;
			std::vector<std::unique_ptr<ExprAST>> l;
			l.push_back(std::move(elif_expr));
			Else = { std::move(l), end_k };
		} else {
			Else = ParseExprList();
			if (!Else.second && Else.first.empty())
				return { std::pair<std::vector<std::unique_ptr<ExprAST>>,int>{
						std::vector<std::unique_ptr<ExprAST>>(), 0 }, VarTable{}, false, false };
			if (Else.second == tok_return) {
				while (CurTok.kind == ';')
					getNextToken();
				if (CurTok.kind == tok_end) {
					getNextToken();
				} else {
					errs() << CurLoc << ": 'end' expected\n";
					return { std::pair<std::vector<std::unique_ptr<ExprAST>>,int>{
							std::vector<std::unique_ptr<ExprAST>>(), 0 }, VarTable{}, false, false };
				}
			}
		}
		inside_branch = old_inside_branch;
	} else {
		Else = { std::vector<std::unique_ptr<ExprAST>>(), 0 };
	}
	VarTable else_locals_table = have_else ? std::move(locals_table.back()) : VarTable();
	if (kind == tok_repeat) {
		if (then_locals_table.table) {
			for (auto then_node = then_locals_table.first(); then_node; ++then_node) {
				MapValue* node = then_node.getValue();
				auto then_var = (FullVar*)((char*)node + node->offset);
				if (!locals_table.back().insert(then_node.getKey(), *then_var)) {
					errs() << Loc << ": Variable '" << then_node.getKey() << "' already exists in outer scope\n";
					return { std::pair<std::vector<std::unique_ptr<ExprAST>>,int>{
							std::vector<std::unique_ptr<ExprAST>>(), 0 }, VarTable{}, false, false };
				}
			}
		}
	} else if (have_else) {
		locals_table.pop_back();
		if (then_locals_table.table && else_locals_table.table) {
			for (auto then_node = then_locals_table.first(); then_node; ++then_node) {
				FullVar* else_var = else_locals_table[then_node.getKey()];
				if (else_var) {
					if (!locals_table.back().insert(then_node.getKey(), *else_var)) {
						errs() << Loc << ": Variable '" << then_node.getKey() << "' already exists in outer scope\n";
						return { std::pair<std::vector<std::unique_ptr<ExprAST>>,int>{
								std::vector<std::unique_ptr<ExprAST>>(), 0 }, VarTable{}, false, false };
					}
				}
			}
		}
	}
	return { std::move(Else), std::move(else_locals_table), have_else, true };
}

// try to add new variable to current context's database
// return values:
//   - nullptr:    error, e.g. symbol already in use as function or module name
//   - otherwise:  pointer to description of variable
//      * bit 0 set:   variable was newly created
//      * bit 0 clean: variable has existed before

static FullVar* DeclareNewVariable(std::unique_ptr<ExprAST>& LHS, std::unique_ptr<ExprAST>* RHS,
                               llvm::Type* LHS_type, llvm::Type* RHS_type, unsigned LHS_attr,
                               unsigned RHS_attr, SourceLocation& BinLoc, bool LHS_is_unknown_type,
                               bool RHS_is_unknown_type, bool ref_allowed = true, bool is_iterator = false)
{
	if (!RHS_type || RHS_type->isVoidTy()) {
		errs() << BinLoc << ": RHS of declaration is " << (RHS_type ? "of void type\n" : "indeterminate\n");
		return nullptr;
	}
	ReferenceExprAST* RefL;
	VariableExprAST* VarL = nullptr;
	if (auto v = dynamic_cast<VariableExprAST*>(LHS.get())) {
		if (v->full_var)
			return v->full_var;
		VarL = v;
	} else if (auto function = dynamic_cast<FunctionExprAST*>(LHS.get())) {
		if (inside_function && local_var_may_shadow_func_and_mod) { // local variable will shadow function name
			LHS = std::make_unique<VariableExprAST>(function->Loc, function->Name, nullptr);
			VarL = dynamic_cast<VariableExprAST*>(LHS.get());
		} else {
			errs() << LHS->Loc << ": '" << function->Name << "' is already declared as function\n";
			return nullptr;
		}
	} else if (auto mod = dynamic_cast<ModuleExprAST*>(LHS.get())) {
		if (inside_function && local_var_may_shadow_func_and_mod) {
			LHS = std::make_unique<VariableExprAST>(mod->Loc, mod->Name, nullptr);
			VarL = dynamic_cast<VariableExprAST*>(LHS.get());
		} else {
			errs() << LHS->Loc << ": '" << mod->Name << "' is already in use as module prefix\n";
			return nullptr;
		}
	}
	if (VarL)
		RefL = nullptr;
	else
		if ((RefL = dynamic_cast<ReferenceExprAST*>(LHS.get()))) {
			if (!ref_allowed) {
				errs() << LHS->Loc << ": reference not allowed "
				      << (is_iterator ? " with this iterator\n" : "in this case\n");
				return nullptr;
			}
			VarL = dynamic_cast<VariableExprAST*>(RefL->Operand.get());
			if (VarL->full_var)
				return VarL->full_var;
			if (auto call_expr = dynamic_cast<CallExprAST*>((*RHS).get())) {
				// LHS is function pointer; signature of RHS will be used to select overloaded function
				if (auto typeexpr = dynamic_cast<TypeExprAST*>(call_expr->Callee.get())) {
					errs() << LHS->Loc << ": references to constructors or conversions not allowed ('" << typeexpr->Name << "' is a type)\n";
					return nullptr;
				} else if (auto method = dynamic_cast<MethodExprAST*>(call_expr->Callee.get())) {
					errs() << LHS->Loc << ": references to methods not allowed ('" << method->Method->Name << "' is a method of type '" << *method->Receiver->ft << "')\n";
					return nullptr;
				}
				*RHS = std::make_unique<FunctionExprAST>(call_expr);
				RHS_type = (*RHS)->ft ? (*RHS)->ft->type : nullptr;
				RHS_attr = 0;
				RHS_is_unknown_type = false;
				LHS = std::move(RefL->Operand);
				RefL = nullptr;
				LHS_type = LHS->ft ? LHS->ft->type : nullptr;
				LHS_attr = LHS->ft ? LHS->ft->type_attr : 0;
				LHS_is_unknown_type = LHS->is_unknown_type;
				VarL = dynamic_cast<VariableExprAST*>(LHS.get());
			}
		}
	if (!VarL) {
		errs() << LHS->Loc << ": left operand of \":=\" must be a variable\n";
		return nullptr;
	} else {
		auto [type, is_signed] = MakeType(RHS_type, RHS_attr & A_signed, RHS_is_unknown_type);
		FullVar fv = {
			.val = nullptr,
			.ft = RHS ? *(*RHS)->ft : volvoxc::FullType{}
		};
		fv.ft.type = type;
		fv.ft.type_attr &= ~(A_global | A_const | A_rvalue | A_mainvar);
		if (is_signed)
			fv.ft.type_attr |= A_signed;
		else
			fv.ft.type_attr &= ~A_signed;
		if (RefL)
			if (dynamic_cast<LvalueExprAST*>(LHS.get()))
				fv.ft.type_attr = (fv.ft.type_attr | A_ptrref) & ~A_destructor; // references need no destructors
			else {
				errs() << (RHS ? (*RHS)->Loc : CurLoc) << ": RHS of reference declaration must be an lvalue\n";
				return nullptr;
			}
		else if (llvm::isa<llvm::ArrayType>(fv.ft.type) && (fv.ft.elem_type->type_attr & A_destructor)) {
			fv.ft.type_attr |= A_destructor;
		}
		if (inside_function || inside_branch) {
			if (auto entry = locals_table.back().insert(VarL->Name.c_str(), fv)) {
				VarL->full_var = nullptr; // in case a global with the same name had been found
				return (FullVar*)((uintptr_t)entry | 1);
			} else {
				errs() << VarL->Loc << ": variable '" << VarL->Name << "' already exists in current scope\n";
				return nullptr;
			}
		} else {
			fv.ft.type_attr |= A_mainvar;
			if (auto entry = lex.module->globals_table.insert(VarL->Name.c_str(), fv)) {
				return (FullVar*)((uintptr_t)entry | 1);
			} else {
				errs() << VarL->Loc << ": variable '" << VarL->Name << "' already exists in \"main\" scope\n";
				return nullptr;
			}
			// errs() << VarL->Loc << ": inserted " << VarL->Name << ", " << fv.ft.type_attr << " in mainvars\n";
		}
	}
}

/// for...in...;...[elif...]else...end
static std::unique_ptr<ExprAST> ParseForExpr(int terminator = 0) {
	SourceLocation ForLoc = CurLoc;
	getNextToken(); // eat for.
	// condition - expect bool.
	locals_table.emplace_back();
	auto old_inside_branch = inside_branch;
	inside_branch = true;
	auto KeyVal = ParseExpression(tok_in);
	if (!Expect(tok_in, eNone)) {
		errs() << CurLoc << ": 'in' expected!\n";
		return nullptr;
	}
	std::unique_ptr<ExprAST> Key;
	std::unique_ptr<ExprAST> Value;
	BinaryExprAST* bin_expr;
	if ((bin_expr = dynamic_cast<BinaryExprAST*>(KeyVal.get())) && bin_expr->Op[0] == ',' && !bin_expr->Op[1]) {
		Key = std::move(bin_expr->LHS);
		Value = std::move(bin_expr->RHS);
	} else {
		Value = std::move(KeyVal);
	}
	if (!Key && !Value) {
		errs() << CurLoc << ": at least one control variable must be declared for 'for' loop\n";
		return nullptr;
	}
	std::string KeyName;
	std::string ValueName;
	if (Key) {
		if (auto lvalKey = dynamic_cast<LvalueExprAST*>(Key.get())) {
			KeyName = lvalKey->Name;
		} else {
			errs() << Key->Loc << ": 'for' key control variable must be an Lvalue\n";
			return nullptr;
		}
	}
	if (Value) {
		if (auto lvalValue = dynamic_cast<LvalueExprAST*>(Value.get())) {
			ValueName = lvalValue->Name;
		} else {
			errs() << Value->Loc << ": 'for' key control variable must be an Lvalue\n";
			return nullptr;
		}
	}
	auto Iterator = ParseCondition(tok_in);
	auto [KeyFt, ValueFt, IteratorTy] = getKeyValueIteratorTypes(Iterator->ft);
	FullVar* KeyFV = nullptr;
	FullVar* ValueFV = nullptr;
	if (Key) {
		if (!KeyFt) {
			errs() << Iterator->Loc << ": unable to determine type of 'for' key control variable\n";
			return nullptr;
		}
		if (auto key_var = dynamic_cast<VariableExprAST*>(Key.get())) {
			if (key_var->full_var)
				KeyFV = key_var->full_var;
			else
				if (!(KeyFV = (FullVar*)((uintptr_t)DeclareNewVariable(
					                         Key, nullptr, Key->ft->type, KeyFt->type, Key->ft->type_attr,
					                         KeyFt->type_attr, Key->Loc, Key->is_unknown_type,
					                         Iterator->is_unknown_type, false, true) & ~(uintptr_t)1))) {
					errs() << key_var->Loc << ": unable to declare key control variable '" << key_var->Name
					       << "'\n";
					return nullptr;
				}
		}
	}
	if (Value) {
		if (!ValueFt) {
			errs() << Iterator->Loc << ": unable to determine type of 'for' value control variable\n";
			return nullptr;
		}
		if (auto value_var = dynamic_cast<VariableExprAST*>(Value.get())) {
			if (value_var->full_var)
				ValueFV = value_var->full_var;
			else
				if (!(ValueFV = (FullVar*)((uintptr_t)DeclareNewVariable(
					                           Value, nullptr, Value->ft->type, ValueFt->type, Value->ft->type_attr,
					                           ValueFt->type_attr, Value->Loc, Value->is_unknown_type,
					                           Iterator->is_unknown_type, false, true) & ~(uintptr_t)1))) {
					errs() << value_var->Loc << ": unable to declare value control variable '" << value_var->Name
					       << "'\n";
					return nullptr;
				}
		}
	}
	auto Body = ParseExprList();
	if (!Body.second && Body.first.empty())
		return nullptr;
	inside_branch = old_inside_branch;
	VarTable then_locals_table = std::move(locals_table.back());
	locals_table.pop_back();
	auto [Else, else_locals_table, have_else, success] = ParseElse(then_locals_table, ForLoc, tok_for, Body.second);
	if (!success)
		return nullptr;
	if (have_else && Else.second == tok_end && Body.second != tok_elif || !have_else && Body.second == tok_end)
		if (!Expect(tok_end, eBinOp)) {
			errs() << CurLoc << ": 'end' expected\n";
			return nullptr;
		}
	return std::make_unique<ForExprAST>(ForLoc, std::move(Iterator), std::move(then_locals_table),
	                                    std::move(else_locals_table), std::move(Key), std::move(Value),
	                                    std::move(KeyName), std::move(ValueName), std::move(Body.first),
	                                    std::move(Else.first), Body.second, Else.second, ValueFV, KeyFV);
}

static std::unique_ptr<ExprAST> ParseFunctionExpr(int terminator = 0) {
	auto FnLoc = CurLoc;
	unsigned visibility = A_closure;
	auto ast = ParseDefinition(visibility);
	return std::make_unique<FunctionExprAST>(FnLoc, std::move(ast));
}

/// primary
///   ::= identifierexpr
///   ::= numberexpr
///   ::= parenexpr
///   ::= ifexpr
///   ::= forexpr
///   ::= varexpr
static std::unique_ptr<ExprAST> ParsePrimary(int terminator = 0) {
	switch ((int)CurTok.kind) {
	case tok_eof:
		errs() << "EOF when expecting an expression\n";
		exit(1);
	case tok_identifier:
		return ParseIdentifierExpr(terminator);
	case tok_number:
		return ParseNumberExpr(terminator);
	case tok_str_lit:
		return ParseStringExpr(terminator);
	case tok_ptr_lit:
		return ParsePointerExpr(terminator);
	case '(':
		return ParseParenExpr(terminator);
	case ')':
		if ((int)CurTok.kind != terminator) {
			errs() << CurLoc << ": superfluous ')'\n";
			purgeLine();
			return nullptr;
		}
		return std::make_unique<EmptyExprAST>();
	case '{':
		return ParseListExpr(terminator);
	case '[':
	case tok_map:
	case tok_set:
	case tok_chan:
		return ParseAggregateExpr(false, terminator);
	case tok_if:
	case tok_while:
	case tok_repeat:
		return ParseIfExpr(terminator);
	case tok_for:
		return ParseForExpr(terminator);
	case tok_fn:
		return ParseFunctionExpr(terminator);
	default:
		errs() << CurLoc << ": unexpected token '" << CurTok.str() << "' when expecting a " << lex.Expected << " or an expression\n";
		purgeLine();
		return nullptr;
	}
}

static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec, std::unique_ptr<ExprAST> LHS, int terminator = 0);

/// unary
///   ::= primary
///   ::= '!' unary
static std::unique_ptr<ExprAST> ParseUnary(int terminator = 0) {
	// If the current token is not an unary prefix operator, it must be a primary expr.
	auto kind = CurTok.kind;
	if (kind != tok_unary && kind != tok_ref && kind != tok_optional && kind != tok_task)
		return ParsePrimary(terminator);
	
	// If this is a unary operator, read it.
	int TokPrec = GetTokPrecedence();
	std::string Op = IdentifierStr;
	auto Loc = CurLoc;
	getNextToken();
	if (auto Operand = ParseUnary(terminator)) {
		Operand = ParseBinOpRHS(TokPrec, std::move(Operand), terminator);
		if (!Operand)
			return nullptr;
		if (kind == tok_ref || kind == tok_optional) {
			if (auto lval = dynamic_cast<LvalueExprAST*>(Operand.get())) {
				auto Lval = std::unique_ptr<LvalueExprAST>(lval);
				Operand.release();
				return std::make_unique<ReferenceExprAST>(Loc, std::move(Lval), kind == tok_optional);
			}
			errs() << Loc << ": reference operator '&' cannot be applied to rvalue\n";
			return nullptr;
		} else if (kind == tok_task) {
			if (auto call = dynamic_cast<CallExprAST*>(Operand.get())) {
				auto Call = std::unique_ptr<CallExprAST>(call);
				Operand.release();
				return std::make_unique<TaskExprAST>(Loc, std::move(Call));
			}
			errs() << Operand->Loc << ": 'task' requires a call expression as operand\n";
			return nullptr;
		}
		return std::make_unique<UnaryExprAST>(Loc, Op.c_str(), std::move(Operand));
	}
	return nullptr;
}

/// binoprhs
///   ::= ('+' unary)*
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec, std::unique_ptr<ExprAST> LHS, int terminator) {
	// If this is a binop, find its precedence.
	while (true) {
		int TokPrec = GetTokPrecedence();
		// If this is a binop that binds at least as tightly as the current binop,
		// consume it, otherwise we are done.
		if (!LHS->ft)
			return nullptr;
		if (NextTokPrecedence() <= ExprPrec) {
			if (LHS->ft->type && (LHS->ft->type->isFunctionTy() || dynamic_cast<TypeExprAST*>(LHS.get())))
				LHS = std::make_unique<CallExprAST>(LHS->Loc, std::move(LHS), std::vector<std::unique_ptr<ExprAST>>{});
			return LHS;
		}
		// Okay, we know this is a binop.
		std::string BinOp = IdentifierStr;
		SourceLocation BinLoc = CurLoc;
		auto BinKind = CurTok.kind;
		if (LHS->ft->type && (LHS->ft->type->isFunctionTy() || dynamic_cast<TypeExprAST*>(LHS.get()))) {
			// make this a call expression even without '()' if the following if followed by a usual operand
			// (';' and '\n' are handled above or below. The ',' case will need special handling if used inside LHS
			// of decl-assign but this can only be done later when the '=' operator has been seen
			if (BinKind == tok_selector && BinOp != "(" || BinKind >= tok_mult && BinKind < tok_colon || BinKind == tok_comma) {
				LHS = std::make_unique<CallExprAST>(LHS->Loc, std::move(LHS), std::vector<std::unique_ptr<ExprAST>>{});
				continue;
			}
		}
		if (BinKind == tok_postfix) {
			auto lval = dynamic_cast<LvalueExprAST*>(LHS.get());
			if (!lval) {
				errs() << LHS->Loc << ": postfix operator '" << BinOp << "' requires lvalue as operand\n";
				return nullptr;
			}
			auto Lvalue = std::unique_ptr<LvalueExprAST>(lval);
			LHS.release();
			LHS = std::make_unique<PostfixExprAST>(BinLoc, BinOp.c_str(), std::move(Lvalue));
			getNextToken(eBinOp); // eat postfix operator and expect binop/terminator
			continue;
		} else
			getNextToken(); // eat binop
		// Parse the unary expression after the binary operator.
		bool is_index = BinKind == tok_selector && CurTok.kind == '[';
		bool is_dotselect = BinKind == tok_selector && BinOp == ".";
		std::unique_ptr<ExprAST> RHS;
		if (is_index)
			RHS =  ParseAggregateExpr(true, terminator);
		else
			if (is_dotselect)
				RHS = ParseIdent(terminator);
			else
				RHS = ParseUnary(terminator);
		if (!RHS)
			return nullptr;

		// If BinOp binds less tightly with RHS than the operator after RHS, let
		// the pending operator take RHS as its LHS.
		if (TokPrec <= NextTokPrecedence()) {
			RHS = ParseBinOpRHS(TokPrec, std::move(RHS), terminator);
		}
		if (!RHS || !RHS->ft)
			return nullptr;
		if (RHS->ft->type && RHS->ft->type->isFunctionTy())
			RHS = std::make_unique<CallExprAST>(RHS->Loc, std::move(RHS), std::vector<std::unique_ptr<ExprAST>>{});
		// Merge LHS/RHS.
		// save types befor objects are moved
		auto LHS_type = LHS->ft ? LHS->ft->type : nullptr;
		auto LHS_attr = LHS->ft ? LHS->ft->type_attr : 0;
		auto LHS_is_unknown_type = LHS->is_unknown_type;
		auto RHS_type = RHS->ft ? RHS->ft->type : nullptr;
		auto RHS_attr = RHS->ft ? RHS->ft->type_attr : 0;
		auto RHS_is_unknown_type = RHS->is_unknown_type;
		if (BinOp == ":=" || BinOp == "::=") {
			if (!((uintptr_t)DeclareNewVariable(LHS, &RHS, LHS_type, RHS_type, LHS_attr, RHS_attr,
			                                    BinLoc, LHS_is_unknown_type, RHS_is_unknown_type) & ~(uintptr_t)1))
				return nullptr;
		} else if (LHS_type && (LHS_type->isFunctionTy() || dynamic_cast<TypeExprAST*>(LHS.get()))) {
			if (BinOp[0] == '(' || BinOp[0] == '\0') {
				auto Args = SplitExprList(std::move(RHS));
				LHS = std::make_unique<CallExprAST>(LHS->Loc, std::move(LHS), std::move(Args));
				continue;
			} else if (LHS_type->isFunctionTy() && BinOp[0] == '=' && BinOp[1] == '\0') {
				// LHS virtual attribute method call like 'tm.month = 5'
				std::vector<std::unique_ptr<ExprAST>> arglist;
				arglist.push_back(std::move(RHS));
				LHS = std::make_unique<CallExprAST>(LHS->Loc, std::move(LHS), std::move(arglist));
				continue;
			}
		} else if (is_index) {
			if (!LHS->ft || !LHS->ft->type) {
				errs() << LHS->Loc << ": ";
				if (auto lval = dynamic_cast<LvalueExprAST*>(LHS.get()))
					if (!lval->Name.empty()) {
						errs() << "unknown identifier '" << lval->Name << "'";
						goto have_varname2;
					}
				errs() << "undefined expression";
			have_varname2:
				errs() << " - array expected\n";
				return nullptr;
			}
			LHS = std::make_unique<IndexExprAST>(LHS->Loc, std::move(LHS), std::move(RHS));
			continue;
		} else if (is_dotselect) {
			auto ident = dynamic_cast<IdentExprAST*>(RHS.get());
			if (!ident) {
				errs() << RHS->Loc << ": identifier expected\n";
				return nullptr;
			}
			auto Ident = std::unique_ptr<IdentExprAST>(ident);
			RHS.release();
			if (auto mod = dynamic_cast<ModuleExprAST*>(LHS.get())) {
				auto im = lex.module->ImportedSymbols.find({ mod->Name, Ident->Name });
				if (im == lex.module->ImportedSymbols.end()) {
					errs() << Ident->Loc << ": no 'pub' symbol '" << Ident->Name << "' in module '" << mod->Name << "'\n";
					return nullptr;
				} else {
					std::string fqname = mod->Name + "." + ident->Name;
					// the following is similar to corresponding code in ParseIdentifierExpr()
					if (auto var = im->second.getFullVar()) {
						LHS = std::make_unique<VariableExprAST>(mod->Loc, ident->Name /*fqname?*/, var);
						continue;
					} else if (auto protos = im->second.getProtos()) {
						LHS = std::make_unique<FunctionExprAST>(mod->Loc, fqname, protos);
						continue;
					} else if (auto type = im->second.getFullType()) {
						if (CurTok.kind == '{')
							LHS = ParseStructExpr(type, terminator);
						else {
							LHS = std::make_unique<TypeExprAST>(mod->Loc, ident->Name, type);
						}
						continue;
					} else {
						errs() << LHS->Loc << ": cannot evaluate '" << fqname << "'\n";
						return nullptr;
					}
				}
			} else if (LHS->ft && LHS->ft->type) {
				if (LHS->ft->mangled_name) {
					auto proto = MethodProtos.find({LHS->ft->mangled_name, Ident->Name});
					if (proto != MethodProtos.end()) {
						LHS = std::make_unique<MethodExprAST>(LHS->Loc, std::move(LHS), std::move(Ident), &proto->second);
						continue;
					}
				}
				LHS = std::make_unique<SelectExprAST>(LHS->Loc, std::move(LHS), std::move(Ident));
				continue;
			} else {
				errs() << LHS->Loc << ": ";
				if (auto lval = dynamic_cast<LvalueExprAST*>(LHS.get()))
					if (!lval->Name.empty()) {
						errs() << "'" << lval->Name << "'";
						goto have_varname;
					}
				errs() << "LHS of '.'";
			have_varname:
				errs() << " is neither a module nor a known variable nor a struct literal\n";
				return nullptr;
			}
		}
		auto res_t = getResType(LHS_type, RHS_type, BinOp.c_str(), LHS_attr, RHS_attr,
		                        LHS_is_unknown_type, RHS_is_unknown_type);
		if (std::get<4>(res_t)) {
			errs() << BinLoc << ": " << llvm::format(std::get<4>(res_t), BinOp.c_str());
			return nullptr;
		}
		if ((!RHS_type || RHS_type->isVoidTy()) && !dynamic_cast<ListExprAST*>(RHS.get())) {
			if (BinOp == ",") {
				if (auto bin_rhs = dynamic_cast<BinaryExprAST*>(RHS.get())) {
					if (!strcmp(bin_rhs->Op, ",") || !strcmp(bin_rhs->Op, ":"))
						goto valid_void;
				} else {
					goto valid_void; // TODO: more sophisticated check if this is allowed,
					                 // e.g. "global a, b, c", "x, y = f()", ...
				}
				if (!RHS_type && terminator == tok_in)
					goto valid_void; // allow declaration of control variables
			}
			if (auto lval = dynamic_cast<LvalueExprAST*>(RHS.get()))
				if (!lval->Name.empty())
					errs() << lval->Loc << ": unknown identifier '" << lval->Name << "'\n";
			errs() << BinLoc << ": RHS of '" << BinOp << "' is " << (RHS_type ? "of void type\n" : "indeterminate\n");
			return nullptr;
		}
	valid_void:
		LHS = std::make_unique<BinaryExprAST>(BinLoc, BinOp.c_str(), std::move(LHS), std::move(RHS), res_t);
	}
}

/// expression
///   ::= unary binoprhs
///
std::unique_ptr<ExprAST> ParseExpression(int terminator) {
	auto LHS = ParseUnary(terminator);
	if (LHS && parseOk)
		LHS = ParseBinOpRHS(0, std::move(LHS), terminator);
	if (!parseOk) {
		parseOk = true;
		return nullptr;
	}
	return LHS;
}

static bool MarkAsGlobalVar(ExprAST* expr) {
	if (auto var_expr = dynamic_cast<VariableExprAST*>(expr)) {
		if (var_expr->full_var) {
			errs() << var_expr->Loc << "'" << var_expr->Name << "' is already in use as "
			       << (var_expr->full_var->global ? "reference to global\n" : "local variable\n");
			return false;
		}
		if (auto global_fv = lookup_var(var_expr->Name.c_str(), true)) {
			FullVar fv = {
				.global = global_fv
			};
			locals_table.back().insert(var_expr->Name.c_str(), fv);
			return true;
		} else {
			errs() << var_expr->Loc << ": there is no known global variable '" << var_expr->Name << "'\n";
			return false;
		}
	} else {
		errs() << expr->Loc << ": comma separated list of variable names expected\n";
		return false;
	}
}

static std::pair<std::unique_ptr<ExprAST>, int> ParseExprOrReturn() {
	while (CurTok.kind == ';')
		getNextToken();
	auto kind = CurTok.kind;
	if (kind == tok_return || kind == tok_else || kind == tok_elif || kind == tok_end || kind == tok_until) {
		if (kind == tok_return) {
			getNextToken(eSemi);
			if (CurTok.kind == ';' || CurTok.kind == tok_end) 
				return { nullptr, kind };
			else
				return { ParseExpression(), kind };
		}
		else
			return { nullptr, kind };
	} else if (kind == tok_global) {
		if (!inside_function) {
			errs() << CurLoc << ": global list useless outside functions\n";
			return { nullptr, 0 };
		}
		getNextToken();
		auto globals_list = ParseExpression();
		while (auto bin_expr = dynamic_cast<BinaryExprAST*>(globals_list.get())) {
			if (bin_expr->Op[0] != ',') {
				errs() << bin_expr->Loc << ": ',' expected as separator in 'global' list\n";
				return { nullptr, 0 };
			}
			if (!MarkAsGlobalVar(bin_expr->RHS.get()))
				return { nullptr, 0 };
			globals_list = std::move(bin_expr->LHS);
		}
		if (!MarkAsGlobalVar(globals_list.get()))
			return { nullptr, 0 };
		else
			return { nullptr, kind };
	} else {
		return { ParseExpression(), 0 };
	}
}

static std::pair<std::vector<std::unique_ptr<ExprAST>>, int> ParseExprList() {
	std::vector<std::unique_ptr<ExprAST>> expr_list;
	int end_kind = 0;
	while (!end_kind) {
		auto expr = ParseExprOrReturn();
		end_kind = expr.second;
		if (expr.first) {
			if (!end_kind)
				if (auto I = dynamic_cast<IfExprAST*>(expr.first.get()))
					if (I->ThenEndKind == tok_return && I->ElseEndKind == tok_return)
						end_kind = tok_return;
			expr_list.push_back(std::move(expr.first));
		} else {
			if (!end_kind)
				return { std::vector<std::unique_ptr<ExprAST>>{}, 0 };
			if (end_kind == tok_global)
				end_kind = 0;
		}
	}
	return { std::move(expr_list), end_kind };
}

/// prototype
///   ::= id '(' id* ')'
///   ::= binary LETTER number? (id, id)
///   ::= unary LETTER (id)
static std::unique_ptr<PrototypeAST> ParsePrototype(unsigned& visibility) {
	std::string FnName;
	volvoxc::FullType* ReceiverType = nullptr;
	std::string UnmagledReceiverTypeName;
	SourceLocation FnLoc = CurLoc;

	unsigned Kind = 0; // 0 = identifier, 1 = unary, 2 = binary.
	std::vector<std::string> ArgNames;
	std::vector<volvoxc::FullType*> ArgTypes;
	std::vector<SourceLocation> ArgPos;
	bool isVarArgs = false;
	volvoxc::FullType* tmp_rec_type = nullptr;
	if (!(visibility & A_closure)) {
		if (CurTok.kind != tok_identifier) {
			errs() << CurLoc << ": identifier expected (function name or receiver type)\n";
			return nullptr;
		}
		// make sure the function name is not in use as (global/main) variable
		// if "inside_function" we would not see conflicting non-global main vars, so clear this flag temporarily
		auto old_inside_function = inside_function;
		inside_function = false;
		if (auto var = lookup_var(IdentifierStr.c_str())) {
			errs() << CurLoc << ": symbol '" << IdentifierStr << "' is already in use as variable of type '" << var->ft << "'\n";
			return nullptr;
		}
		inside_function = old_inside_function;
		// identify constructors and destructors:
		// all kinds of methods start with a type name - even destructors as we
		// have eaten the '~' already in ParseDefinition()
		tmp_rec_type = lex.get_full_type(IdentifierStr.c_str());
		if (tmp_rec_type) {
			visibility |= A_method; // 1st token of function name is known type -> must be method
			if (visibility & A_destructor) {
				if (!last_defined_type) {
					errs() << FnLoc << ": destructor definition is only valid immediately after type definition\n";
					return nullptr;
				} else if (IdentifierStr != last_defined_type) {
					errs() << CurLoc << ": destructor must refer to type of preceding definition ('" << last_defined_type << "')\n";
					return nullptr;
				}
				tmp_rec_type->type_attr |= A_destructor; // mark this type in database to have destructor
			} else if (lex.peek() != '.') {
				visibility |= A_constructor;
			}
			if (visibility & A_c_api) {
				errs() << CurLoc << ": methods/constructors/destructors cannot be declared using C-API - use 'fn' instead of 'cfn'\n";
				return nullptr;
			}
			if ((visibility & (A_destructor | A_constructor)) && (!last_defined_type || IdentifierStr != last_defined_type)) {
				errs() << CurLoc << ": constructor/destructor must refer to type of preceding definition ('" << last_defined_type << "')\n";
				return nullptr;
			}
			ReceiverType = new_FullType(*tmp_rec_type);
			// TODO: avoid creating new FullTypes just for adding attributes
			// This will require ParseType() to return attributes separately
			// and ProtoTypeAST::ProtoTypeAST() to get ArgAttrs passed
			ReceiverType->type_attr |= A_ref;
			UnmagledReceiverTypeName = std::move(IdentifierStr);
			ArgNames.push_back("this");
			ArgTypes.push_back(ReceiverType);
			ArgPos.push_back(CurLoc);
			if (!(visibility & (A_destructor | A_constructor))) {
				getNextToken(eBinOp, eSemi);
				if (!Expect(tok_selector))
					return nullptr;
			}
		} else {
			if (lex.peek() == '.' || (visibility & A_destructor)) {
				errs() << CurLoc << ": error in " << ((visibility & A_destructor) ? "destructor" : "method")
				       << " parsing - '" << IdentifierStr << "' is not a known type\n";
				return nullptr;
			}
		}
		if (last_defined_type && !(visibility & (A_constructor | A_destructor)))
			// there is still a destructor/constructor section pending for the last type declaration
			// which is unrelated - let's finish that before we continue with this definition here
			finish_constructors_and_destructor();
	}
	switch ((int)CurTok.kind) {
	case tok_identifier:
		FnName = (visibility & A_destructor) ? ("~" + UnmagledReceiverTypeName) :
			(visibility & A_constructor) ? std::move(UnmagledReceiverTypeName) : std::move(IdentifierStr);
		Kind = 0;
		getNextToken(eSemi);
		break;
	case tok_fn: // closure
		FnName = std::string(createAnonFnName());
		Kind = 0;
		getNextToken(eSemi);
		break;
	case tok_unary:
		getNextToken();
		if (!isascii(CurTok.kind)) {
			errs() << "Expected unary operator\n";
			return nullptr;
		}
		FnName = "unary";
		FnName += (char)CurTok.kind;
		Kind = 1;
		getNextToken();
		break;
	case tok_colon: // ... not really
		getNextToken();
		if (!isascii(CurTok.kind)) {
			errs() << "Expected binary operator\n";
			return nullptr;
		}
		FnName = "binary";
		FnName += (char)CurTok.kind;
		Kind = 2;
		getNextToken();

		// Read the precedence if present.
		if (CurTok.kind == tok_number) {
			if (CurTok.Val.Int < 1 || CurTok.Val.Int > 100) {
				errs() << "Invalid precedence: must be 1..100\n";
				return nullptr;
			}
			getNextToken();
		}
		break;
	default:
		errs() << CurLoc << ": expected function name in prototype - got " << CurTok << "\n";
		return nullptr;
	}
	if (CurTok.kind != '(')
		goto nobrace;
	else
		getNextToken();
	if (CurTok.kind == ')')
		goto noargs;
	for (;;) {
		if (CurTok.kind == tok_ellipsis) {
			isVarArgs = true;
			getNextToken();
			if (CurTok.kind != ')') {
				errs() << CurLoc << ": unexpected '" << CurTok << "' after '...' - ')' expected\n";
				return nullptr;
			}
			break;
		}
		auto ArgLoc = CurLoc;
		auto [name, ft] = ParseTypedIdent(')', false);
		if (!ft)
			return nullptr;
		bool is_black_ident = ((uintptr_t)ft & 1) || name == "_";
		ArgNames.push_back(is_black_ident ? " " : name);
		ArgPos.push_back(ArgLoc);
		auto type = (volvoxc::FullType*)((uintptr_t)(ft) & ~1ULL);
		ArgTypes.push_back(type);
		if (CurTok.kind == ')')
			break;
		Eat(',');
	}
noargs:
	Eat(')', eSemi); //getNextToken(); // eat ')'.
nobrace:
	// parse return type(s)
	volvoxc::FullType* RetType = nullptr;
	SourceLocation retLoc = CurLoc;
	while (CurTok.kind != ';') {
		auto type = ParseType(0, eSemi);
		if (!type) {
			errs() << "error parsing return type of function prototype\n";
			return nullptr;
		} else if (RetType) {
			errs() << "functions returning multiple objecs is not implemented, yet\n";
			return nullptr;
		}
		RetType = type;
	}
	// getNextToken();
	// Verify right number of names for operator.
	if (Kind && ArgNames.size() != Kind) {
		errs() << "Invalid number of operands for operator\n";
		return nullptr;
	}
	if ((visibility & A_destructor) && (RetType || ArgTypes.size() != 1)) {
		errs() << CurLoc << ": definition of '" << FnName << "()' - destructors are not allowed to have ";
		if (ArgTypes.size() != 1) {
			errs() << "arguments";
			if (RetType)
				errs() << " or ";
		}
		if (RetType)
			errs() << "a return value";
		errs() << '\n';
		return nullptr;
	} else if (visibility & A_constructor) {
		if (RetType) {
			if (ArgTypes.size() != 1) {
				errs() << CurLoc << ": definition of '" << FnName << "()' - conversions are not allowed to have arguments and constructors are not allowed to have a return value\n";
				return nullptr;
			}
			visibility = (visibility & ~A_constructor) | A_conversion;
		} else {
			// default constructor - set flag in type
			if (ArgTypes.size() == 1)
				tmp_rec_type->type_attr |= A_constructor;
		}
	}
	if (visibility & (A_constructor | A_destructor))
		visibility |= A_pub;
	return std::make_unique<PrototypeAST>(FnLoc, FnName, ArgNames, visibility, retLoc, Kind != 0, RetType, ArgTypes, ArgPos, isVarArgs);
}


// append prototype to list after checking that it does not already exist
static bool check_and_add_proto(std::vector<std::unique_ptr<PrototypeAST>>& protos, std::unique_ptr<PrototypeAST> Proto,
                                std::string& unmangledName, bool isMethod = false) {
	for (auto& p: protos) {
		if (Proto->Name == p->Name) {
			errs() << Proto->retLoc << (isMethod ? ": method '" : ": function '") << unmangledName
			       << "()' with the same signature has already been defined\n";
			prompt_indent = 0;
			return false;
		}
	}
	protos.push_back(std::move(Proto));
	return true;
}

#define TEST_FN_PREFIX "test_"

/// definition ::= 'fn' prototype expression
std::unique_ptr<FunctionAST> ParseDefinition(unsigned& visibility) {
	if (!(visibility & A_closure)) {
		getNextToken(eSemi); // eat fn.
		if (CurTok.kind == tok_unary && IdentifierStr == "~") {
			visibility |= A_destructor;
			getNextToken(eSemi);
		}
	}
	auto Proto = ParsePrototype(visibility);
	prompt_indent++;
	if (!Proto) {
		prompt_indent = 0;
		return nullptr;
	}
	auto sz = Proto->Args.size();
	// initialize local vars lookup table with function arguments
	for (int i=0; i<sz; i++) {
		FullVar fv = {
			.ft = *Proto->ArgTypes[i]
		};
		bool is_new = locals_table.back().insert(Proto->Args[i].c_str(), fv);
		if (!is_new) {
			errs() << "duplicat function arg '" << Proto->Args[i] << "'\n";
			prompt_indent = 0;
			return nullptr;
		}
	}
	auto ProtoRef = Proto.get();
	std::string unmangledName = Proto->getName();
	if (visibility & A_c_api)
		Proto->Name = unmangledName;
	else if ((visibility & A_conversion) && !(visibility & A_constructor)) {
		std::vector<volvoxc::FullType*> targetType = { Proto->ArgTypes[0], Proto->RetType };
		Proto->Name = Mangle(lex.module->import_path, unmangledName, targetType, Proto->visibility).c_str();
	}
	else
		Proto->Name = Mangle(lex.module->import_path, unmangledName, Proto->ArgTypes, Proto->visibility).c_str();
	if (visibility & A_constructor)
		if (Proto->ArgTypes.size() == 1) // default constructor
			AutoMethods[Proto->ArgTypes[0]->mangled_name].first = Proto->Name;
		else
			Conversions[Proto->Name] = Proto->FT;
	else if (visibility & A_destructor)
		AutoMethods[Proto->ArgTypes[0]->mangled_name].second = Proto->Name;
	if (Proto->visibility & A_method) {
		std::string mangled_receiver_type(Proto->ArgTypes[0]->mangled_name);
		if (!check_and_add_proto(MethodProtos[{mangled_receiver_type, unmangledName}], std::move(Proto), unmangledName, true))
			return nullptr;
	} else if (!strncmp(unmangledName.c_str(), TEST_FN_PREFIX, sizeof(TEST_FN_PREFIX)-1)) {
		if (sz) {
			errs() << Proto->ArgPos[0] << ": 'test_...()' functions must not have any arguments\n";
			prompt_indent = 0;
			return nullptr;
		}
		if (Proto->RetType->type != llvm_bool_type) {
			errs() << Proto->retLoc << ": 'test_...()' functions must return 'bool'\n";
			prompt_indent = 0;
			return nullptr;
		}
		if (!check_and_add_proto(lex.module->FunctionProtos[unmangledName], std::move(Proto), unmangledName))
			return nullptr;
		// 'unmangledName' will not outlive this function so to have a long-lived pointer to
		// the unmangled function name we find the map key's address
		TestFunction = lex.module->FunctionProtos.find(unmangledName)->first.c_str();
	} else {
		if (!check_and_add_proto(lex.module->FunctionProtos[unmangledName], std::move(Proto), unmangledName))
			return nullptr;
	}
	std::pair<std::vector<std::unique_ptr<ExprAST>>, int> Elist = ParseExprList();
	if (!Elist.second && Elist.first.empty())
		return nullptr;
	prompt_indent = 0;
	return std::make_unique<FunctionAST>(ProtoRef, std::move(Elist.first), Elist.second, std::move(unmangledName));
}

std::unique_ptr<ExprAST> GetTopLevelExpression(unsigned sym_kind) {
	if (auto E = ParseExpression()) {
		if (!E->ft || !E->ft->type) {
			if (auto B = dynamic_cast<BinaryExprAST*>(E.get())) {
				if (B->err_msg)
					return AutoErr(B->Loc, B->LHS->ft->type, B->RHS->ft->type, B->LHS->ft->type_attr, B->RHS->ft->type_attr, B->err_msg);
				if (B->opclass == OpDeclAssign) {
					if (!strcmp(B->Op, "::="))
						sym_kind |= A_rvalue;
					if ((comp_mode == comp_jit && !do_test) || (sym_kind & A_globally_visible)) {
						auto uB = std::unique_ptr<BinaryExprAST>(B);
						E.release();
						return HandleGlobalVariable(std::move(uB), sym_kind);
					}
					else
						return E;
				}
				errs() << E->Loc << ' ' << B->Op << ": Cannot evaluate expression\n";
				return nullptr;
			} else {
				errs() << E->Loc << ": indeterminate expression\n";
				return nullptr;
			}
		}
		return E;
	} else {
		return nullptr;
	}
}

std::unique_ptr<FunctionAST> ParseTopLevelExpr(std::unique_ptr<ExprAST> E, bool suppress_output) {
	SourceLocation FnLoc = E->Loc;
	if (comp_mode == comp_jit)
		finishFunctionOrModule();
	// Make an anonymous proto.
	volvoxc::FullType* TheType = lex.get_full_type(have_return ? "int" : "bool");
	auto Proto = std::make_unique<PrototypeAST>(FnLoc, "__anon_expr",
	                                            std::vector<std::string>(),
	                                            A_c_api | A_pub,
	                                            FnLoc, false, TheType);
	std::vector<std::unique_ptr<ExprAST>> ExprList;
	int return_val_idx = -1;
	if (last_shadow_restorer) {
		auto restorer_proto = lex.findProtos(last_shadow_restorer);
		if (!restorer_proto) {
			errs() << "could not find restorer '" << last_shadow_restorer << "'\n";
		} else {
			auto restorer = std::make_unique<FunctionExprAST>(FnLoc, last_shadow_restorer, restorer_proto);
			auto restorer_call = std::make_unique<CallExprAST>(FnLoc, std::move(restorer), std::move(std::vector<std::unique_ptr<ExprAST>>()));
			ExprList.push_back(std::move(restorer_call));
		}
	}
	if (E->ft->type->isVoidTy() || suppress_output || have_return) {
		ExprList.push_back(std::move(E));
		if (!suppress_output && !have_return)
			ExprList.push_back(std::move(std::make_unique<LiteralExprAST>(Token(true))));
		return_val_idx = ExprList.size() - 1;
	} else {
		std::string mangled_println = "_ZN6volvox7printlnEPKcz";
		auto println_proto = lex.findProtos(mangled_println);
		if (!println_proto) {
			errs() << "Fatal error: could not find 'println' function\n";
			return nullptr;
		}
		auto volvox_println = std::make_unique<FunctionExprAST>(FnLoc, mangled_println, println_proto);
		std::vector<std::unique_ptr<ExprAST>> PrintArgs;
		bool is_string = E->ft->type->isPointerTy();
		if (is_string)
			PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token(std::string("\"")))));
		else
			PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token((void*)0))));
		PrintArgs.push_back(std::make_unique<InterfaceExprAST>(std::move(E)));
		// println requires parameters for width, precision and flags - pass 0s (and signed bit) to get defaults
		PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token(0LL))));
		PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token(0LL))));
		PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token(0LL))));
		if (is_string)
			PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token(std::string("\"")))));
		else
			PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token((void*)0))));
		PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token((void*)0))));
		auto print_call = std::make_unique<CallExprAST>(FnLoc, std::move(volvox_println), std::move(PrintArgs));
		ExprList.push_back(std::move(print_call));
	}
	if (last_shadow_saver) {
		auto saver_proto = lex.findProtos(last_shadow_saver);
		if (!saver_proto) {
			errs() << "could not find saver '" << last_shadow_saver << "\n";
		} else {
			auto saver = std::make_unique<FunctionExprAST>(FnLoc, last_shadow_saver, saver_proto);
			auto saver_call = std::make_unique<CallExprAST>(FnLoc, std::move(saver), std::move(std::vector<std::unique_ptr<ExprAST>>()));
			ExprList.push_back(std::move(saver_call));
			ExprList.push_back(std::move(std::make_unique<LiteralExprAST>(Token(true))));
		}
	}
	auto ProtoRef = Proto.get();
	std::string unmangledName = Proto->getName();
	lex.module->FunctionProtos[unmangledName].push_back(std::move(Proto));
	return std::make_unique<FunctionAST>(ProtoRef, std::move(ExprList), tok_return, std::move(unmangledName), return_val_idx);
}

/// external ::= 'extern' prototype
std::unique_ptr<PrototypeAST> ParseExtern(unsigned visibility) {
	getNextToken(eSemi); // eat fn.
	visibility |= (A_extern | A_pub);
	return ParsePrototype(visibility);
}
