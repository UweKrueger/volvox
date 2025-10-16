/*
 * Copyright © Uwe Krüger 2021, 2022, 2023, 2024, 2025
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include "../include/volvox.hh"

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

//                     vtable_t                                  method name             prototype                        embedded interfaces
std::vector<std::tuple<llvm::ArrayType*,std::unique_ptr<std::map<std::string,std::vector<std::unique_ptr<PrototypeAST>>>>,std::vector<volvoxc::FullType*>>> InterfaceProtos;

// methods table - keys: { mangled_type_name, method_name }
std::map<std::pair<std::string,std::string>, std::vector<std::unique_ptr<PrototypeAST>>> MethodProtos;
std::vector<std::unique_ptr<PrototypeAST>>* int_int_proto = nullptr;
FVListElem* anon_fullvars = nullptr;
FVListElem** anon_fullvars_end = &anon_fullvars;
ProtoListElem* anon_protos = nullptr;
ProtoListElem** anon_protos_end = &anon_protos;

// Syntactically 'elif' does not introduce more nesting than 'else' - however,
// semantically it does. Keep track of syntactic nesting delta for each semantic level.
// Used to find merge point for multi level 'brk'
std::vector<uint8_t> syntax_nesting;

extern llvm::ExitOnError ExitOnErr;
bool parseOk = true;

// branches may be be nested and each nesting level may have several parts separated by 'brk'
// we keek track, where we are. Currently it is only used to know *if* we are inside a branch
// so the implementation is too sophisticated - let's keep it though for debugging purposes
//
std::vector<branch_part_t> current_branch_part;

return_kind_t function_return_kind = return_expr; // main function return status value

bool LogicalLocation::operator>(const LogicalLocation& other) const {
	if (Pos <= other.Pos)
		return false;
	size_t min_depth = (Branch.size() < other.Branch.size()) ? Branch.size() : other.Branch.size();
	for (int i=0; i<min_depth; i++) {
		if (Branch[i].conditional > other.Branch[i].conditional)
			return true;
		if (Branch[i].branch != other.Branch[i].branch)
			return false;
	}
	return true;
}

Token& getNextToken(eXpect expect, int terminator) {
	CurTok = lex.gettok(expect, terminator);
	if (CurTok.kind == tok_error)
		parseOk = false;
	return CurTok;
}

Token& purgeLine() {
	parseOk = true;
	CurTok = lex.purge_line();
	// clean up global parser states
	merge_points.clear();
	current_branch_part.clear();
	syntax_nesting.clear();
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
static std::unique_ptr<PrototypeAST> ParsePrototype(unsigned& visibility);

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
								errs() << "dimension must be a constexpr size_t value\n";
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
			return new_FullType(array_type, attribs, nullptr, nullptr, elem_type);
		}
			break;
		case '{': {
			// struct type
			getNextToken();
			std::vector<std::string> FieldNames;
			std::vector<FieldTypeLoc> FieldTypes;
			std::vector<llvm::Type*> LLVMFieldTypes;
			for (;;) {
				auto field_Loc = CurLoc;
				auto [name, ft] = ParseTypedIdent('}', true);
				if (!ft)
					return nullptr;
				FieldNames.push_back(name);
				FieldTypeLoc type = {
					.ft = (volvoxc::FullType*)((uintptr_t)(ft) & ~1ULL),
					.Loc = field_Loc
				};
				FieldTypes.push_back(type);
				LLVMFieldTypes.push_back(type.ft->type);
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
				MapNode* new_node = map_string_tag_insert(&fields, FieldNames[i].c_str(), i, MapValue{ .src_ptr = &FieldTypes[i] }, sizeof(FieldTypeLoc), &replace);
				if (replace) {
					errs() << CurLoc << ": duplicate field name '" << FieldNames[i] << "' in struct declaration\n";
					return nullptr;
				}
			}
			return new_FullType(struct_type, attribs, nullptr /*DIType*/, fields);
		}
			break;
		case tok_map:
		case tok_vec:
		case tok_set: {
			auto typeTok = CurTok.kind;
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
			volvoxc::FullType* ft;
			if (typeTok == tok_map || typeTok == tok_set) {
				auto ftpair = new_FullType(*key_ft, 0, 1); // reserve space for 1 additional FullType
				volvoxc::FullType* val_ft;
				if (typeTok == tok_map) {
					val_ft = ParseType(0, expect, terminator);
					if (!val_ft) {
						errs() << KeyLoc << ": type (of map value) expected\n";
						return nullptr;
					}
					ftpair[1] = *val_ft;
				} else
					ftpair[1] = volvoxc::FullType{0};
				ft = new_FullType(llvm_ptr_type, A_map | attribs, nullptr, nullptr, ftpair);
			} else {
				ft = new_FullType(*vec_type, attribs);
				ft->elem_type = key_ft;
			}
			return ft;
		}
		case tok_func: {
			unsigned visibility = A_closure;
			auto Loc = CurLoc;
			auto proto = ParsePrototype(visibility);
			auto ft = new_FullType(proto->FT, 0);
			ft->Protos = new_AnonProto(std::move(proto), Loc);
			ft->mangled_name = "4func"; // TODO: mangle function type
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
	if (attribs != type->type_attr)
		type = new_FullType(*type, attribs);
	if (is_ptr) {
		llvm::Type* ptr_type = llvm_ptr_type;
		type = new_FullType(ptr_type, 0, nullptr, nullptr, type);
	}
	return type;
}

// parse argument of function prototype or or element in struct declaration
// typically something like "x type[,)}\n]" - 'x' can be omitted in which case
// the type name is used as name and the lowest bit of FullType* is set to
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

/// numberexpr := number
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

static std::unique_ptr<ExprAST> ParseInterpolatedStringExpr(int terminator = 0) {
	std::vector<char*> str_parts;
	std::vector<std::tuple<std::unique_ptr<ExprAST>,std::unique_ptr<ExprAST>,std::unique_ptr<ExprAST>,unsigned>> interpolations;
	SourceLocation Loc = CurLoc;
	str_parts.push_back(CurTok.Val.CStr); // move pointer value
	CurTok.Val.CStr = nullptr; // prevent ~Token() from free()ing
	do {
		std::unique_ptr<ExprAST> expr = nullptr;
		std::unique_ptr<ExprAST> w = nullptr;
		std::unique_ptr<ExprAST> p = nullptr;
		unsigned flags = 0;
		// for parsing the format specifiers we must think in terms of characters - not tokens
		// so we use the Lexer directly
		while (lex.CurChar != '{' && lex.CurChar != '_' && !isalpha(lex.CurChar)) {
			if (lex.CurChar == '#') {
				if (flags & (FMT_ALT | FMT_CSV)) {
					errs() << lex.Loc << ": at most 1 format specifier out of '#' and ',' allowed\n";
					goto handle_error;
				}
				flags |= FMT_ALT;
			} else if (lex.CurChar == ',') {
				if (flags & (FMT_ALT | FMT_CSV)) {
					errs() << lex.Loc << ": at most 1 format specifier out of '#' and ',' allowed\n";
					goto handle_error;
				}
				flags |= FMT_CSV;
			} else if (lex.CurChar == '0') {
				if (flags & FMT_ZEROPAD) {
					errs() << lex.Loc << ": at most 1 format specifier '" << (char)lex.CurChar << "' allowed\n";
					goto handle_error;
				}
				flags |= FMT_ZEROPAD;
			} else if (lex.CurChar == ' ' || lex.CurChar == '+') {
				if (flags & (FMT_PREFIX_SPACE | FMT_PREFIX_PLUS)) {
					errs() << lex.Loc << ": at most 1 format specifier out of ' ' and '+' allowed\n";
					goto handle_error;
				}
				flags |= (lex.CurChar == ' ' ? FMT_PREFIX_SPACE : FMT_PREFIX_PLUS);
			} else if (lex.CurChar == '\'') {
				if (flags & FMT_GROUPED) {
					errs() << lex.Loc << ": at most 1 format specifier '" << (char)lex.CurChar << "' allowed\n";
					goto handle_error;
				}
				flags |= FMT_GROUPED;
// The following are Volvox specific - in C they are handle by the conversion letter 'd', 'x', ...
			} else if (lex.CurChar == '!') {
				if (flags & FMT_UPPER) {
					errs() << lex.Loc << ": at most 1 format specifier '" << (char)lex.CurChar << "' allowed\n";
					goto handle_error;
				}
				flags |= FMT_UPPER;
			} else if (lex.CurChar == '%' || lex.CurChar == '~') {
				if (flags & (FMT_DISPLAY_HEX | FMT_DISPLAY_OCT)) {
					errs() << lex.Loc << ": at most 1 format specifier out of '%' and '~' allowed\n";
					goto handle_error;
				}
				flags |= (lex.CurChar == '%' ? FMT_DISPLAY_HEX : FMT_DISPLAY_OCT);
			} else if (lex.CurChar == '.' || lex.CurChar == '^') {
				if (flags & (FMT_DISPLAY_EXP | FMT_DISPLAY_FIXED)) {
					errs() << lex.Loc << ": at most 1 format specifier out of '.' and '^' allowed\n";
					goto handle_error;
				}
				flags |= (lex.CurChar == '.' ? FMT_DISPLAY_FIXED : FMT_DISPLAY_EXP);
			} else if (lex.CurChar == '`') {
				if (flags & (FMT_DISPLAY_EXP | FMT_DISPLAY_FIXED | FMT_DISPLAY_HEX | FMT_DISPLAY_OCT| FMT_CHAR)) {
					errs() << lex.Loc << ": '`' invalid since '^', '.', '%', '0' or a previous '`' has already been specified\n";
					goto handle_error;
				}
				flags |= FMT_CHAR;
			} else {
				errs() << lex.Loc << ": unexpected string interpolation specifier '" << (char)lex.CurChar << "'\n";
				goto handle_error;
			}
			lex.CurChar = lex.advance();
		}
		if (lex.CurChar == '{') {
			lex.CurChar = lex.advance();
			getNextToken();
			auto exprs = ParseExpression('}');
			if (!exprs)
				goto handle_error;
			if (CurTok.kind != '}') {
				errs() << CurLoc << ": '}' expected at end of string interpolation expression\n";
				goto handle_error;
			}
			auto expr_list = SplitExprList(std::move(exprs));
			expr = std::move(expr_list[0]);
			if (expr_list.size() > 1) {
				w = std::move(expr_list[1]);
				if (expr_list.size() > 2) {
					p = std::move(expr_list[2]);
					if (expr_list.size() > 3) {
						errs() << expr_list[3]->Loc << ": unexpected 4th interpolation expression - expected $*{value[,width[,precision]]}\n";
						goto handle_error;
					}
				}
			}
		} else {
			std::string VarName;
			CurLoc = lex.Loc;
			do {
				VarName += lex.CurChar;
				lex.CurChar = lex.advance();
			} while (lex.CurChar == '_' || isalnum(lex.CurChar));
			expr = std::make_unique<VariableExprAST>(CurLoc, VarName);
			if (!expr->ft->type) {
				errs() << CurLoc << ": unknown variable name '" << VarName << "'\n";
				goto handle_error;
			}
		}
		CurTok = lex.get_str_tok('"');
		interpolations.push_back({ std::move(expr), std::move(w), std::move(p), flags });
		str_parts.push_back(CurTok.Val.CStr); // move pointer value
		CurTok.Val.CStr = nullptr; // prevent ~Token() from free()ing
	} while (CurTok.kind == tok_part_str_lit);
	getNextToken(eBinOp);
	return std::make_unique<InterpStrLitExprAST>(
		Loc, std::move(str_parts), std::move(interpolations));
handle_error:
	purgeLine();
	return nullptr;
}

