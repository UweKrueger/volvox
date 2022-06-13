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

FVListElem* anon_fullvars = nullptr;
FVListElem** anon_fullvars_end = &anon_fullvars;
extern llvm::ExitOnError ExitOnErr;

Token getNextToken(eXpect expect) { return CurTok = lex.gettok(expect); }
Token purgeLine() { return CurTok = lex.purge_line(); }

/// GetTokPrecedence - Get the precedence of the pending binary operator token.
static inline int GetTokPrecedence() {
	return (CurTok.kind < 0 && CurTok.kind > tok_last_op) ? (-CurTok.kind) << 8 : -256;
}

// same as above but take into account that some operators are right binding
static inline int NextTokPrecedence() {
	int prec = GetTokPrecedence();
	// assignments and the invisible operator are right binding
	if (CurTok.kind == tok_assign || CurTok.kind == tok_)
		prec++;
	return prec;
}

static bool Expect(int tok, eXpect expect = eNone) {
	bool res = CurTok.kind == tok;
	if (res) {
		getNextToken(expect);
	} else {
		errs() << CurLoc << ": unexpected '" << CurTok.str() << "' - expected '" << Token::tokName(tok) << "'\n";
	}
	return res;
}

static void Eat(int tok, eXpect expect = eNone) {
	if (CurTok.kind == tok) {
		getNextToken(expect);
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
	// Due to operator precedence rules the tree is stricly left-heavy ## this is wrong ## and can be
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
 */
volvoxc::FullType* ParseType(bool allow_attribute, eXpect expect,
                             const char* tname,
                             std::vector<std::unique_ptr<ExprAST>>* exprs) {
	unsigned attribs = 0;
	std::vector<uint64_t> lens = {};
	while (CurTok.kind != tok_identifier) {
		if (allow_attribute) {
			switch (CurTok.kind) {
			case tok_atomic:
				attribs |= A_atomic;
				break;
			case tok_shared:
				attribs |= A_shared;
				break;
			case tok_iso:
				attribs |= A_iso;
				break;
			case tok_const:
				attribs |= A_const;
				break;
			}
		} else {
			if (CurTok.kind == tok_packed)
				attribs |= A_packed;
		}
		if (CurTok.kind == '&') {
			attribs = (attribs & 0xffff) | ((attribs & 0xffff0000) + 0x10000);
			getNextToken(eType);
			continue;
		}
		if (attribs)
			getNextToken();
		switch (CurTok.kind) {
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
					if (auto e = ParseExpression()) {
						if (exprs) {
							if (!exprs->size() && !Lexer::is_type_start(lex.peek_strict())) {
								// this is a vector - just return elements
								*exprs = SplitExprList(std::move(e));
								errs() << "type: simple array " << exprs->size() << '\n';
								if (!Expect(']', expect)) {
									return nullptr;
								}
								return nullptr;
							} else {
								exprs->push_back(std::move(e));
							}
						} else {
							// we are parsing a pure type - like "[5][][7]f64"
							// in this case the dimensions ("5", "7") must be compile time consts
							// and we can calculate ther values now
							auto VLen = e->codegen();
							if (!VLen) {
								errs() << "cannot generate code for index\n";
								return nullptr;
							}
							errs() << "Type dim: " << *VLen << '\n';
							if (auto Len = llvm::dyn_cast<llvm::ConstantInt>(VLen)) {
								int64_t len = Len->getSExtValue();
								if (len <= 0) {
									errs() << "dimension must be a positive int (not " << len << "\n";
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
			auto elem_type = ParseType();
			if (!elem_type) {
				errs() << CurLoc << ": type expected\n";
				if (exprs)
					*exprs = std::vector<std::unique_ptr<ExprAST>>{};
				return nullptr;
			}
			llvm::Type* array_type = elem_type->type;;
			if (int i = lens.size())
				do
					array_type = llvm::ArrayType::get(array_type, lens[--i]);
				while (i > 0);
			else
				for (int i = exprs->size(); i > 0; i--)
					array_type = llvm::ArrayType::get(array_type, 0);
			return new_FullType(array_type, A_rtlen, nullptr, elem_type);
		}
			break;
		case '{': {
			// struct type
			getNextToken();
			std::vector<std::string> FieldNames;
			std::vector<volvoxc::FullType*> FieldTypes;
			std::vector<llvm::Type*> LLVMFieldTypes;
			for (;;) {
				if (CurTok.kind != tok_identifier) {
					errs () << CurLoc << ": unexpected '" << CurTok.str() << "' in struct declaration - field name expected\n";
					return nullptr;
				}
				FieldNames.push_back(IdentifierStr);
				getNextToken(eType);
				auto type = ParseType(true, eComma);
				if (!type) {
					errs() << CurLoc << ": unexpected '" << CurTok.str() << "' in struct declaration - type name expected\n";
					return nullptr;
				}
				FieldTypes.push_back(type);
				LLVMFieldTypes.push_back(type->type);
				Eat(',');
				if (CurTok.kind == '}')
					break;
			}
			getNextToken(eColon);
			llvm::Type* struct_type = tname ?
				llvm::StructType::create(Context, LLVMFieldTypes, tname, (bool)(attribs & A_packed)) :
				llvm::StructType::get(Context, LLVMFieldTypes, (bool)(attribs & A_packed));
			MapNode* fields = map_string_new_map();
			for (int i=0; i<FieldNames.size(); i++) {
				MapNode* new_node = map_string_tag_insert(&fields, FieldNames[i].c_str(), i, MapValue{ .src_ptr = FieldTypes[i] }, 0, false);
				if (!new_node) {
					errs() << "Duplicate field name '" << FieldNames[i] << "' in struct declaration\n";
					return nullptr;
				}
			}
				
			return new_FullType(struct_type, attribs, nullptr /*DIType*/, (volvoxc::FullType*)fields);
		}
			break;
		default:
			errs() << "Unexpected '" << CurTok.str() << "' - type name expected\n";
			return nullptr;
		}
		getNextToken(expect);
	}
	auto type = type_table.get_full(IdentifierStr.c_str());
	if (!type) {
		errs() << "Unknown type '" << IdentifierStr << "'\n";
		return nullptr;
	}
	getNextToken(expect);
	//if (type.type_attr)
	//	attribs |= A_signed;
	return type;
}

/// numberexpr ::= number
static std::unique_ptr<ExprAST> ParseNumberExpr() {
	auto Result = std::make_unique<LiteralExprAST>(CurTok);
	getNextToken(eBinOp); // consume the number
	return Result;
}

static std::unique_ptr<ExprAST> ParseStringExpr() {
	auto Result = std::make_unique<LiteralExprAST>(CurTok);
	getNextToken(eBinOp); // consume the string
	return Result;
}

static std::unique_ptr<ExprAST> ParsePointerExpr() {
	auto Result = std::make_unique<LiteralExprAST>(CurTok);
	getNextToken(eBinOp); // consume the pointer
	return Result;
}

/// parenexpr ::= '(' expression ')'
static std::unique_ptr<ExprAST> ParseParenExpr() {
	getNextToken(); // eat (.
	auto V = ParseExpression();
	if (!V)
		return nullptr;

	Eat(')', eBinOp);
	return V;
}

/// identifierexpr
///   ::= identifier
///   ::= identifier '(' expression* ')'
static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
	std::string IdName = IdentifierStr;

	SourceLocation LitLoc = CurLoc;

	getNextToken(eBinOp); // eat identifier.
	// first try to find a function with this name
	auto F = FunctionProtos.find(IdName);
	if (F != FunctionProtos.end()) {
		return std::make_unique<FunctionExprAST>(LitLoc, IdName, F->second.get());
	}
	
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
static std::unique_ptr<ExprAST> ParseAggregateExpr() {
	int kind = CurTok.kind;
	// "[3]" can be a fixed array, an index ("a[3]", "f(x)[3]" or "a[4][3]") or
	// part of the type of an aggregate literal ("[3]i32{...}" or [3][3]f64{...}".
	// so spaces before and/or after "[3]" do matter:
	// a [3] [4]f64{...} <---> a[3][4] <---> a[3] [4]f64{...}
	bool is_index = kind == '[' && Lexer::is_expr_end(lex.PreviousChar);
	char closing = '}'; // for dynamic aray, map, set
	bool explicit_type = false;
	llvm::Type* key_type = nullptr;
	bool key_is_signed = false;
	std::vector<std::unique_ptr<ExprAST>> Dims = {};
	std::vector<std::unique_ptr<ExprAST>> Elems = {};
	std::unique_ptr<ExprAST> Len = nullptr;
	volvoxc::FullType* ft = ParseType(false, eBinOp, nullptr, &Dims);
	if (ft)
		errs() << "got type "<< *ft->type << '\n';
	else
		errs() << "got no type, yet\n";
	SourceLocation loc = CurLoc;
	int64_t dim = -1;                       // if size of array is known at compile time
	std::unique_ptr<ExprAST> Init = nullptr;
	std::unique_ptr<ExprAST> Cap = nullptr;
	SourceLocation LenLoc;
	if (ft) {
		if (Dims.size())
			Len = std::move(Dims[0]); // if size can only be determined at run time
		if (!Expect('{'))
		    return nullptr;
		closing = '}';
		switch (ft->type->getTypeID()) {
		case llvm::Type::ArrayTyID:
			key_type = llvm::Type::getInt64Ty(Context);
			break;
		case llvm::Type::StructTyID:
			if (ft->type_attr & A_rtlen) {
				key_type = llvm::Type::getInt64Ty(Context);
				break;
			}
		default:
			errs() << CurLoc << ": " << *ft->type << " as arrgegate type not implemented\n";
			return nullptr;
		}
		if (CurTok.kind == '}') {
			if (!Len) {
				errs() << CurLoc << ": empty initialization requires explicit dimension\n";
				return nullptr;
			}
			getNextToken(eBinOp);
			switch (kind) {
			case '[':
				return std::make_unique<FixedArrayExprAST>(loc, ft, std::vector<std::unique_ptr<ExprAST>>{}, std::move(Len));
			case '{':
			case tok_map:
			case tok_set:
			case tok_chan:
			case tok_identifier: // struct literal
			default:
				abort();
			}
		}
		if (Len && Len->is_compile_time_const) {
			LenLoc = Len->Loc;
			auto Vdim = Len->codegen();
			if (!Vdim) {
				errs() << Len->Loc << ": cannot generate code for index\n";
				return nullptr;
			}
			auto Dim = llvm::cast<llvm::ConstantInt>(Vdim);
			dim = Dim->getSExtValue();
		}
		if (dim >= 0 && ft) {
			llvm::Type* array_type = llvm::ArrayType::get(ft->elem_type->type, dim);
			ft = new_FullType(array_type, 0, nullptr, ft->elem_type);
			errs() << "new ft-type: " << *ft->type << '\n';
		}
		// else: run time sized - dim of literal will be determined by Elements.size()
		explicit_type = true;
		if (CurTok.kind == '}') {
			if (!ft)
				errs() << CurLoc << ": explicit type required for literals with empty initialization list\n";
			getNextToken(eBinOp);
			return std::make_unique<FixedArrayExprAST>(loc, ft, std::vector<std::unique_ptr<ExprAST>>{}, std::move(Len), LenLoc);
		}
		auto Elem = ParseExpression();
		if (!Expect('}', eBinOp))
			return nullptr;
		if (!Elem)
			return nullptr;
		Elems = SplitExprList(std::move(Elem));
	} else {
		Elems = std::move(Dims);
	}
	if (true) {
		std::vector<std::unique_ptr<ExprAST>> Elements = {};
		uint64_t idx = 0;
		errs() << "parsing " << Elems.size() << " elements\n";
		for (auto& elem: Elems) {
			if (auto bin_expr = dynamic_cast<BinaryExprAST*>(elem.get())) {
				if (bin_expr->Op[0] == ':') { // struct or map
					if (auto ident = dynamic_cast<VariableExprAST*>(bin_expr->LHS.get())) {
						if (kind != tok_identifier) { // no struct, i.e. no field name - so it must be a special built-in
							if (ident->Name == "len") {
								LenLoc = bin_expr->RHS->Loc;
								if (kind != '[' && kind != '{') { // no array
									aggr_prop_err(LenLoc, "len", kind);
									return nullptr;
								}
								if (dim >= 0 || Len) {
									aggr_prop_redefinition(LenLoc, "len");
									return nullptr;
								}
								Len = std::move(bin_expr->RHS);
							} else if(ident->Name == "cap") {
								if (kind != '{' && kind != tok_chan) { // neither dynamic array nor channel
									aggr_prop_err(ident->Loc, "cap", kind);
									return nullptr;
								}
								if (Cap) {
									aggr_prop_redefinition(ident->Loc, "cap");
									return nullptr;
								}
								Cap = std::move(bin_expr->RHS);
							} else if(ident->Name == "ini") {
								if (kind != '[' && kind != '{') { // no array
									aggr_prop_err(ident->Loc, "ini", kind);
									return nullptr;
								}
								if (Init) {
									aggr_prop_redefinition(ident->Loc, "ini");
									return nullptr;
								}
								Init = std::move(bin_expr->RHS);
							} else {
								aggr_prop_err(ident->Loc, ident->Name.c_str(), kind);
								return nullptr;
							}
						} else {
							; // handle struct element initializer
						}
					} else {
						// LHS of a:b is a general expression - we require it to be a compile time const
						if (llvm::Value* Key = bin_expr->LHS->codegen()) {
							unsigned keyTID = Key->getType()->getTypeID();
							if (auto key = llvm::dyn_cast<llvm::ConstantInt>(Key)) {
								switch (kind) {
								case '[':
								case '{':
									idx = key->getZExtValue();
									if (idx < Elements.size()) {
										if (Elements[idx]) {
											errs() << elem->Loc << ": value for index " << idx << " already defined\n";
											return nullptr;
										}
										Elements[idx++] = std::move(bin_expr->RHS);
									} else {
										while (Elements.size() < idx)
											Elements.push_back(nullptr);
										Elements.push_back(std::move(bin_expr->RHS));
										idx++;
									}
									break;
								default:
									errs() << aggr_kind_str(kind) << " not implemented, yet\n";
									return nullptr;
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
									return nullptr;
								}
							} else {
								errs() << bin_expr->LHS->Loc << ": key in aggregate literals must be a compile time const\n";
								return nullptr;
							}
						} else {
							errs() << bin_expr->LHS->Loc << ": cannot generate code for key\n";
							return nullptr;
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
			switch (kind) {
			case '[':
			case '{':
				if (idx < Elements.size()) {
					if (Elements[idx]) {
						errs() << elem->Loc << ": value for index " << idx << " already defined\n";
						return nullptr;
					}
					Elements[idx++] = std::move(elem);
				} else {
					while (Elements.size() < idx)
						Elements.push_back(nullptr);
					Elements.push_back(std::move(elem));
					idx++;
				}
				break;
			default:
				errs() << elem->Loc << ": initialization element for " << aggr_kind_str(kind) << " requires \"key:\"\n";
				return nullptr;
			}
		}
		if (Len && Len->is_compile_time_const) {
			auto Vdim = Len->codegen();
			if (!Vdim) {
				errs() << LenLoc << ": cannot generate code for property\n";
				return nullptr;
			}
			auto Dim = llvm::cast<llvm::ConstantInt>(Vdim);
			dim = Dim->getSExtValue();
			auto elem_type = ft->elem_type;
			if (elem_type) {
				if (dim >= 0) {
					llvm::Type* array_type = llvm::ArrayType::get(elem_type->type, dim);
					ft = new_FullType(array_type, 0, nullptr, elem_type);
					errs() << "new ft-type: " << *ft->type << '\n';
				}
			}
		}
		if (ft) {
			if (dim >= 0) {
				if (ft->type->isArrayTy()) {
					errs() << 11 << '\n';
					return std::make_unique<FixedArrayExprAST>(loc, ft, std::move(Elements));
				} else if (auto struct_type = llvm::dyn_cast<llvm::StructType>(ft->type)) {
					errs() << 22 << '\n';
					return std::make_unique<FixedArrayExprAST>(loc, std::move(Elements), ft->elem_type, dim);
				} else {
					errs() << "internal compiler error\n";
					abort();
				}
			} else {
				return std::make_unique<FixedArrayExprAST>(loc, ft, std::move(Elements), std::move(Len), LenLoc);
			}
		} else {
			if (dim >= 0) {
				if ((int64_t)Elements.size() > dim) {
					errs() << LenLoc << ": maximum index of initialization elements (" << Elements.size() - 1 << ") is not lower than given length (" << dim << '\n';
					return nullptr;
				} else {
					return std::make_unique<FixedArrayExprAST>(loc, std::move(Elements), nullptr, dim);
				}
			} else {
				return std::make_unique<FixedArrayExprAST>(loc, std::move(Elements), nullptr, -1, std::move(Len), LenLoc);
			}
		}
		return std::make_unique<FixedArrayExprAST>(loc, std::move(Elements), nullptr, dim);
	} else {
		errs() << "AggregateExpr: unexpected '" << CurTok.str() << "' (expected expression)\n";
		return nullptr;
	}
}

static std::pair<std::vector<std::unique_ptr<ExprAST>>, int> ParseExprList();

/// ifexpr ::= 'if' expression 'then' expression 'else' expression
static std::unique_ptr<ExprAST> ParseIfExpr() {
	SourceLocation IfLoc = CurLoc;

	getNextToken(); // eat the if.

	// condition - expect bool.
	auto Cond = ParseExpression();
	if (!Cond)
		return nullptr;

	if (CurTok.kind != tok_then) {
		errs() << "expected then\n";
		return nullptr;
	}
	getNextToken(); // eat the then

	auto Then = ParseExprList();
	std::pair<std::vector<std::unique_ptr<ExprAST>>, int> Else;
	bool have_else = false;
	if (CurTok.kind == tok_else) {
		have_else = true;
		getNextToken();
		Else = ParseExprList();
	} else {
		Else = { std::vector<std::unique_ptr<ExprAST>>(), 0 };
	}
	if (CurTok.kind != tok_end) {
		errs() << "unexpected token " << CurTok.kind << " (expected " << (have_else ? "'end'\n" : "'else'\n");
		return nullptr;
	}
	getNextToken(eBinOp);
	auto conv = (Else.first.size() && Else.first.back()->ft->type && !Else.first.back()->ft->type->isVoidTy()
	             && Then.first.back()->ft->type && !Then.first.back()->ft->type->isVoidTy()) ?
		convBinOp(Then.first.back()->ft->type, Else.first.back()->ft->type,
		          Then.first.back()->ft->type_attr, Else.first.back()->ft->type_attr,
		          Then.first.back()->is_unknown_type, Else.first.back()->is_unknown_type,
		          "-")
		: BinOpConvSet{{ nullptr, nullptr, llvm::Type::getVoidTy(Context), 0, false, nullptr },
		               { nullptr, nullptr, llvm::Type::getVoidTy(Context), 0, false, nullptr }};
	return std::make_unique<IfExprAST>(IfLoc, std::move(Cond), std::move(Then.first),
	                                   std::move(Else.first), Then.second, Else.second, conv);
}

/// forexpr ::= 'for' identifier '=' expr ',' expr (',' expr)? 'in' expression
static std::unique_ptr<ExprAST> ParseForExpr() {
	getNextToken(); // eat the for.

	if (CurTok.kind != tok_identifier) {
		errs() << "expected identifier after for\n";
		return nullptr;
	}

	std::string IdName = IdentifierStr;
	getNextToken(eBinOp); // eat identifier.

	if (CurTok.kind != tok_assign) {
		errs() << "expected '=' after for\n";
		return nullptr;
	}
	getNextToken(); // eat '='.

	auto Start = ParseExpression();
	if (!Start)
		return nullptr;
	if (CurTok.kind != ',') {
		errs() << "expected ',' after for start value\n";
		return nullptr;
	}
	getNextToken();

	auto End = ParseExpression();
	if (!End)
		return nullptr;

	// The step value is optional.
	std::unique_ptr<ExprAST> Step;
	if (CurTok.kind == ',') {
		getNextToken();
		Step = ParseExpression();
		if (!Step)
			return nullptr;
	}

	if (CurTok.kind != tok_in) {
		errs() << "expected 'in' after for\n";
		return nullptr;
	}
	getNextToken(); // eat 'in'.

	auto Body = ParseExpression();
	if (!Body)
		return nullptr;

	return std::make_unique<ForExprAST>(IdName, std::move(Start), std::move(End),
	                                    std::move(Step), std::move(Body));
}

/// primary
///   ::= identifierexpr
///   ::= numberexpr
///   ::= parenexpr
///   ::= ifexpr
///   ::= forexpr
///   ::= varexpr
static std::unique_ptr<ExprAST> ParsePrimary() {
	switch (CurTok.kind) {
	case tok_eof:
		errs() << "EOF when expecting an expression\n";
		exit(1);
	case tok_identifier:
		return ParseIdentifierExpr();
	case tok_number:
		return ParseNumberExpr();
	case tok_str_lit:
		return ParseStringExpr();
	case tok_ptr_lit:
		return ParsePointerExpr();
	case '(':
		return ParseParenExpr();
	case ')':
		return std::make_unique<EmptyExprAST>();
	case '{':
	case '[':
	case tok_map:
	case tok_set:
	case tok_chan:
		return ParseAggregateExpr();
	case tok_if:
		return ParseIfExpr();
	case tok_for:
		return ParseForExpr();
	default:
		errs() << "unknown token '" << CurTok.kind << "' '" << CurTok.str() << "' when expecting an expression\n";;
		purgeLine();
		return nullptr;
	}
}

/// unary
///   ::= primary
///   ::= '!' unary
static std::unique_ptr<ExprAST> ParseUnary() {
	// If the current token is not an operator, it must be a primary expr.
	if (CurTok.kind != tok_unary)
		return ParsePrimary();
	
	// If this is a unary operator, read it.
	std::string Op = IdentifierStr;
	getNextToken();
	if (auto Operand = ParseUnary())
		return std::make_unique<UnaryExprAST>(Op.c_str(), std::move(Operand));
	return nullptr;
}

/// binoprhs
///   ::= ('+' unary)*
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec, std::unique_ptr<ExprAST> LHS) {
	// If this is a binop, find its precedence.
	while (true) {
		int TokPrec = GetTokPrecedence();
		// If this is a binop that binds at least as tightly as the current binop,
		// consume it, otherwise we are done.
		if (TokPrec <= ExprPrec) {
			return LHS;
		}
		// Okay, we know this is a binop.
		std::string BinOp = IdentifierStr;
		SourceLocation BinLoc = CurLoc;
		getNextToken(); // eat binop
		// Parse the unary expression after the binary operator.
		auto RHS = ParseUnary();
		if (!RHS)
			return nullptr;

		// If BinOp binds less tightly with RHS than the operator after RHS, let
		// the pending operator take RHS as its LHS.
		if (TokPrec <= NextTokPrecedence()) {
			RHS = ParseBinOpRHS(TokPrec, std::move(RHS));
			if (!RHS)
				return nullptr;
		}
		// Merge LHS/RHS.
		// save types befor objects are moved
		auto LHS_type = LHS->ft ? LHS->ft->type : nullptr;
		auto LHS_attr = LHS->ft ? LHS->ft->type_attr : 0;
		auto LHS_is_unknown_type = LHS->is_unknown_type;
		auto RHS_type = RHS->ft ? RHS->ft->type : nullptr;
		auto RHS_attr = RHS->ft ? RHS->ft->type_attr : 0;
		auto RHS_is_unknown_type = RHS->is_unknown_type;
		if (inside_function && BinOp == ":=") {
			if (auto VarL = dynamic_cast<VariableExprAST*>(LHS.get())) {
				auto type_descr = MakeType(RHS_type, RHS_attr & A_signed, RHS_is_unknown_type);
				llvm::Type* type = std::get<0>(type_descr);
				bool is_signed = std::get<2>(type_descr);
				FullVar fv = {
					.val = nullptr,
					.ft = *RHS->ft
				};
				fv.ft.type = type;
				fv.ft.type_attr = is_signed ? 1U : 0U;

				if (!locals_table.back().insert(VarL->Name.c_str(), fv)) {
					errs() << "variable " << VarL->Name << " already exists in current scope\n";
					return nullptr;
				}
			} else {
				errs() << "left operand of \":=\" must be a variable\n";
				return nullptr;
			}
		} else if (LHS_type && LHS_type->isFunctionTy() && (BinOp[0] == '(' || BinOp[0] == '\0')) {
			auto Args = SplitExprList(std::move(RHS));
			LHS = std::make_unique<CallExprAST>(LHS->Loc, std::move(LHS), std::move(Args));
			if (verbosity >= 4)
				dbgs() << "create call expr " << *dynamic_cast<CallExprAST*>(LHS.get())->Callee->ft->type << "\n";
			continue;
		} else if (LHS_type && LHS_type->isAggregateType() && BinOp[0] == '[') {
			LHS = std::make_unique<IndexExprAST>(LHS->Loc, std::move(LHS), std::move(RHS));
			continue;
		}
		LHS = std::make_unique<BinaryExprAST>(BinLoc, BinOp.c_str(), std::move(LHS), std::move(RHS),
		                                      convBinOp(LHS_type, RHS_type, LHS_attr, RHS_attr,
		                                                LHS_is_unknown_type, RHS_is_unknown_type, BinOp.c_str()));
	}
}

/// expression
///   ::= unary binoprhs
///
std::unique_ptr<ExprAST> ParseExpression() {
	auto LHS = ParseUnary();
	if (!LHS)
		return nullptr;

	return ParseBinOpRHS(0, std::move(LHS));
}

static std::pair<std::unique_ptr<ExprAST>, int> ParseExprOrReturn() {
	while (CurTok.kind == ';')
		getNextToken();
	auto kind = CurTok.kind;
	if (kind == tok_return || kind == tok_else || kind == tok_end) {
		if (kind == tok_return) {
			getNextToken(eColon);
			if (CurTok.kind == ';') 
				return { nullptr, kind };
			else
				return { ParseExpression(), kind };
		}
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
						end_kind = tok_leave;
			expr_list.push_back(std::move(expr.first));
		}
	}
	return { std::move(expr_list), end_kind };
}

/// prototype
///   ::= id '(' id* ')'
///   ::= binary LETTER number? (id, id)
///   ::= unary LETTER (id)
static std::unique_ptr<PrototypeAST> ParsePrototype() {
	std::string FnName;

	SourceLocation FnLoc = CurLoc;

	unsigned Kind = 0; // 0 = identifier, 1 = unary, 2 = binary.
	unsigned BinaryPrecedence = 30;
	std::vector<std::string> ArgNames;
	std::vector<volvoxc::FullType*> ArgTypes;
	std::vector<llvm::Type*> LLVMArgTypes;
	std::vector<SourceLocation> ArgPos;
	bool is_method;
	bool isVarArgs = false;

	switch (CurTok.kind) {
	case '(': {
		is_method = true;
		unsigned attribs = 0;
		getNextToken();
		if (CurTok.kind != tok_identifier) {
			errs() << "Unexpected '" << CurTok.str() << "' in method prototype - receiver name expected\n";
			return nullptr;
		}
		ArgNames.push_back(IdentifierStr);
		ArgPos.push_back(CurLoc);
		getNextToken(eType);
		auto type = ParseType(true);
		if (!type->type) {
			errs() << "Unexpected '" << CurTok.str() << "' in method prototype - type name expected\n";
			return nullptr;
		}
		ArgTypes.push_back(type);
		LLVMArgTypes.push_back(type->type);
		Expect(')');
	}
	default:
		is_method = false;
	}
	switch (CurTok.kind) {
	case tok_identifier:
		FnName = IdentifierStr;
		Kind = 0;
		getNextToken();
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
			BinaryPrecedence = (unsigned)CurTok.Val.Int;
			getNextToken();
		}
		break;
	default:
		errs() << "Expected function name in prototype\n";
		return nullptr;
	}
	if (!Expect('(')) return nullptr;
	if (CurTok.kind == ')')
		goto noargs;
	for (;;) {
		if (CurTok.kind != tok_identifier) {
			if (CurTok.kind == tok_ellipsis) {
				isVarArgs = true;
				getNextToken();
				if (CurTok.kind != ')') {
					errs() << "Unexpected '" << CurTok.str() << "' after '...' - ')' expected\n";
					return nullptr;
				}
				else
					break;
			}
			errs() << "Unexpected '" << CurTok.str() << "' in function arg list - arg name expected\n";
			return nullptr;
		}	
		ArgNames.push_back(IdentifierStr);
		ArgPos.push_back(CurLoc);
		getNextToken(eType);
		auto type = ParseType(true);
		if (!type) {
			errs() << "Unexpected '" << CurTok.str() << "' in function arg list - type name expected\n";
			return nullptr;
		}
		ArgTypes.push_back(type);
		LLVMArgTypes.push_back(type->type);
		if (CurTok.kind == ')')
			break;
		Eat(',');
	}
noargs:
	Eat(')', eColon); //getNextToken(); // eat ')'.
	// parse return type(s)
	volvoxc::FullType* RetType = nullptr;
	SourceLocation retLoc = CurLoc;
	while (CurTok.kind != ';') {
		auto type = ParseType(true, eColon);
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

	return std::make_unique<PrototypeAST>(FnLoc, FnName, ArgNames, retLoc, Kind != 0, RetType, ArgTypes, LLVMArgTypes, ArgPos, isVarArgs);
}

#define TEST_FN_PREFIX "test_"

/// definition ::= 'fn' prototype expression
std::unique_ptr<FunctionAST> ParseDefinition() {
	getNextToken(); // eat fn.
	auto Proto = ParsePrototype();
	prompt_indent++;
	if (!Proto) {
		prompt_indent = 0;
		return nullptr;
	}
	auto sz = Proto->Args.size();
	if (!strncmp(Proto->Name.c_str(), TEST_FN_PREFIX, sizeof(TEST_FN_PREFIX)-1)) {
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
		TestFunction = Proto->Name.c_str();
	}
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
	FunctionProtos[Proto->getName()] = std::move(Proto);
	std::pair<std::vector<std::unique_ptr<ExprAST>>, int> Elist = ParseExprList();
	prompt_indent = 0;
	return std::make_unique<FunctionAST>(ProtoRef, std::move(Elist.first), Elist.second);
}

std::unique_ptr<ExprAST> GetTopLevelExpression() {
	if (auto E = ParseExpression()) {
		if (!E->ft || !E->ft->type) {
			if (auto B = dynamic_cast<BinaryExprAST*>(E.get())) {
				if (B->conv.compat.err_msg)
					return AutoErr(B->Loc, B->LHS->ft->type, B->RHS->ft->type, B->LHS->ft->type_attr, B->RHS->ft->type_attr, B->conv.compat.err_msg);
				if (!strcmp(B->Op, ":="))
					return HandleGlobalVariable(B);
				if (!strcmp(B->Op, "="))
					if (auto leftVar = dynamic_cast<VariableExprAST*>(B->LHS.get()))
						if (!leftVar->full_var.first) {
							errs() << "unknown variable name '" << leftVar->getName() << "' - did you mean ':='?\n";
							return nullptr;
						}
				errs() << E->Loc << ' ' << B->Op << ": Cannot evaluate expression\n";
				return nullptr;
			} else {
				errs() << E->Loc << ": Cannot deduce type of expression\n";
				if (E->ft)
					E->ft->dump();
				return nullptr;
			}
		}
		return E;
	} else {
		return nullptr;
	}
}

std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
	SourceLocation FnLoc = CurLoc;
	if (auto E = GetTopLevelExpression()) {
		if (comp_mode == comp_jit) {
#if LLVM_VERSION_MAJOR >= 12
			ExitOnErr(TheJIT->addModule(
				          llvm::orc::ThreadSafeModule(std::move(TheModule), TS_Context)));
#else
			TheJIT->addModule(std::move(TheModule));
#endif
			InitializeModuleAndPassManager();
		}
		// Make an anonymous proto.
		volvoxc::FullType* TheType = type_table.get_full("bool");
		auto Proto = std::make_unique<PrototypeAST>(FnLoc, "__anon_expr",
		                                            std::vector<std::string>(),
		                                            FnLoc, false, TheType);
		std::vector<std::unique_ptr<ExprAST>> ExprList;
		if (last_shadow_restorer) {
			auto restorer_proto = FunctionProtos.find(last_shadow_restorer);
			if (restorer_proto == FunctionProtos.end()) {
				errs() << "could not find restorer '" << last_shadow_restorer << "'\n";
			} else {
				auto restorer = std::make_unique<FunctionExprAST>(FnLoc, last_shadow_restorer, restorer_proto->second.get());
				auto restorer_call = std::make_unique<CallExprAST>(FnLoc, std::move(restorer), std::move(std::vector<std::unique_ptr<ExprAST>>()));
				ExprList.push_back(std::move(restorer_call));
			}
		}
		if (E->ft->type->isVoidTy()) {
			ExprList.push_back(std::move(E));
			ExprList.push_back(std::move(std::make_unique<LiteralExprAST>(Token(true))));
		} else {
			llvm::Constant* rttype_ptr = getRtType(E->ft);
			if (E->ft->type->isAggregateType()) {
				// pass by reference
				E = std::make_unique<UnaryExprAST>("&", std::move(E));
			}
			std::string mangled_println = "_ZN6volvox7printlnEPKcPKNS_6RtTypeEz";
			auto println_proto = FunctionProtos.find(mangled_println);
			if (println_proto == FunctionProtos.end()) {
				errs() << "Fatal error: could not find 'println' function\n";
				return nullptr;
			}
			auto volvox_println = std::make_unique<FunctionExprAST>(FnLoc, mangled_println, println_proto->second.get());
			std::vector<std::unique_ptr<ExprAST>> PrintArgs;
			bool is_string = E->ft->type->isPointerTy();
			if (is_string)
				PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token(std::string(E->ft->type->isPointerTy() ? "\"" : "")))));
			else
				PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token((void*)0))));
			PrintArgs.push_back(std::move(std::make_unique<ConstExprAST>(rttype_ptr)));
			// println requires parameters for width, precision and flags - pass 0s (and signed bit) to get defaults
			PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token(0LL))));
			PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token(0LL))));
			PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token(0LL))));
			PrintArgs.push_back(std::move(E));
			if (is_string)
				PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token(std::string("\"")))));
			else
				PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token((void*)0))));
			PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token((void*)0))));
			auto print_call = std::make_unique<CallExprAST>(FnLoc, std::move(volvox_println), std::move(PrintArgs));
			ExprList.push_back(std::move(print_call));
		}
		if (last_shadow_saver) {
			auto saver_proto = FunctionProtos.find(last_shadow_saver);
			if (saver_proto == FunctionProtos.end()) {
				errs() << "could not find saver '" << last_shadow_saver << "\n";
			} else {
				auto saver = std::make_unique<FunctionExprAST>(FnLoc, last_shadow_saver, saver_proto->second.get());
				auto saver_call = std::make_unique<CallExprAST>(FnLoc, std::move(saver), std::move(std::vector<std::unique_ptr<ExprAST>>()));
				ExprList.push_back(std::move(saver_call));
				ExprList.push_back(std::move(std::make_unique<LiteralExprAST>(Token(true))));
			}
		}
		auto ProtoRef = Proto.get();
		FunctionProtos[Proto->getName()] = std::move(Proto);
		auto tmp_function = std::make_unique<FunctionAST>(ProtoRef, std::move(ExprList), tok_return);
		return tmp_function;
	}
	return nullptr;
}

/// external ::= 'extern' prototype
std::unique_ptr<PrototypeAST> ParseExtern() {
	getNextToken(); // eat extern.
	return ParsePrototype();
}