static std::unique_ptr<ExprAST> ParsePointerExpr(int terminator = 0) {
	auto Result = std::make_unique<LiteralExprAST>(std::move(CurTok));
	getNextToken(eBinOp, terminator); // consume the pointer
	return Result;
}

/// parenexpr := '(' expression ')'
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
///   := identifier
///   := identifier '(' expression* ')'
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
		if (im->second.isPrefix()) {
			return std::make_unique<ModuleExprAST>(LitLoc, im->second.Loc, std::move(IdName));
		}
	// or a type name
	if (auto ft = lex.get_full_type(IdName.c_str())) {
		if (CurTok.kind == '{') {
			if (auto s = ParseStructExpr(ft, terminator))
				return s;
		}
		else if (CurTok.kind == ';'
		         || CurTok.kind < 0
		         && CurTok.kind > tok_selector
		         && CurTok.kind != tok_colon
		         && CurTok.kind != tok_invisible)
			if (llvm::isa<llvm::StructType>(ft->type)) {
				auto list = std::make_unique<ListExprAST>(LitLoc);
				return std::make_unique<StructExprAST>(LitLoc, ft, std::move(list));
			}
		return std::make_unique<TypeExprAST>(LitLoc, std::move(IdName), ft);
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
	case tok_vec:
		return "vec";
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
	bool is_set = CurTok.kind == tok_set;
	bool is_map = CurTok.kind == tok_map;
	bool is_vec = CurTok.kind == tok_vec;
	if ((is_set || is_map || is_vec) && lex.peek() == '{') {
		if (is_vec) {
			ft = new_FullType(*vec_type, 0);
		} else {
			ft = new_FullType(llvm_ptr_type, A_map);
		}
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
			LenLocs.push_back(dim ? dim->Loc : SourceLocation());
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
			if (ft->type == llvm_vec_type) {
				auto vec_ast = std::make_unique<VecExprAST>(loc, ft, std::move(init_list->Elements));
				if (!vec_ast->ft || !vec_ast->ft->type)
					return nullptr;
				return vec_ast;
			} else {
				errs() << CurLoc << ": internal error - struct literal\n";
				return nullptr;
			}
		case llvm::Type::PointerTyID:
			if (ft->type_attr & A_map) {
				std::unique_ptr<ExprAST> map_ast;
				if (is_set)
					map_ast = std::make_unique<SetExprAST>(loc, ft, std::move(init_list->Elements));
				else
					map_ast = std::make_unique<MapExprAST>(loc, ft, std::move(init_list->Elements));
				if (!map_ast->ft || !map_ast->ft->type)
					return nullptr;
				return map_ast;
			}
			return nullptr;
		default:
			errs() << CurLoc << ": " << *ft->type << " as arrgegate type not implemented\n";
			return nullptr;
		}
		if (!init_list)
			return nullptr;
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
									errs() << bin_expr->LHS->Loc << ": index '" << idx << "' invalid (should be > " << Elements.size()-1 << " - indices must be in ascending order)\n";
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

// return list, end_kind, brk_level
static std::tuple<std::vector<std::unique_ptr<ExprAST>>, int, unsigned> ParseExprList();

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

static std::tuple<std::vector<BranchDescription>,std::set<std::string>,VarTable,bool,bool,int,unsigned> ParseElse(
	VarTable& then_locals_table, SourceLocation& Loc, TokenKind kind, int ThenEndkind);

std::map<std::string,FullVar*> get_destruct_vars(int b_lev) {
	std::map<std::string,FullVar*> destr_vars;
	int sz = locals_table.size();
	for (int n=b_lev; n>0; n--) {
		auto& table = locals_table[sz-n];
		for (auto t = table.first(); (bool)t; ++t) {
			FullVar* fullV = fullVar(t);
			if (fullV->ft.type_attr & (A_destructor | A_string | A_map)) {
				std::string key(t.getKey());
				destr_vars.insert({ std::move(key), fullV });
			}
		}
	}
	return destr_vars;
}

void get_destruct_vars_main(std::map<std::string,FullVar*>& destr_vars) {
	auto& table = lex.module->globals_table;
	for (auto t = table.first(); (bool)t; ++t) {
		FullVar* fullV = fullVar(t);
		if (fullV->ft.type_attr & (A_destructor | A_string | A_map)) {
			std::string key(t.getKey());
			// errs() << key << " ";
			destr_vars.insert({ std::move(key), fullV });
		}
	}
}

std::map<std::string,FullVar*> get_destruct_vars_main() {
	std::map<std::string,FullVar*> destr_vars;
	get_destruct_vars_main(destr_vars);
	return destr_vars;
}

/// if..., elif..., while...[elif...]else...end, repeat...until
static std::unique_ptr<ExprAST> ParseIfExpr(int terminator = 0) {
	SourceLocation IfLoc = CurLoc;
	auto kind = TokenKind(CurTok.kind); // to remember if it's 'if', 'while' or 'repeat'
	getNextToken(); // eat the if/while.
	// condition - expect bool.
	std::unique_ptr<ExprAST> Cond;
	syntax_nesting.push_back(kind == tok_elif ? 0 : 1);
	if (kind == tok_if || kind == tok_elif || kind == tok_while) {
		Cond = ParseCondition(kind);
		if (!Cond)
			return nullptr;
	}
	locals_table.emplace_back();
	current_branch_part.push_back(branch_part_t{0});
	unsigned max_brk_level = 0;
	std::vector<BranchDescription> Then;
	for (;;) {
		auto [list, e_kind, level] = ParseExprList();
		if (level > max_brk_level)
			max_brk_level = level;
		bool is_brk = ~(~e_kind & ((1<<16)-1)) == tok_brk;
		unsigned b_lev = is_brk ? ((~e_kind) >> 16) // semantic level - raw handling has been done in ParseExprList()
			: (e_kind == tok_return) ? (unsigned)locals_table.size() : 1;
		std::map<std::string,FullVar*> destr_vars = get_destruct_vars(b_lev);
		if (e_kind == tok_return && !inside_function)
			get_destruct_vars_main(destr_vars);
		Then.push_back({ std::move(list), BreakDescription{
					.vars_to_destruct = std::move(destr_vars),
					.end_kind = e_kind,
					.break_level = b_lev
				} });
		if (!is_brk)
			break;
		current_branch_part.back().brk_part++;
	}
	if (!Then.back().second.end_kind && Then.back().first.empty()) {
		errs() << CurLoc << ": malformed branch expression\n";
		return nullptr;
	}
	VarTable then_locals_table = std::move(locals_table.back());
	locals_table.pop_back();
	auto [Else, merged_vars, else_locals_table, have_else, success, then_end_kind, else_level] = ParseElse(then_locals_table, IfLoc, kind, Then.back().second.end_kind);
	if (else_level > max_brk_level)
		max_brk_level = else_level;
	if (!success)
		return nullptr;
	if (kind == tok_repeat) {
		if (!Expect(tok_until))
			return nullptr;
		Cond = ParseCondition(kind, terminator);
		if (!Cond)
			return nullptr;
	} else {
		if (have_else && Else.back().second.end_kind == tok_end && then_end_kind != tok_elif || !have_else && Then.back().second.end_kind == tok_end)
			if (!Expect(tok_end, eBinOp)) {
				errs() << CurLoc << ": 'end' expected\n";
				return nullptr;
			}
	}
	bool always_return = Then.size() == 1 && Then.back().second.end_kind == tok_return && have_else && Else.size() == 1 && Else.back().second.end_kind == tok_return;
	auto res_t = ((kind == tok_if || kind == tok_elif) && Then.size() == 1 && Else.size() == 1 &&
	              Else.back().first.size() && Else.back().first.back()->ft->type &&
	              !Else.back().first.back()->ft->type->isVoidTy() && Then.back().first.back()->ft->type &&
	              !Then.back().first.back()->ft->type->isVoidTy() &&
	              !(Then.back().second.end_kind == tok_return || have_else && Else.back().second.end_kind == tok_return)) ?
		getResType(Then.back().first.back()->ft->type, Else.back().first.back()->ft->type, "if",
		           Then.back().first.back()->ft->type_attr, Else.back().first.back()->ft->type_attr,
		           Then.back().first.back()->is_unknown_type, Else.back().first.back()->is_unknown_type)
		: std::tuple<llvm::Type*, unsigned, bool, OpClass, const char*>{ llvm::Type::getVoidTy(Context),
		                                                                 0, false, OpNormal, nullptr };
	syntax_nesting.pop_back();
	return std::make_unique<IfExprAST>(IfLoc, std::move(Cond), std::move(Then), std::move(Else),
	                                   std::move(then_locals_table), std::move(else_locals_table), max_brk_level,
	                                   std::move(merged_vars),
	                                   res_t, kind == tok_elif ? tok_if : kind, always_return);
}

static std::tuple<std::vector<BranchDescription>,std::set<std::string>,VarTable,bool,bool,int,unsigned> ParseElse(
	VarTable& then_locals_table, SourceLocation& Loc, TokenKind kind, int ThenEndkind)
{
	std::vector<BranchDescription> Else;
	std::set<std::string> merged_vars;
	int then_end_kind;
	bool have_else = false;
	unsigned max_brk_level = 0;
	if (ThenEndkind == tok_else || ThenEndkind == tok_elif) {
		then_end_kind = ThenEndkind;
		have_else = true;
		if (ThenEndkind != tok_elif)
			getNextToken();
	} else if (ThenEndkind == tok_return) {
		while (CurTok.kind == ';')
			getNextToken();
		then_end_kind = CurTok.kind;
		if (CurTok.kind == tok_end) {
			getNextToken();
			have_else = false;
		} else if (CurTok.kind == tok_else || CurTok.kind == tok_elif) {
			if (CurTok.kind != tok_elif)
				getNextToken();
			have_else = true;
		} else {
			errs() << CurLoc << ": 'else', 'elif' or 'end' expected (branch has returned unconditionally so any statement would be dead code)\n";
			std::vector<BranchDescription> ret_vec;
			ret_vec.push_back({ std::vector<std::unique_ptr<ExprAST>>(), BreakDescription{0} });
			return { std::move(ret_vec), std::move(merged_vars), VarTable{}, false, false, then_end_kind, max_brk_level };
		}
	}
	if (have_else) {
		if (kind == tok_repeat) {
			errs() << CurLoc << ": 'else' not allowed with 'repeat'\n";
			std::vector<BranchDescription> ret_vec;
			ret_vec.push_back({ std::vector<std::unique_ptr<ExprAST>>(), BreakDescription{0} });
			return { std::move(ret_vec), std::move(merged_vars), VarTable{}, false, false, then_end_kind, max_brk_level };
		}
		locals_table.emplace_back();
		current_branch_part.back() = branch_part_t{
			.branch = 1 // else branch
		};
		if (CurTok.kind == tok_elif) {
			// TODO: modify elif counter vector
			auto elif_expr = ParseIfExpr();
			auto elifif_expr = dynamic_cast<IfExprAST*>(elif_expr.get());
			if (!elifif_expr) {
				errs() << CurLoc << ": invalid 'if ... elif' structure\n";
				std::vector<BranchDescription> ret_vec;
				ret_vec.push_back({ std::vector<std::unique_ptr<ExprAST>>(), BreakDescription{0} });
				return { std::move(ret_vec), std::move(merged_vars), VarTable{}, false, false, then_end_kind, 0 };
			}
			elifif_expr->is_elif_branch = true;
			if (elifif_expr->max_brk_level > max_brk_level)
				max_brk_level = elifif_expr->max_brk_level - 1;
			auto end_k = elifif_expr->always_return ? tok_return : tok_end;
			std::vector<std::unique_ptr<ExprAST>> l;
			l.push_back(std::move(elif_expr));
			unsigned b_lev = elifif_expr->always_return ? 0 : 1;
			std::map<std::string,FullVar*> destr_vars = get_destruct_vars(b_lev);
			Else.push_back({ std::move(l), BreakDescription{
						.vars_to_destruct = std::move(destr_vars),
						.end_kind = end_k,
						.break_level = b_lev
					} });
		} else {
			for (;;) {
				auto [list, e_kind, level] = ParseExprList();
				if (level > max_brk_level)
					max_brk_level = level;
				bool is_brk = ~(~e_kind & ((1<<16)-1)) == tok_brk;
				unsigned b_lev = is_brk ? ((~e_kind) >> 16)
					: (e_kind == tok_return) ? (unsigned)locals_table.size() : 1;
				std::map<std::string,FullVar*> destr_vars = get_destruct_vars(b_lev);
				if (e_kind == tok_return && !inside_function)
					get_destruct_vars_main(destr_vars);
				Else.push_back({ std::move(list), BreakDescription{
							.vars_to_destruct = std::move(destr_vars),
							.end_kind = e_kind,
							.break_level = b_lev
						} });
				if (~(~Else.back().second.end_kind & ((1<<16)-1)) != tok_brk)
					break;
				current_branch_part.back().brk_part++;
			}
			if (!Else.back().second.end_kind && Else.back().first.empty()) {
				errs() << CurLoc << ": invalid 'if ... else' structure\n";
				std::vector<BranchDescription> ret_vec;
				ret_vec.push_back({ std::vector<std::unique_ptr<ExprAST>>(), BreakDescription{0} });
				return { std::move(ret_vec), std::move(merged_vars), VarTable{}, false, false, then_end_kind, 0 };
			}
			if (Else.back().second.end_kind == tok_return) {
				while (CurTok.kind == ';')
					getNextToken();
				if (CurTok.kind == tok_end) {
					getNextToken();
				} else {
					errs() << CurLoc << ": 'end' expected\n";
					std::vector<BranchDescription> ret_vec;
					ret_vec.push_back({ std::vector<std::unique_ptr<ExprAST>>(), BreakDescription{0} });
					return { std::move(ret_vec), std::move(merged_vars), VarTable{}, false, false, then_end_kind, 0 };
				}
			}
		}
	} else {
		Else.push_back({ std::vector<std::unique_ptr<ExprAST>>(), BreakDescription{0} });
	}
	current_branch_part.pop_back();
	if (!current_branch_part.empty())
		current_branch_part.back().conditional++; // to distinguist from other BranchExprs in enclosing level
	VarTable else_locals_table = have_else ? std::move(locals_table.back()) : VarTable();
	if (kind == tok_repeat) {
		if (then_locals_table.table) {
			for (auto then_node = then_locals_table.first(); then_node; ++then_node) {
				auto then_var = fullVar(then_node);
				// only add vars declared before 1st 'brk' to outer scope
				if (!then_var->branch_parts->back().brk_part) {
					if (then_var->ft.type_attr & (A_destructor | A_string | A_map)) {
						std::string then_var_name(then_node.getKey());
						merged_vars.insert(std::move(then_var_name));
					}
					bool success = false;
					if (locals_table.empty()) {
						if (auto fv = lex.module->globals_table.insert(then_node.getKey(), *then_var)) {
							fv->ft.type_attr |= A_mainvar;
							success = true;
							fv->branch_parts = new std::vector<branch_part_t>(*then_var->branch_parts);
							fv->branch_parts->pop_back();
						}
					} else {
						auto new_then_var = *then_var;
						new_then_var.branch_parts = new std::vector<branch_part_t>(*then_var->branch_parts);
						new_then_var.branch_parts->pop_back();
						if (!locals_table.back().insert(then_node.getKey(), new_then_var)) {
							errs() << Loc << ": Variable '" << then_node.getKey() << "' already exists in outer scope\n";
							std::vector<BranchDescription> ret_vec;
							ret_vec.push_back({ std::vector<std::unique_ptr<ExprAST>>(), BreakDescription{0} });
							return { std::move(ret_vec), std::move(merged_vars), VarTable{}, false, false, then_end_kind, 0 };
						}
					}
				}
			}
		}
	} else if (have_else) {
		locals_table.pop_back();
		if (then_locals_table.table && else_locals_table.table) {
			for (auto then_node = then_locals_table.first(); then_node; ++then_node) {
				FullVar* else_var = else_locals_table[then_node.getKey()];
				// only add to outer scope if declared before 1st 'brk' in each branch
				FullVar* then_var = fullVar(then_node);
				if (else_var && !then_var->branch_parts->back().brk_part && !else_var->branch_parts->back().brk_part) {
					if (then_var->ft.type_attr & (A_destructor | A_string | A_map)) {
						std::string then_var_name(then_node.getKey());
						merged_vars.insert(std::move(then_var_name));
					}
					bool success = false;
					if (locals_table.empty()) {
						if (auto fv = lex.module->globals_table.insert(then_node.getKey(), *else_var)) {
							fv->ft.type_attr |= A_mainvar;
							success = true;
							fv->branch_parts = new std::vector<branch_part_t>(*else_var->branch_parts);
							fv->branch_parts->pop_back();
						}
					} else {
						auto new_else_var = *else_var;
						new_else_var.branch_parts = new std::vector<branch_part_t>(*else_var->branch_parts);
						new_else_var.branch_parts->pop_back();
						success = locals_table.back().insert(then_node.getKey(), new_else_var);
					}
					if (!success) {
						errs() << Loc << ": Variable '" << then_node.getKey() << "' already exists in outer scope\n";
						std::vector<BranchDescription> ret_vec;
						ret_vec.push_back({ std::vector<std::unique_ptr<ExprAST>>(), BreakDescription{0} });
						return { std::move(ret_vec), std::move(merged_vars), VarTable{}, false, false, then_end_kind, 0 };
					}
					if (verbosity >= 2)
						errs() << CurLoc << ": added '" << then_node.getKey() << "' to outer scope\n";
				} else {
					if (verbosity >= 2) {
						errs() << CurLoc << ": ***not*** added '" << then_node.getKey() << "' to outer scope ";
						if (else_var && else_var->branch_parts)
							errs() << fullVar(then_node)->branch_parts->back().brk_part << " " << else_var->branch_parts->back().brk_part << "\n";
						else
							errs() << "<\n";
					}
				}
			}
		}
	}
	return { std::move(Else), std::move(merged_vars), std::move(else_locals_table), have_else, true, then_end_kind, max_brk_level };
}

// try to add new variable to current context's database
//
static std::pair<FullVar*,new_var_kind> DeclareNewVariable(
	std::unique_ptr<ExprAST>& LHS, std::unique_ptr<ExprAST>* RHS,
	llvm::Type* LHS_type, llvm::Type* RHS_type, unsigned LHS_attr,
	unsigned RHS_attr, SourceLocation& BinLoc, bool LHS_is_unknown_type,
	bool RHS_is_unknown_type, bool ref_allowed = true, bool is_iterator = false)
{
	if (!RHS_type || RHS_type->isVoidTy()) {
		errs() << BinLoc << ": RHS of declaration is " << (RHS_type ? "of void type\n" : "indeterminate\n");
		if (RHS_type)
			if (auto branch_expr = dynamic_cast<BranchExprAST*>((*RHS).get()))
				if (branch_expr->errmsg)
					errs() << branch_expr->Loc << ": in conditional expression: " << branch_expr->errmsg;
		return { nullptr, new_var_none };
	}
	ReferenceExprAST* RefL = nullptr;
	VariableExprAST* VarL = nullptr;
	if (auto v = dynamic_cast<VariableExprAST*>(LHS.get())) {
		if (v->full_var)
			return { v->full_var, existing_var_returned };
		VarL = v;
	} else if (auto type = dynamic_cast<TypeExprAST*>(LHS.get())) {
		lex.type_err(type->Name.c_str(), type->Loc, type->ft);
		return { nullptr, new_var_none };
	} else if (auto function = dynamic_cast<FunctionExprAST*>(LHS.get())) {
		bool is_method = true;
		if (auto method = dynamic_cast<MethodExprAST*>(function)) {
			for (auto& proto: *method->ft->Protos)
				if (proto->visibility & A_setter)
					return { nullptr, setter_method_returned };
		} else
			is_method = false;
		lex.protos_err(function->Name.c_str(), function->Loc, function->ft->Protos, false, is_method);
		return { nullptr, new_var_none };
	} else if (auto mod = dynamic_cast<ModuleExprAST*>(LHS.get())) {
		lex.module_err(mod->Name.c_str(), mod->Loc, mod->importLoc);
		return { nullptr, new_var_none };
	}
	if (VarL)
		RefL = nullptr;
	else
		if ((RefL = dynamic_cast<ReferenceExprAST*>(LHS.get()))) {
			if (!ref_allowed) {
				errs() << LHS->Loc << ": reference not allowed "
				       << (is_iterator ? " with this iterator\n" : "in this case\n");
				return { nullptr, new_var_none };
			}
			if ((VarL = dynamic_cast<VariableExprAST*>(RefL->Operand.get()))) {
				if (VarL->full_var)
					return { VarL->full_var, existing_var_returned };
				if (RHS) {
					if (auto call_expr = dynamic_cast<CallExprAST*>((*RHS).get())) {
						// LHS is function pointer; signature of RHS will be used to select overloaded function
						if (auto typeexpr = dynamic_cast<TypeExprAST*>(call_expr->Callee.get())) {
							errs() << LHS->Loc << ": references to constructors or conversions not allowed ('" << typeexpr->Name << "' is a type)\n";
							return { nullptr, new_var_none };
						} else if (auto method = dynamic_cast<MethodExprAST*>(call_expr->Callee.get())) {
							errs() << LHS->Loc << ": references to methods not allowed ('" << method->Method->Name << "' is a method of type '" << *method->Receiver->ft << "')\n";
							return { nullptr, new_var_none };
						}
						*RHS = std::make_unique<FunctionExprAST>(call_expr);
						if (!(*RHS)->ft) {
							errs() << (*RHS)->Loc << ": unable to get function reference\n";
							return { nullptr, new_var_none };
						}
						RHS_type = (*RHS)->ft->type;
						RHS_attr = 0;
						RHS_is_unknown_type = false;
						// LHS = std::move(RefL->Operand);
						// RefL = nullptr;
						LHS_type = LHS->ft ? LHS->ft->type : nullptr;
						LHS_attr = LHS->ft ? LHS->ft->type_attr : 0;
						LHS_is_unknown_type = LHS->is_unknown_type;
						VarL = dynamic_cast<VariableExprAST*>(RefL->Operand.get());
					}
				}
			}
		}
	if (!VarL) {
		if (auto lval = dynamic_cast<LvalueExprAST*>(LHS.get()))
			// in AST.h:ForExprAST Key/Value FV/Lval are declared as unions - here we do a dirty conversion
			return { (FullVar*)lval, generic_lvalue_returned };
		else {
			errs() << LHS->Loc << ": left operand of assignment/declaration must be an lvalue\n";
			return { nullptr, new_var_none };
		}
	} else {
		auto [type, is_signed] = MakeType(RHS_type, RHS_attr, RHS_is_unknown_type);
		// TODO: check that variable always has the same type if declared in different
		// branches within the same function even when not merged (not necessary but design decision)
		FullVar fv = {
			.val = nullptr,
			.decl_loc = LHS->Loc,
			.branch_parts = new std::vector<branch_part_t>(current_branch_part),
			.ft = RHS ? *(*RHS)->ft : volvoxc::FullType{0}
		};
		fv.ft.type = type;
		fv.ft.type_attr &= ~(A_global | A_const | A_rvalue | A_mainvar);
		if (is_signed & A_signed)
			fv.ft.type_attr |= A_signed;
		else
			fv.ft.type_attr &= ~A_signed;
		if (is_signed & A_complex)
			fv.ft.type_attr |= A_complex;
		if (RefL)
			if (dynamic_cast<LvalueExprAST*>(LHS.get()))
				fv.ft.type_attr = (fv.ft.type_attr | A_ptrref) & ~A_destructor; // references need no destructors
			else {
				errs() << (RHS ? (*RHS)->Loc : CurLoc) << ": RHS of reference declaration must be an lvalue\n";
				return { nullptr, new_var_none };
			}
		else if (llvm::isa<llvm::ArrayType>(fv.ft.type) && (fv.ft.elem_type->type_attr & (A_destructor | A_string | A_map))) {
			fv.ft.type_attr |= A_destructor;
		}
		if (verbosity >= 2) {
			errs() << CurLoc << ": var " << VarL->Name;
			dump_branch_parts(fv.branch_parts);
		}
		if (inside_function || !current_branch_part.empty()) {
			if (auto entry = locals_table.back().insert(VarL->Name.c_str(), fv)) {
				VarL->full_var = nullptr; // in case a global with the same name had been found
				return { entry, new_var_created };
			} else {
				errs() << VarL->Loc << ": variable '" << VarL->Name << "' already exists in current scope\n";
				return { nullptr, new_var_none };
			}
		} else {
			fv.ft.type_attr |= A_mainvar;
			if (auto entry = lex.module->globals_table.insert(VarL->Name.c_str(), fv)) {
				return { entry, new_var_created};
			} else {
				errs() << VarL->Loc << ": variable '" << VarL->Name << "' already exists in \"main\" scope\n";
				return { nullptr, new_var_none };
			}
			// errs() << VarL->Loc << ": inserted " << VarL->Name << ", " << fv.ft.type_attr << " in mainvars\n";
		}
	}
}

/// for...in...;...[elif...]else...end
static std::unique_ptr<ExprAST> ParseForExpr(int terminator = 0) {
	SourceLocation ForLoc = CurLoc;
	getNextToken(); // eat for.
	syntax_nesting.push_back(1);
	// condition - expect bool.
	locals_table.emplace_back();
	current_branch_part.push_back(branch_part_t{0});
	auto KeyVal = ParseExpression(tok_in);
	bool descending;
	switch (CurTok.kind) {
	case tok_in:
		descending = false;
		break;
	case tok_reverse_in:
		descending = true;
		break;
	default:
		errs() << CurLoc << ": 'in' of '~in' expected!\n";
		return nullptr;
	}
	getNextToken(eNone);
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
	auto Iterator = ParseCondition(tok_in);
	auto [KeyFt, ValueFt, IteratorTy] = getKeyValueIteratorTypes(Iterator->ft, Iterator->Loc);
	FullVar* KeyFV = nullptr;
	FullVar* ValueFV = nullptr;
	new_var_kind key_kind = new_var_none, value_kind = new_var_none;
	if (Key) {
		if (!KeyFt) {
			errs() << Iterator->Loc << ": unable to determine type of 'for' key control variable\n";
			return nullptr;
		}
		if (auto lvalKey = dynamic_cast<LvalueExprAST*>(Key.get())) {
			std::tie(KeyFV, key_kind) = DeclareNewVariable(
				Key, nullptr, Key->ft->type, KeyFt->type, Key->ft->type_attr,
				KeyFt->type_attr, Key->Loc, Key->is_unknown_type,
				Iterator->is_unknown_type, false, true);
			if (key_kind == new_var_none) {
				errs() << lvalKey->Loc << ": unable to declare key control variable '"
				       << lvalKey->Name << "'\n";
				return nullptr;
			} else if (key_kind == setter_method_returned) {
				errs() << lvalKey->Loc << ": using virtual type fields as 'for' control variable not supported, yet\n";
				return nullptr;
			}
		} else {
			errs() << Key->Loc << ": 'for' key control variable must be an Lvalue\n";
			return nullptr;
		}
	}
	if (Value) {
		if (!ValueFt) {
			errs() << Iterator->Loc << ": unable to determine type of 'for' value control variable\n";
			return nullptr;
		}
		if (auto lvalValue = dynamic_cast<LvalueExprAST*>(Value.get())) {
			std::tie(ValueFV, value_kind) = DeclareNewVariable(
				Value, nullptr, Value->ft->type, ValueFt->type, ValueFt->type_attr,
				ValueFt->type_attr, Value->Loc, Value->is_unknown_type,
				Iterator->is_unknown_type, true, true);
			if (value_kind == new_var_none) {
				errs() << lvalValue->Loc << ": unable to declare value control variable '"
				       << lvalValue->Name << "'\n";
				return nullptr;
			} else if (value_kind == setter_method_returned) {
				errs() << lvalValue->Loc << ": using virtual type fields as 'for' control variable not supported, yet\n";
				return nullptr;
			}
		} else {
			errs() << Value->Loc << ": 'for' key control variable must be an Lvalue\n";
			return nullptr;
		}
	}
	std::vector<BranchDescription> Body;
	unsigned max_brk_level = 0;
	for (;;) {
		auto [list, e_kind, level] = ParseExprList();
		if (level > max_brk_level)
			max_brk_level = level;
		bool is_brk = ~(~e_kind & ((1<<16)-1)) == tok_brk;
		unsigned b_lev = is_brk ? ((~e_kind) >> 16)
			: (e_kind == tok_return) ? (unsigned)locals_table.size() : 1;
		std::map<std::string,FullVar*> destr_vars = get_destruct_vars(b_lev);
		if (e_kind == tok_return && !inside_function)
			get_destruct_vars_main(destr_vars);
		Body.push_back({ std::move(list), BreakDescription{
					.vars_to_destruct = std::move(destr_vars),
					.end_kind = e_kind,
					.break_level = b_lev
				} });
		if (~(~Body.back().second.end_kind & ((1<<16)-1)) != tok_brk)
			break;
		current_branch_part.back().brk_part++;
	}
	if (!Body.back().second.end_kind && Body.back().first.empty())
		return nullptr;
	VarTable then_locals_table = std::move(locals_table.back());
	locals_table.pop_back();
	auto [Else, merged_vars, else_locals_table, have_else, success, then_end_kind, level] = ParseElse(then_locals_table, ForLoc, tok_for, Body.back().second.end_kind);
	if (!success)
		return nullptr;
	if (level > max_brk_level)
			max_brk_level = level;
	if (have_else && Else.back().second.end_kind == tok_end && Body.back().second.end_kind != tok_elif || !have_else && Body.back().second.end_kind == tok_end)
		if (!Expect(tok_end, eBinOp)) {
			errs() << CurLoc << ": 'end' expected\n";
			return nullptr;
		}
	syntax_nesting.pop_back();
	return std::make_unique<ForExprAST>(ForLoc, std::move(Iterator), std::move(then_locals_table),
	                                    std::move(else_locals_table), max_brk_level, std::move(merged_vars), std::move(Key),
	                                    std::move(Value), std::move(KeyName), std::move(ValueName),
	                                    std::move(Body), std::move(Else), ValueFV, KeyFV,
	                                    ValueFt, KeyFt, key_kind, value_kind, descending);
}

static std::unique_ptr<ExprAST> ParseFunctionExpr(int terminator = 0) {
	auto FnLoc = CurLoc;
	unsigned visibility = A_closure;
	return_kind_t old_return_kind = function_return_kind;
	bool old_inside_function = inside_function;
	auto ast = ParseDefinition(visibility);
	function_return_kind = old_return_kind;
	inside_function = old_inside_function;
	if (ast)
		return std::make_unique<FunctionExprAST>(FnLoc, std::move(ast));
	else
		return nullptr;
}

/// primary
///   := identifierexpr
///   := numberexpr
///   := parenexpr
///   := ifexpr
///   := forexpr
///   := varexpr
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
	case tok_part_str_lit:
		return ParseInterpolatedStringExpr(terminator);
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
	case tok_vec:
	case tok_chan:
		return ParseAggregateExpr(false, terminator);
	case tok_if:
	case tok_while:
	case tok_repeat:
		return ParseIfExpr(terminator);
	case tok_for:
		return ParseForExpr(terminator);
	case tok_func:
		return ParseFunctionExpr(terminator);
	default:
		errs() << CurLoc << ": unexpected token '" << CurTok.str() << "' when expecting a " << lex.Expected << " or an expression\n";
		purgeLine();
		return nullptr;
	}
}

static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec, std::unique_ptr<ExprAST> LHS, int terminator = 0);

/// unary
///   := primary
///   := '!' unary
static std::unique_ptr<ExprAST> ParseUnary(int terminator = 0) {
	// If the current token is not an unary prefix operator, it must be a primary expr.
	auto kind = CurTok.kind;
	if (kind != tok_unary && kind != tok_ref && kind != tok_optional && kind != tok_thread)
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
		} else if (kind == tok_thread) {
			if (auto call = dynamic_cast<CallExprAST*>(Operand.get())) {
				auto Call = std::unique_ptr<CallExprAST>(call);
				Operand.release();
				return std::make_unique<ThreadExprAST>(Loc, std::move(Call));
			}
			errs() << Operand->Loc << ": 'task' requires a call expression as operand\n";
			return nullptr;
		}
		bool op_is_struct_ty = llvm::isa<llvm::StructType>(Operand->ft->type);
		if (op_is_struct_ty || (Operand->ft->type_attr & A_complex)) { // operator method
			auto fnName = "unary" + Op;
			const char* mangled_name = op_is_struct_ty ? Operand->ft->mangled_name : "3c32";
			if (auto Protos = findProtos(mangled_name, fnName)) {
				auto m_ident = std::make_unique<IdentExprAST>(Loc, std::move(fnName));
				auto method = std::make_unique<MethodExprAST>(
					Loc, std::move(Operand), std::move(m_ident), Protos);
				return std::make_unique<CallExprAST>(Loc, std::move(method));
			} else {
				errs() << Loc << ": unary '" << Op << "' not defined for operand of type " << *Operand->ft << "\n";
				return nullptr;
			}
		} else {
			return std::make_unique<UnaryExprAST>(Loc, Op.c_str(), std::move(Operand));
		}
	}
	return nullptr;
}

std::unique_ptr<ExprAST> getSelect(SourceLocation Loc, std::unique_ptr<ExprAST> LHS, std::unique_ptr<IdentExprAST> Ident) {
	if (LHS->ft->mangled_name) {
		if (LHS->ft->type_attr & A_interface) {
			auto protos = std::get<1>(*LHS->ft->InterfaceProtos)->find(Ident->Name);
			if (protos == std::get<1>(*LHS->ft->InterfaceProtos)->end()) {
				errs() << LHS->Loc << ": interface '" << *LHS->ft << "' has no method '" << Ident->Name << "'\n";
				return nullptr;
			}
			return std::make_unique<MethodExprAST>(LHS->Loc, std::move(LHS), std::move(Ident), &protos->second);
		}
		// handle some built-in methods - see also SelectExpAST::SelectExpAST() for further cases
		std::string real_method;
		std::string* the_method;
		if (LHS->ft->type == llvm_vec_type) {
			if (Ident->Name == "push") {
				real_method = "__push";
			} else if (Ident->Name == "pop") {
				real_method = "__pop";
			} else if (Ident->Name == "insert") {
				real_method = "__insert";
			} else if (Ident->Name == "remove") {
				real_method = "__remove";
			}
		}
		if (real_method.empty())
			the_method = &Ident->Name;
		else
			the_method = &real_method;
		auto protos = MethodProtos.find({LHS->ft->mangled_name, *the_method});
		if (protos != MethodProtos.end()) {
			if (LHS->ft->type == llvm_vec_type && !real_method.empty()) {
				// rewrite proto
				auto vec_protos = new_AnonProto(protos->second[0].get(), Ident->Loc);
				uint64_t elem_sz = TheModule->getDataLayout().getTypeAllocSize(LHS->ft->elem_type->type);
				(*vec_protos)[0]->implicitArgs.push_back(getSize(elem_sz));
				// see __vec methods in builtin.vx
				(*vec_protos)[0]->Args.erase((*vec_protos)[0]->Args.begin() + 1);
				(*vec_protos)[0]->ArgTypes.erase((*vec_protos)[0]->ArgTypes.begin() + 1);
				(*vec_protos)[0]->ArgAttrs.erase((*vec_protos)[0]->ArgAttrs.begin() + 1);
				(*vec_protos)[0]->ArgPos.erase((*vec_protos)[0]->ArgPos.begin() + 1);
				if ((*vec_protos)[0]->returnName.empty()) {
					(*vec_protos)[0]->ArgTypes[1] = new_FullType(*LHS->ft->elem_type, A_ref);
				} else {
					(*vec_protos)[0]->RetType = LHS->ft->elem_type;
				}
				return std::make_unique<MethodExprAST>(LHS->Loc, std::move(LHS), std::move(Ident), vec_protos);
			}
			return std::make_unique<MethodExprAST>(LHS->Loc, std::move(LHS), std::move(Ident), &protos->second);
		}
	}
	return std::make_unique<SelectExprAST>(LHS->Loc, std::move(LHS), std::move(Ident));
}

/// binoprhs
///   := ('+' unary)*
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec, std::unique_ptr<ExprAST> LHS, int terminator) {
	// If this is a binop, find its precedence.
	while (true) {
		int TokPrec = GetTokPrecedence();
		// If this is a binop that binds at least as tightly as the current binop,
		// consume it, otherwise we are done.
		if (!LHS || !LHS->ft)
			return nullptr;
		if (NextTokPrecedence() <= ExprPrec) {
			if (LHS->ft->type && (LHS->ft->type->isFunctionTy() ||
			                      LHS->ft->type == llvm_closure_type || dynamic_cast<TypeExprAST*>(LHS.get())) && (ExprPrec >> 8) != -(int)tok_ref)
				// function call without argument, e.g. "abort"
				LHS = std::make_unique<CallExprAST>(LHS->Loc, std::move(LHS), std::vector<std::unique_ptr<ExprAST>>{});
			return LHS;
		}
		// Okay, we know this is a binop.
		std::string BinOp = IdentifierStr;
		SourceLocation BinLoc = CurLoc;
		auto BinKind = CurTok.kind;
		if (LHS->ft->type && (LHS->ft->type->isFunctionTy() ||
		                      LHS->ft->type == llvm_closure_type || dynamic_cast<TypeExprAST*>(LHS.get()))) {
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
		if (RHS->ft->type && (RHS->ft->type->isFunctionTy() || RHS->ft->type == llvm_closure_type))
			// RHS of binary expression is function call without parameters, e.g. "x = f"
			RHS = std::make_unique<CallExprAST>(RHS->Loc, std::move(RHS), std::vector<std::unique_ptr<ExprAST>>{});
		// Merge LHS/RHS.
		// save types befor objects are moved
		auto LHS_type = LHS->ft ? LHS->ft->type : nullptr;
		auto LHS_attr = LHS->ft ? LHS->ft->type_attr : 0;
		auto LHS_is_unknown_type = LHS->is_unknown_type;
		auto RHS_type = RHS->ft ? RHS->ft->type : nullptr;
		auto RHS_attr = RHS->ft ? RHS->ft->type_attr : 0;
		auto RHS_is_unknown_type = RHS->is_unknown_type;
		bool is_decl = false;
		if (BinOp == "=" || BinOp == ":=") {
			auto [new_fv, new_kind] = DeclareNewVariable(LHS, &RHS, LHS_type, RHS_type, LHS_attr, RHS_attr,
			                                 BinLoc, LHS_is_unknown_type, RHS_is_unknown_type);
			if (new_kind == new_var_none)
				return nullptr;
			if (new_kind == setter_method_returned) {
				std::vector<std::unique_ptr<ExprAST>> args;
				args.push_back(std::move(RHS));
				LHS = std::make_unique<CallExprAST>(
					LHS->Loc, std::move(LHS),
					std::move(args));
				continue;
			}
			is_decl = (new_kind == new_var_created);
			// TODO: store returned 'new_fv' instead of discarding here and re-evaluating in codegen.cc
		} else if (LHS_type && (LHS_type->isFunctionTy()
		                        || LHS_type == llvm_closure_type || dynamic_cast<TypeExprAST*>(LHS.get()))) {
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
		} else if (BinOp == "?=") {
			if (auto colon_expr = dynamic_cast<BinaryExprAST*>(RHS.get())) {
				if (strcmp(colon_expr->Op, ":")) {
					errs() << colon_expr->Loc << ": expected ':' as separator of RHS of '?='\n";
					return nullptr;
				}
			} else {
				errs() << RHS->Loc << ": expected colon-separated expressions as RHS of '?='\n";
				return nullptr;
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
				errs() << RHS->Loc << ": selector name expected\n";
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
				// create temporary to work around MSVC problem
				auto LL = LHS->Loc;
				LHS = getSelect(LL, std::move(LHS), std::move(Ident));
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
		if (LHS_type && RHS_type) {
			if (LHS_type->isStructTy() || (LHS_attr & A_complex)) {
				auto fnName = "binary" + BinOp;
				const char* mangled_name = LHS_type->isStructTy() ? LHS->ft->mangled_name : "3c32";
				if (auto Protos = findProtos(mangled_name, fnName)) {
					auto m_ident = std::make_unique<IdentExprAST>(BinLoc, std::move(fnName));
					auto method = std::make_unique<MethodExprAST>(
						BinLoc, std::move(LHS), std::move(m_ident), Protos);
					std::vector<std::unique_ptr<ExprAST>> Arg;
					Arg.push_back(std::move(RHS));
					LHS = std::make_unique<CallExprAST>(BinLoc, std::move(method), std::move(Arg));
					continue;
				} else if (BinOp != "," && BinOp != "=" && BinOp != ":") {
					errs() << BinLoc << ": no operator method for " << *LHS->ft << BinOp << *RHS->ft << " declared\n";
					return nullptr;
				}
			}
			if (RHS_type->isStructTy() || (RHS_attr & A_complex)) {
				auto fnName = "reverse" + BinOp;
				const char* mangled_name = RHS_type->isStructTy() ? RHS->ft->mangled_name : "3c32";
				if (auto Protos = findProtos(mangled_name, fnName)) {
					auto m_ident = std::make_unique<IdentExprAST>(BinLoc, std::move(fnName));
					auto method = std::make_unique<MethodExprAST>(
						BinLoc, std::move(RHS), std::move(m_ident), Protos);
					std::vector<std::unique_ptr<ExprAST>> Arg;
					Arg.push_back(std::move(LHS));
					LHS = std::make_unique<CallExprAST>(BinLoc, std::move(method), std::move(Arg));
					continue;
				} else if (BinOp != "," && BinOp != "=" && BinOp != ":") {
					errs() << BinLoc << ": no (reverse) operator method for " << *LHS->ft << BinOp << *RHS->ft << " declared\n";
					return nullptr;
				}
			}
		}
		auto res_t = (is_decl || BinOp == ",")
			? std::tuple<llvm::Type*, unsigned, bool, OpClass, const char*>{
			is_decl ? nullptr : llvm::Type::getVoidTy(Context), 0, false, is_decl ? OpDeclAssign : getOpClass(BinOp.c_str()), nullptr }
			: getResType(LHS_type, RHS_type, BinOp.c_str(), LHS_attr, RHS_attr,
			             LHS_is_unknown_type, RHS_is_unknown_type);
		if (!std::get<0>(res_t) && BinOp == ":") {
			std::get<0>(res_t) = llvm::Type::getVoidTy(Context);
			std::get<4>(res_t) = nullptr;
		}
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
			if (RHS_type)
				if (auto branch_expr = dynamic_cast<BranchExprAST*>(RHS.get()))
					if (branch_expr->errmsg)
						errs() << branch_expr->Loc << ": in conditional expression: " << branch_expr->errmsg;

			return nullptr;
		}
	valid_void:
		if (BinKind == tok_invisible) {
			auto lit = dynamic_cast<LiteralExprAST*>(RHS.get());
			if (lit && lit->ft->type != llvm_string_type) {
				// we forbid 'a 2' instead of '2 a', or '12 34' instead of 408
				// because usually these are cases of a missing ',' separator ('a, 2'; '12, 34')
				errs() << lit->Loc << ": literal number as RHS of invisible operator not allowed - maybe you forgot a ',' or an explicit operator\n";
				return nullptr;
			}
		}
		LHS = std::make_unique<BinaryExprAST>(BinLoc, BinOp.c_str(), std::move(LHS), std::move(RHS), res_t);
	}
}

/// expression
///   := unary binoprhs
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

static std::pair<std::unique_ptr<ExprAST>, int> ParseExprOrReturn() {
	while (CurTok.kind == ';')
		getNextToken();
	auto kind = CurTok.kind;
	if (kind == tok_return) {
		getNextToken(eSemi);
		if (CurTok.kind == ';' || CurTok.kind == tok_end)
			return { nullptr, kind };
		else
			return { ParseExpression(), kind };
	} else if (kind == tok_brk) {
		unsigned levels = 0;
		do {
			levels++;
			getNextToken(eSemi);
		} while (CurTok.kind == tok_brk);
		if (levels > current_branch_part.size()) {
			if (current_branch_part.empty())
				errs() << CurLoc << ": 'brk' called outside of any branch branch\n";
			else
				errs() << CurLoc << ": " << levels << " level 'brk' called, but branch nesting level is only "
				       << current_branch_part.size() << "\n";
			return { nullptr, 0 };
		}
		// encode multi level brk in upper bits of kind
		// but be careful: kind is negative
		return { ParseExpression(), (int)~(~(unsigned)kind | (levels << 16)) };
	} else if (kind == tok_else || kind == tok_elif || kind == tok_end || kind == tok_until) {
		return { nullptr, kind };
	} else if (kind == tok_global || kind == tok_const || kind == tok_atomic) {
		errs() << CurLoc << ": global/const/atomic cannot be defined inside functions\n";
		return { nullptr, 0 };
	} else {
		return { ParseExpression(), 0 };
	}
}

static std::tuple<std::vector<std::unique_ptr<ExprAST>>, int, unsigned> ParseExprList() {
	std::vector<std::unique_ptr<ExprAST>> expr_list;
	int end_kind = 0;
	unsigned max_brk_level = 0;
	while (!end_kind) {
		auto expr = ParseExprOrReturn();
		end_kind = expr.second;
		if (expr.first) {
			if (!end_kind) {
				if (auto I = dynamic_cast<IfExprAST*>(expr.first.get())) {
					if (I->max_brk_level) {
						if (max_brk_level < I->max_brk_level)
							max_brk_level = I->max_brk_level - 1;
					} else if (I->Then.back().second.end_kind == tok_return && I->Else.back().second.end_kind == tok_return) {
						// both branches do return and there is no break
						// TODO: stricter checks for multi level break
						end_kind = tok_return;
					}
				}
			} else if (end_kind == tok_return && function_return_kind != return_expr) {
				errs() << expr.first->Loc << ": return value for "
				       << ((function_return_kind == return_variable) ? "function with return variable" :
				           (function_return_kind == return_constructor) ? "constructor" :
				           (function_return_kind == return_destructor) ? "destructor" : "void function")
				       << " unexpected\n";
				return { std::vector<std::unique_ptr<ExprAST>>{}, 0, 0 };
			}
			expr_list.push_back(std::move(expr.first));
		} else {
			if (!end_kind)
				return { std::vector<std::unique_ptr<ExprAST>>{}, 0, 0 };
			if (end_kind == tok_global)
				end_kind = 0;
			else if (function_return_kind == return_expr && end_kind == tok_return) {
				errs() << CurLoc << ": return value expected for non-void function\n";
				return { std::vector<std::unique_ptr<ExprAST>>{}, 0, 0 };
			}
		}
	}
	if (~(~end_kind & ((1<<16)-1)) == tok_brk) {
		unsigned brk_depth_raw = (~end_kind) >> 16;
		// errs() << CurLoc << ": got 'brk' - raw: " << brk_depth_raw;
		unsigned brk_depth = 0;
		int idx = syntax_nesting.size() - 1;
		while (brk_depth_raw) {
			if (idx < 0) {
				errs() << CurLoc << ": 'brk' level exceeds nesting level\n";
				return { std::vector<std::unique_ptr<ExprAST>>{}, 0, 0 };
			}
			brk_depth++;
			brk_depth_raw -= syntax_nesting[idx--];
		}
		// errs() << " effective: " << brk_depth << "\n";
		//
		// update end_kind - maybe this is too early(?)
		end_kind = (int)~(~(unsigned)tok_brk | (brk_depth << 16));
		if (brk_depth > max_brk_level)
			max_brk_level = brk_depth;
	}
	return { std::move(expr_list), end_kind, max_brk_level };
}

// for operator methods - '.' means normal method - '=' must be last
#define OVERLOAD_OPERATORS ".+-*/%^<>!="

/// prototype
///   := id '(' id* ')'
///   := binary LETTER number? (id, id)
///   := unary LETTER (id)
static std::unique_ptr<PrototypeAST> ParsePrototype(unsigned& visibility) {
	std::string FnName;
	volvoxc::FullType* ReceiverType = nullptr;
	std::string UnmagledReceiverTypeName;
	SourceLocation FnLoc = CurLoc;
	std::string returnName;
	unsigned Kind = 0; // 0 = identifier, 1 = unary, 2 = binary, 3 reverse binary.
	std::vector<std::string> ArgNames;
	std::vector<volvoxc::FullType*> ArgTypes;
	std::vector<arg_needs_constructor_t> ArgNeedsConstructor = {};
	std::vector<SourceLocation> ArgPos;
	bool isVarArgs = false;
	bool this_is_value = false; // TODO: check modules of type/constructor
	volvoxc::FullType* tmp_rec_type = nullptr;
	int operator_idx = -1;
	std::string TheFn = IdentifierStr;
	const char* operators = OVERLOAD_OPERATORS;
	if (!(visibility & A_closure)) {
		if (CurTok.kind != tok_identifier) {
			errs() << CurLoc << ": identifier expected (function name or receiver type)\n";
			return nullptr;
		}
		// make sure the function name is not in use as (global/main) variable
		// if "inside_function" we would not see conflicting non-global main vars, so clear this flag temporarily
		auto old_inside_function = inside_function;
		inside_function = false;
		if (lex.previously_used(IdentifierStr, CurLoc, lex_skip_protos | lex_skip_type))
		    return nullptr;
		inside_function = old_inside_function;
		// identify constructors and destructors:
		// all kinds of methods start with a type name - even destructors as we
		// have eaten the '~' already in ParseDefinition()
		tmp_rec_type = lex.get_full_type(IdentifierStr.c_str());
		auto op_ptr = strchr(operators, lex.peek());
		if (op_ptr) {
			operator_idx = op_ptr - operators;
			if (operator_idx == ARRAY_SIZE(OVERLOAD_OPERATORS) - 2 && lex.peek2() != '=')
				operator_idx = -1;
		}
		if (visibility & A_interface) {
			visibility |= A_method;
			ReceiverType = interface_ref_type;
			UnmagledReceiverTypeName = "interface";
			ArgNames.push_back("this");
			ArgTypes.push_back(ReceiverType);
			ArgNeedsConstructor.push_back(arg_is_borrowed_or_pod);
			ArgPos.push_back(CurLoc);
		} else {
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
				} else {
					if (operator_idx < 0) // normal method has operator_idx == 0
						visibility |= (A_constructor | A_ref);
				}
				if (visibility & A_c_api) {
					errs() << CurLoc << ": methods/constructors/destructors cannot be declared using C-API - use 'def' instead of 'cdef'\n";
					return nullptr;
				}
				ReceiverType = new_FullType(*tmp_rec_type);
				// TODO: avoid creating new FullTypes just for adding attributes
				// This will require ParseType() to return attributes separately
				// and ProtoTypeAST::ProtoTypeAST() to get ArgAttrs passed
				UnmagledReceiverTypeName = std::move(IdentifierStr);
				auto receiver_struct_type = llvm::dyn_cast<llvm::StructType>(ReceiverType->type);
				if (receiver_struct_type && receiver_struct_type->isOpaque()) {
					errs() << CurLoc << ": method for incomplete type '" << UnmagledReceiverTypeName << "' not allowed\n";
					errs() << ReceiverType->decl_loc << ": this is the location of the type declaration but the is no definition anywhere\n";
					return nullptr;
				}
				if (ReceiverType->type->isStructTy() || operator_idx >= 0) {
					if (visibility & A_const)
						// we use "by-const-reference" even for small receivers to allow
						// more consistent interface declarations
						ReceiverType->type_attr |= (A_ref | A_by_value);
					else
						ReceiverType->type_attr |= A_ref;
					ArgNames.push_back("this");
					ArgTypes.push_back(ReceiverType);
					ArgNeedsConstructor.push_back(arg_is_borrowed_or_pod);
					ArgPos.push_back(CurLoc);
				} else {
					if (visibility & A_constructor) {
						// this is a base type constructor that returns "this" by value
						returnName = "this";
						this_is_value = true;
					}
				}
				if (!(visibility & (A_destructor | A_constructor))) {
					getNextToken(eBinOp, eSemi);
					if (!operator_idx && !Expect(tok_selector)) // eat '.' for method declarations
						return nullptr;
				}
			} else {
				if (operator_idx >= 0 || (visibility & A_destructor)) {
					errs() << CurLoc << ": error in " << ((visibility & A_destructor) ? "destructor" : "method")
					       << " parsing - '" << IdentifierStr << "' is not a known type\n";
					return nullptr;
				}
			}
			if (last_defined_type && !(visibility & (A_constructor | A_destructor)) && !this_is_value)
				// there is still a destructor/constructor section pending for the last type declaration
				// which is unrelated - let's finish that before we continue with this definition here
				finish_constructors_and_destructor();
		}
	}
	switch ((int)CurTok.kind) {
	case tok_identifier:
		if (ReceiverType && !(visibility & (A_destructor | A_destructor)))
			if (MapValue* mv = map_string_get(ReceiverType->fields, IdentifierStr.c_str())) {
				errs() << CurLoc << ": method name '" << IdentifierStr << "' for type '" << *ReceiverType << "' conflicts with field of same name\n";
				errs() << ReceiverType->decl_loc << ": this is the location of the type definition\n";
				return nullptr;
			}
		FnName = (visibility & A_destructor) ? ("~" + UnmagledReceiverTypeName) :
			(visibility & A_constructor) ? std::move(UnmagledReceiverTypeName) : std::move(IdentifierStr);
		Kind = 0;
		getNextToken(eSemi);
		break;
	case tok_func: // closure
		FnName = std::string(createAnonFnName());
		Kind = 0;
		getNextToken(eSemi);
		break;
	case tok_assign:
		if (IdentifierStr == "=")
			goto unexpected;
	case tok_add:
	case tok_mult:
	case tok_pow:
	case tok_cmp:
		FnName = IdentifierStr;
		getNextToken();
		break;
	default:
	unexpected:
		errs() << CurLoc << ": unexpected token '" << CurTok << "' - expected prototype name or operator\n";
		return nullptr;
	}
	if (CurTok.kind == '=') {
		visibility |= A_setter;
		getNextToken(eSemi);
	}
	if (CurTok.kind != '(')
		goto nobrace;
	else
		getNextToken();
	if (CurTok.kind == ')')
		goto noargs;
	for (;;) {
		if (CurTok.kind == tok_ellipsis) {
			auto El_Pos = CurLoc;
			getNextToken();
			if (CurTok.kind != ')') {
				errs() << CurLoc << ": unexpected '" << CurTok << "' after '...' - ')' expected\n";
				return nullptr;
			}
			isVarArgs = true;
			if (!(visibility & A_c_api)) {
				ArgNames.push_back("va_args");
				ArgPos.push_back(El_Pos);
				ArgTypes.push_back(va_arg_type);
				ArgNeedsConstructor.push_back(arg_is_borrowed_or_pod);
			}
			break;
		}
		auto ArgLoc = CurLoc;
		auto [name, ft] = ParseTypedIdent(')', false);
		if (!ft)
			return nullptr;
		if (lex.previously_used(name, ArgLoc, 0))
			return nullptr;
		// treat type without variable name as blanc ident
		ArgNames.push_back(((uintptr_t)ft & 1) ? "_" : name);
		ArgPos.push_back(ArgLoc);
		auto type = (volvoxc::FullType*)((uintptr_t)(ft) & ~1ULL);
		if (!(type->type_attr & (A_ref | A_interface))) {
			uint64_t arg_size = type->type->isSized() ? TheModule->getDataLayout().getTypeAllocSize(type->type) : 0;
			if (!arg_size || arg_size > sret_limit)
				type = new_FullType(*type, A_by_value | A_ref);
		}
		ArgTypes.push_back(type);
		// If we have to call the copy constructor for the argument depends
		// on the implementation, so we cannot know while parsing the prototype
		// but at least we know for sure that we don't need one if there is none
		arg_needs_constructor_t argflags = arg_is_borrowed_or_pod;
		if ((type->type_attr & A_by_value) || !(type->type_attr & A_ref)) {
			if (type->type_attr & A_constructor)
				set_arg_flag(&argflags, arg_has_constructor);
// 			if (type->type_attr & A_destructor)
// 				set_arg_flag(&argflags, arg_has_destructor);
			if (argflags != arg_is_borrowed_or_pod)
			  set_arg_flag(&argflags, maybe_arg_is_owned);
		}
		ArgNeedsConstructor.push_back(argflags);
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
	if (CurTok.kind != ';') {
		if (CurTok.kind == '(') {
			getNextToken();
			std::tie(returnName, RetType) = ParseTypedIdent(')', false);
			getNextToken();
		} else {
			RetType = ParseType(0, eSemi);
		}
		if (!RetType) {
			errs() << "error parsing return type of function prototype\n";
			return nullptr;
		}
	}
	if (visibility & A_setter) {
		if (!(visibility & A_method) || (visibility & (A_constructor | A_destructor))) {
			errs() << CurLoc << ": setter must be a regular method\n";
			return nullptr;
		}
		if (ArgTypes.size() != 2 || (ArgTypes[1]->type_attr & A_va_arg)) {
			if (visibility & A_interface) {
				if (ArgTypes.size() != 1) {
					errs() << CurLoc << ": getter/setter interface declaration may have at most 1 argument\n";
					return nullptr;
				}
				if (!RetType) {
					errs() << CurLoc << ": getter/setter interface short declaration requires a return type\n";
					return nullptr;
				}
			} else {
				errs() << CurLoc << ": setter must be regular method with exactly 1 argument\n";
				return nullptr;
			}
		}
	}
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
			if (ReceiverType->type->isStructTy()) {
				if (ArgTypes.size() != 1) {
					errs() << CurLoc << ": function declaration for type identifier '" << FnName << "' is invalid:\n - to be a constructor it cannot have a return type\n - to be a type conversion operator it must not have any call argument\n";
					return nullptr;
				}
			} else {
				errs() << CurLoc << ": function declaration for type identifier '" << FnName << "' is invalid:\n - to be a constructor it cannot have a return type\n - a type conversion operator cannot be declared for basic types\n";
				return nullptr;
			}
			visibility = (visibility & ~A_constructor) | A_conversion;
		} else {
			// default constructor - set flag in type
			if (ReceiverType->type->isStructTy()) {
				if (ArgTypes.size() == 1)
					tmp_rec_type->type_attr |= A_constructor;
			} else {
				RetType = ReceiverType;
				if (ArgTypes.size() == 1)
					visibility = visibility & ~(A_constructor | A_method);
			}
		}
	} else if (operator_idx > 0) {
		if (ArgTypes.size() > 2) { // cannot be 0 because we have a receiver as 1st arg
			errs() << ArgPos[2] << ": operator methods must not have more then one argument\n";
			return nullptr;
		}
		if (ArgTypes[0]->type->isStructTy() || (ArgTypes[0]->type_attr & A_complex)) {
			if (ArgTypes.size() == 1) {
				if (operator_idx > 2) {
					errs() << ArgPos[0] << ": operators other than '+' or '-' cannot be declared as unary\n";
					return nullptr;
				}
				FnName = "unary" + FnName;
				Kind = 1;
			} else {
				FnName = "binary" + FnName;
				Kind = 2;
			}
		} else if (ArgTypes.size() == 2 && (ArgTypes[1]->type->isStructTy() || (ArgTypes[1]->type_attr & A_complex))) {
			FnName = "reverse" + FnName;
			Kind = 3;
			auto nam_tmp = std::move(ArgNames[0]);
			ArgNames[0] = std::move(ArgNames[1]);
			ArgNames[1] = std::move(nam_tmp);
			auto ty_tmp = ArgTypes[0];
			ArgTypes[0] = ArgTypes[1];
			ArgTypes[1] = ty_tmp;
			auto pos_tmp = ArgPos[0];
			if (!(ArgTypes[0]->type_attr & A_ref))
				ArgTypes[0]->type_attr |= (A_ref | A_by_value);
			if (!ArgTypes[1]->type->isStructTy())
				// we pass non-struct LHS by value to allow automatic type conversions like
				// int * complex -> f64 * complex
				ArgTypes[1]->type_attr &= ~A_ref;
			ArgPos[0] = ArgPos[1];
			ArgPos[1] = pos_tmp;
			auto needs_constr_tmp = ArgNeedsConstructor[0];
			ArgNeedsConstructor[0] = ArgNeedsConstructor[1];
			ArgNeedsConstructor[1] = needs_constr_tmp;
		} else {
			errs() << ArgPos[0] << ": at least one of receiver or argument must be a declared type\n";
			return nullptr;
		}
	}
	if (!this_is_value && (visibility & (A_constructor | A_destructor))) {
		if (ArgTypes.size() == 1 && (!last_defined_type || TheFn != last_defined_type)) {
			errs() << CurLoc << ": definition(s) of default constructor / destructor must follow immediately corresponding type declaration ('" << last_defined_type << " / " << TheFn << "')\n";
			return nullptr;
		}
		visibility |= A_pub;
	}
	return std::make_unique<PrototypeAST>(FnLoc, FnName, ArgNames, visibility, retLoc, Kind, RetType, ArgTypes, ArgNeedsConstructor, ArgPos, std::move(returnName), isVarArgs);
}

// return -2 for conflict, -1 for new Proto, 0...n for matching index
int PrototypeAST::conflicts(std::vector<std::unique_ptr<PrototypeAST>>& protos) {
	int res = -1;
	int n = protos.size();
	for (int i=0; i<n; i++) {
		auto proto = protos[i].get();
		if ((proto->visibility & A_setter) && !(visibility & A_setter)) {
			errs() << this->retLoc << ": method invalid - setter with the same name has already been declared\n";
			errs() << proto->retLoc << ": this is the location of the setter declaration\n";
			return -2;
		}
		auto matching_state = CompareProtos(this, proto);
		switch (matching_state) {
		case protos_matching:
			if (verbosity >= 1)
				errs() << this->retLoc << ": this prototype has previously been declared in a matching way\n"
				       << proto->retLoc << ": this is the location of the previous declaration\n";
			res = i;
			continue;
		case protos_different:
			continue;
		case protos_conflicting:
			errs() << this->retLoc << ": return type '" << *this->RetType << "' conflicts with\n"
			       << proto->retLoc << ": previous declaration returning '" << *proto->RetType << "'\n";
			return -2;
		case protos_conflicting_c_api_A:
			errs() << this->retLoc << ": conflicting APIs - C-type prototype for function\n"
			       << proto->retLoc << ": previously declared as native-type here\n";
			return -2;
		case protos_conflicting_c_api_B:
			errs() << this->retLoc << ": conflicting APIs - native-type prototype for function\n"
			       << proto->retLoc << ": previously declared as C-type here\n";
			return -2;
		case protos_conflicting_c_signature:
			if (verbosity >= 1)
				errs() << this->retLoc << ": C-type prototype might conflict with previous declaration\n"
				       << proto->retLoc << ": here with identical external name but different signature\n";
			continue;
		}
	}
	if (res == -1 && (visibility & A_setter))
		if (n != 1 || ArgTypes.size() != 2 || protos[0]->ArgTypes.size() != 1 || FullTypes_differ(protos[0]->RetType, ArgTypes[1])) {
			errs() << this->retLoc << ": setter can only be declared if corresponding getter has been declared as only other method with the same name\n";
			for (auto& p: protos)
				errs() << p->retLoc << ": previous method\n";
			return -2;
		}
	return res;
}

// append prototype to list after checking that it does not already exist
bool check_and_add_proto(std::vector<std::unique_ptr<PrototypeAST>>& protos, std::unique_ptr<PrototypeAST> Proto,
                                std::string& unmangledName, bool isMethod) {
	int match_idx = Proto->conflicts(protos);
	switch (match_idx) {
	case -2:
		prompt_indent = 0;
		return false;
	case -1:
		protos.push_back(std::move(Proto));
		return true;
	default:
		protos[match_idx] = std::move(Proto);
		return true;
	}
}

volvoxc::FullType* ParseInterface(unsigned attribs, eXpect expect,
                                  int terminator, const char* iname,
                                  llvm::StructType* existing) {
	if (!Expect('{'))
		return nullptr;
	//       method name                       prototype                     offset in vtable
	auto Protos = std::make_unique<std::map<std::string,std::vector<std::unique_ptr<PrototypeAST>>>>();
	std::vector<volvoxc::FullType*> Embeds;
	size_t offset = 1; // methods[0] holds the size of the vtable - to find RtType at run time
	while (CurTok.kind != '}') {
		unsigned visibility = A_interface;
		auto proto = ParsePrototype(visibility);
		if (!proto)
			return nullptr;
		bool is_getter_setter_field;
		std::unique_ptr<PrototypeAST> getter_proto = nullptr;
		std::unique_ptr<PrototypeAST> setter_proto = nullptr;
		if (proto->visibility & A_setter) {
			if (proto->ArgTypes.size() == 1) {
				// special getter/setter/field interface declaration provide 2 methods with A_setter
				getter_proto = std::make_unique<PrototypeAST>(
					proto->retLoc, proto->Name, std::vector<std::string>{ "this" },
					A_method | A_interface | A_getter, proto->retLoc,
					0, proto->RetType, std::vector<volvoxc::FullType*>{ proto->ArgTypes[0] },
					std::vector<arg_needs_constructor_t>{ arg_is_borrowed_or_pod }, std::vector<SourceLocation>{ proto->retLoc });
				setter_proto = std::make_unique<PrototypeAST>(
					proto->retLoc, proto->Name, std::vector<std::string>{ "this", "assignment_RHS" },
					A_method | A_interface | A_setter, proto->retLoc,
					0, proto->RetType, std::vector<volvoxc::FullType*>{ proto->ArgTypes[0], proto->RetType },
					std::vector<arg_needs_constructor_t>{ arg_is_borrowed_or_pod, arg_is_borrowed_or_pod }, std::vector<SourceLocation>{ proto->retLoc, proto->retLoc });
				is_getter_setter_field = true;
			} else {
				errs() << proto->retLoc << ": getter/setter has to be in the form 'method=type' inside interface declaration\n";
				return nullptr;
			}
		} else
			is_getter_setter_field = false;
		auto& protos = (*Protos)[proto->Name];
		if (is_getter_setter_field) {
			if (!protos.empty()) {
				errs() << proto->retLoc << ": declaration of '" << proto->Name << "' as (pseudo) field conflicts with method(s) with the same name\n";
				lex.protos_err(nullptr, proto->retLoc, &protos, false, true);
				return nullptr;
			}
		} else {
			for (auto& existing_proto: protos) {
				if ((existing_proto->visibility & A_setter) && existing_proto->ArgTypes.size() == 1) {
					errs() << proto->retLoc << ": declaration of '" << proto->Name << "' as method conflicts with (pseudo) field with the same name\n";
					errs() << existing_proto->retLoc << ": this is the location of the prvious declaration\n";
					return nullptr;
				}
				auto match = CompareProtos(proto.get(), existing_proto.get());
				if (match == protos_different)
					continue;
				if (match == protos_matching)
					errs() << proto->retLoc << ": method '" << proto->Name << "' with same signature has already been declared\n";
				else
					errs() << proto->retLoc << ": method '" << proto->Name << "' with same signature but conflicting return type has already been declared\n";
				errs() << existing_proto->retLoc << ": this is the location of the previous declaration\n";
				return nullptr;
			}
		}
		if (is_getter_setter_field) {
			getter_proto->vtable_offs = offset++;
			protos.push_back(std::move(getter_proto));
			setter_proto->vtable_offs = offset++;
			protos.push_back(std::move(setter_proto));
		} else {
			proto->vtable_offs = offset++;
			protos.push_back(std::move(proto));
		}
		if (CurTok.kind != '}')
			if (!Expect(';'))
				return nullptr;
	}
	getNextToken(eSemi);
	std::vector<llvm::Type*> interface_type_elements = { llvm_ptr_type, llvm_ptr_type };
	if (iname) {
		if (auto struct_ty = llvm::StructType::getTypeByName(Context, iname)) {
			if (!struct_ty->isOpaque()) {
				errs() << CurLoc << ": type '" << iname << "' has already been defined\n";
				return nullptr;
			}
			struct_ty->setBody(interface_type_elements);
			llvm_interface_type = struct_ty;
		} else {
			llvm_interface_type = llvm::StructType::create(Context, interface_type_elements, iname, false);
		}
	} else {
		llvm_interface_type = llvm::StructType::get(Context, interface_type_elements, false);
	}
	auto ft = new_FullType(llvm_interface_type, A_interface);
	auto array_type = llvm::ArrayType::get(llvm_ptr_type, offset);
	InterfaceProtos.push_back({ array_type, std::move(Protos), std::move(Embeds) });
	ft->InterfaceProtos = &InterfaceProtos.back();
	return ft;
}

#define TEST_FN_PREFIX "test_"

void setMangledName(PrototypeAST* Proto, unsigned visibility) {
	std::string unmangledName = Proto->getName();
	if (visibility & A_c_api) {
		if (!cdecl_rename.empty())
			Proto->Name = cdecl_rename;
	} else if ((visibility & A_conversion) && !(visibility & A_constructor)) {
		std::vector<volvoxc::FullType*> targetType = { Proto->ArgTypes[0], Proto->RetType };
		Proto->Name = Mangle(lex.module->import_path, unmangledName, targetType, Proto->visibility).c_str();
	} else {
		unsigned flags = Proto->visibility;
		if ((flags & A_constructor) && Proto->RetType && !Proto->RetType->type->isVoidTy())
			flags |= A_constructor_value_return;
		Proto->Name = Mangle(lex.module->import_path, unmangledName, Proto->ArgTypes, flags).c_str();
	}
}

/// definition := 'def' prototype expression
std::unique_ptr<FunctionAST> ParseDefinition(unsigned& visibility) {
	if (!(visibility & A_closure)) {
		getNextToken(eSemi); // eat 'def'
		if (CurTok.kind != tok_identifier) {
			if (CurTok.kind == tok_const)
				visibility |= A_const;
			else if (CurTok.kind == tok_unary && IdentifierStr == "~")
				visibility |= A_destructor;
			else {
				errs() << CurLoc << ": invalid operator '" << IdentifierStr << "' in function definition\n";
				return nullptr;
			}
			getNextToken(eSemi);
		}
	}
	auto theLoc = CurLoc;
	auto Proto = ParsePrototype(visibility);
	if (!Proto) {
		errs() << theLoc << ": unable to parse declaration\n";
		prompt_indent = 0;
		return nullptr;
	}
	if ((Proto->visibility & A_constructor) && !(Proto->visibility & A_conversion))
		function_return_kind = return_constructor;
	else if (Proto->visibility & A_destructor)
		function_return_kind = return_destructor;
	else if (!Proto->RetType || Proto->RetType->type->isVoidTy())
		function_return_kind = return_void;
	else if (!Proto->returnName.empty())
		function_return_kind = return_variable;
	else
		function_return_kind = return_expr;
	current_branch_part.push_back(branch_part_t{0});
	prompt_indent++;
	auto sz = Proto->Args.size();
	// initialize local vars lookup table with function arguments
	for (int i=0; i<sz; i++) {
		FullVar fv = {
			.decl_loc = Proto->ArgPos[i],
			.ft = *Proto->ArgTypes[i]
		};
		if (i < Proto->ArgNeedsConstructor.size()
		    && Proto->ArgNeedsConstructor[i] != arg_is_borrowed_or_pod)
			fv.needs_constructor = &Proto->ArgNeedsConstructor[i];
		else
			fv.needs_constructor = nullptr;
		if (!fv.ft.type->isSized() || !TheModule->getDataLayout().getTypeAllocSize(fv.ft.type))
			if (!(fv.ft.type_attr & A_ref))
				fv.ft.type_attr |= (A_immutable | A_ref);
		bool is_new = locals_table.back().insert(Proto->Args[i].c_str(), fv);
		if (!is_new) {
			errs() << "duplicat function arg '" << Proto->Args[i] << "'\n";
			prompt_indent = 0;
			return nullptr;
		}
	}
	if (!Proto->returnName.empty()) {
		if (Proto->RetType && !Proto->RetType->type->isVoidTy()) {
			FullVar fv = {
				.decl_loc = Proto->retLoc,
				.ft = *Proto->RetType
			};
			bool is_new = locals_table.back().insert(Proto->returnName.c_str(), fv);
			if (!is_new) {
				errs() << Proto->retLoc << "cannot declare return variable '" << Proto->returnName << "'\n";
				prompt_indent = 0;
				return nullptr;
			}
		} else {
			errs() << Proto->retLoc << ": internal error - named return but no return type\n";
		}
	}
	auto ProtoRef = Proto.get();
	std::string unmangledName = Proto->getName();
	setMangledName(ProtoRef, visibility);
	auto previous_def = defined_functions.find(Proto->Name);
	if (previous_def != defined_functions.end()) {
		errs() << CurLoc << ": a function with the same name and the same signature has already been defined\n";
		errs() << previous_def->second << ": this is the location of the prefious definition\n";
		prompt_indent = 0;
		return nullptr;
	}
	if (visibility & A_constructor) {
		if (Proto->ArgTypes.size() == 1 && (!Proto->RetType || Proto->RetType->type->isVoidTy()) && Proto->returnName.empty()) // default constructor
			AutoMethods[Proto->ArgTypes[0]->mangled_name].first = Proto->Name;
		else if (!(Proto->visibility & A_conversion) && Proto->RetType && !Proto->RetType->type->isVoidTy()) {
			Proto->visibility &= ~A_method;
			if (!check_and_add_proto(lex.module->FunctionProtos[unmangledName], std::move(Proto), unmangledName))
				return nullptr;
			goto parse_body;
		} else
			Conversions[Proto->Name] = Proto->FT;
	} else if (visibility & A_destructor)
		AutoMethods[Proto->ArgTypes[0]->mangled_name].second = Proto->Name;
	if (Proto->visibility & A_method) {
		std::string mangled_receiver_type;
		if (Proto->ArgTypes[0]->type->isStructTy())
			mangled_receiver_type = Proto->ArgTypes[0]->mangled_name;
		else if (Proto->ArgTypes[0]->type_attr & A_complex)
			mangled_receiver_type = "3c32";
		else {
			errs() << Proto->retLoc << ": cannot declare method for type '" << *Proto->ArgTypes[0] << "'\n";
			return nullptr;
		}
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
parse_body:
	auto [list, e_kind, level] = ParseExprList();
	if (level) {
		errs() << CurLoc << ": function body contains invalid 'brk' statement(s)\n";
		return nullptr;
	}
	std::pair<std::vector<std::unique_ptr<ExprAST>>, int> Elist = { std::move(list), e_kind };
	if (!Elist.second && Elist.first.empty())
		return nullptr;
	if (Elist.second == tok_return) {
		while (CurTok.kind == ';')
			getNextToken();
		if (CurTok.kind != tok_end) {
			errs() << CurLoc << ": 'end' expected\n";
			return nullptr;
		}
		if (!ProtoRef->returnName.empty()) {
			errs() << Elist.first.back()->Loc << ": return statement at end of constructor or function with named return not allowed\n";
			return nullptr;
		}
	} else {
		if (Elist.second != tok_end) {
			errs() << CurLoc << ": 'return' or 'end' expected\n";
			return nullptr;
		}
		if (!ProtoRef->RetType->type->isVoidTy() && ProtoRef->returnName.empty()) {
			if (auto if_expr = dynamic_cast<IfExprAST*>(Elist.first.back().get())) {
				if (!if_expr->always_return) {
					errs() << Elist.first.back()->Loc << ": non-void function does not return a value in all branches and has no final return\n";
					return nullptr;
				}
			} else {
				errs() << Elist.first.back()->Loc << ": no return statement at end of non-void function\n";
				return nullptr;
			}
		}
	}
	prompt_indent = 0;
	unsigned b_lev = 1;
	std::map<std::string,FullVar*> destr_vars = get_destruct_vars(b_lev);
	int end_knd = Elist.second;
	BranchDescription bBranch = {
		std::move(Elist.first),
		BreakDescription{
			.vars_to_destruct = std::move(destr_vars),
			.end_kind = end_knd,
			.break_level = b_lev
		}
	};
	current_branch_part.clear();
	return std::make_unique<FunctionAST>(ProtoRef, std::move(bBranch), std::move(unmangledName));
}

std::pair<std::unique_ptr<ExprAST>,int> GetTopLevelExpression(unsigned sym_kind) {
	auto E = ParseExprOrReturn();
	if (E.first) {
		if (!E.first->ft || !E.first->ft->type) {
			if (auto B = dynamic_cast<BinaryExprAST*>(E.first.get())) {
				if (B->err_msg)
					return { AutoErr(B->Loc, B->LHS->ft->type, B->RHS->ft->type, B->LHS->ft->type_attr, B->RHS->ft->type_attr, B->err_msg), 0 };
				if (B->opclass == OpDeclAssign) {
					if (!strcmp(B->Op, ":="))
						sym_kind |= A_rvalue;
					if (jit_repl || (sym_kind & A_globally_visible)) {
						auto uB = std::unique_ptr<BinaryExprAST>(B);
						E.first.release();
						HandleGlobalVariable(std::move(uB), sym_kind);
						return { nullptr, 0 };
					}
					else
						return E;
				}
				errs() << E.first->Loc << ' ' << B->Op << ": Cannot evaluate expression\n";
				return { nullptr, 0 };
			} else {
				errs() << E.first->Loc << ": indeterminate expression\n";
				return { nullptr, 0 };
			}
		}
		return E;
	} else {
		return { nullptr, 0};
	}
}

std::unique_ptr<ExprAST> GenerateResultPrint(std::unique_ptr<ExprAST> E) {
	std::string print = "print";
	auto print_proto = lex.findProtos(print);
	if (!print_proto) {
		errs() << "Fatal error: could not find 'print' function\n";
		return nullptr;
	}
	// long long w = 0LL;
	auto FnLoc = E->Loc;
	auto volvox_print = std::make_unique<FunctionExprAST>(FnLoc, print, print_proto);
	std::vector<std::unique_ptr<ExprAST>> PrintArgs;
	PrintArgs.push_back(std::move(E));
	auto print_call = std::make_unique<CallExprAST>(FnLoc, std::move(volvox_print), std::move(PrintArgs));
	auto success = std::make_unique<BinaryExprAST>(
		FnLoc, ">=", std::move(print_call), std::move(std::make_unique<LiteralExprAST>(Token(0LL))),
		std::tuple<llvm::Type*, unsigned, bool, OpClass, const char*>{ llvm_bool_type, 0, false, OpComparison, nullptr });
	return success;
}

std::unique_ptr<FunctionAST> ParseTopLevelExpr(std::pair<std::unique_ptr<ExprAST>,int> E, bool suppress_output, bool is_bool) {
	SourceLocation FnLoc = E.first->Loc;
	if (comp_mode == comp_jit)
		finishFunctionOrModule();
	// Make an anonymous proto.
	volvoxc::FullType* TheType = is_bool ? bool_type : integer_type;
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
	std::map<std::string,FullVar*> destr_vars;
	if (E.second == tok_return) {
		ExprList.push_back(std::move(E.first));
		destr_vars = get_destruct_vars_main();
	} else {
		if (!do_pres) {
			ExprList.push_back(std::move(E.first));
			ExprList.push_back(std::move(std::make_unique<LiteralExprAST>(Token((long long)JIT_SUCCESS_MAGIC))));
		} else if (E.first->ft->type->isVoidTy() || suppress_output) {
			ExprList.push_back(std::move(E.first));
			if (!suppress_output)
				ExprList.push_back(std::move(std::make_unique<LiteralExprAST>(Token((long long)JIT_SUCCESS_MAGIC))));
			return_val_idx = ExprList.size() - 1;
		} else {
			auto print_call = GenerateResultPrint(std::move(E.first));
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
			}
		}
		ExprList.push_back(std::move(std::make_unique<LiteralExprAST>(Token((long long)JIT_SUCCESS_MAGIC))));
	}
	BranchDescription bBranch = {
		std::move(ExprList),
		BreakDescription{
			.vars_to_destruct = std::move(destr_vars),
			.end_kind = tok_return,
			.break_level = 1
		}
	};
	auto ProtoRef = Proto.get();
	std::string unmangledName = Proto->getName();
	lex.module->FunctionProtos[unmangledName].push_back(std::move(Proto));
	return std::make_unique<FunctionAST>(ProtoRef, std::move(bBranch), std::move(unmangledName), return_val_idx);
}

/// external := 'extern' prototype
std::unique_ptr<PrototypeAST> ParseExtern(unsigned& visibility) {
	visibility |= (A_extern | A_pub);
	return ParsePrototype(visibility);
}
