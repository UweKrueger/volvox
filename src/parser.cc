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

Token getNextToken(bool expectBinary) { return CurTok = lex.gettok(expectBinary); }
Token purgeLine() { return CurTok = lex.purge_line(); }

/// GetTokPrecedence - Get the precedence of the pending binary operator token.
static inline int GetTokPrecedence() {
	return (CurTok.kind < 0 && CurTok.kind > tok_last_op) ? (-CurTok.kind) << 8 : -256;
}

/// LogError* - These are little helper functions for error handling.
std::unique_ptr<ExprAST> LogErrorGen(const char *Str, va_list ap) {
	eprt("Error: ");
	veprt(Str, ap);
	eprt("\n");
	return nullptr;
}

std::unique_ptr<ExprAST> LogError(const char *Str, ...) {
	va_list ap;
	va_start(ap, Str);
	LogErrorGen(Str, ap);
	va_end(ap);
	return nullptr;
}

static std::unique_ptr<PrototypeAST> LogErrorP(const char *Str, ...) {
	va_list ap;
	va_start(ap, Str);
	LogErrorGen(Str, ap);
	va_end(ap);
	return nullptr;
}

static bool Expect(int tok, bool expectBinary = false) {
	bool res = CurTok.kind == tok;
	if (res) {
		getNextToken(expectBinary);
	} else {
		LogErrorP("unexpected `%s` - expected `%s`", CurTok.str().c_str(), Token::tokName(tok).c_str());
	}
	return res;
}

static void Eat(int tok, bool expectBinary = false) {
	if (CurTok.kind == tok) {
		getNextToken(expectBinary);
	}
}

static std::unique_ptr<ExprAST> ParseExpression(llvm::Type* desired_type = nullptr,
                                                unsigned desired_attrib = 0u);

volvox::FullType* ParseType(bool allow_attribute) {
	unsigned attribs = 0;
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
		if (attribs)
			getNextToken();
		switch (CurTok.kind) {
		case '[': {
			getNextToken();
			int64_t dim = -1;
			if (CurTok.kind == ']') {
				getNextToken();
			} else {
				auto e = ParseExpression(llvm::Type::getInt64Ty(*Context.getContext()), A_signed);
				if (auto Dim = e->codegen()) {
					if (llvm::ConstantInt* d = llvm::dyn_cast<llvm::ConstantInt>(Dim)) {
						dim = d->getSExtValue();
					} else {
						LogErrorP("dimension must be constant int");
						return nullptr;
					}
				} else {
					LogErrorP("cannot parse dimension expression");
					return nullptr;
				}
				if (!Expect(']'))
					return nullptr;
				if (dim <= 0 || dim > INT_MAX) {
					LogErrorP("dimension must be a positive int (not %lld)", dim);
					return nullptr;
				}
			} 
			auto elem_type = ParseType();
			if (!elem_type)
				return nullptr;
			llvm::Type* array_type;
			if (dim > 0) {
				array_type = llvm::ArrayType::get(elem_type->type, dim);
			} else {
				llvm::Type* ptr = llvm::PointerType::get(elem_type->type, 0);
				array_type = llvm::StructType::get(ptr, llvm_int_type);
			}
			void* array_elem_type = malloc(sizeof(volvox::FullType));
			memcpy(array_elem_type, &elem_type, sizeof(volvox::FullType));
			return new volvox::FullType{
				.type = array_type,
				// .nrows = (int)dim,
				.elem_type = (volvox::FullType*)array_elem_type
			};
		}
			break;
		case '{': {
			// struct type
			getNextToken();
			std::vector<std::string> FieldNames;
			std::vector<volvox::FullType*> FieldTypes;
			std::vector<llvm::Type*> LLVMFieldTypes;
			for (;;) {
				if (CurTok.kind != tok_identifier) {
					LogErrorP("Unexpected `%s` in struct declaration - field name expected\n", CurTok.str().c_str());
					return nullptr;
				}
				FieldNames.push_back(IdentifierStr);
				getNextToken();
				auto type = ParseType(true);
				if (!type) {
					LogErrorP("Unexpected `%s` in struct declaration - type name expected\n", CurTok.str().c_str());
					return nullptr;
				}
				FieldTypes.push_back(type);
				LLVMFieldTypes.push_back(type->type);
				getNextToken();
				if (CurTok.kind == '}')
					break;
				Eat(',', true);
			}
			getNextToken();
			llvm::Type* struct_type = llvm::StructType::get(*Context.getContext(), LLVMFieldTypes, (bool)(attribs & A_packed));
			MapNode* fields = map_string_new_map();
			for (int i=0; i<FieldNames.size(); i++) {
				MapNode* new_node = map_string_tag_insert(&fields, FieldNames[i].c_str(), i, MapValue{ .src_ptr = FieldTypes[i] }, 0, false);
				if (!new_node) {
					LogErrorP("Duplicat field name `%s` in struct declaration\n", FieldNames[i].c_str());
					return nullptr;
				}
			}
				
			return new_FullType(struct_type, attribs, nullptr /*DIType*/, FieldNames.size(), (volvox::FullType*)fields);
		}
			break;
		case '&':
			do {
				attribs = (attribs & 0xffff) | ((attribs & 0xffff0000) + 0x10000);
				getNextToken(true);
			} while (CurTok.kind == '&');
			if (CurTok.kind == tok_identifier)
				break;
			// else fallthough to error
		default:
			LogErrorP("Unexpected `%s` - type name expected", CurTok.str().c_str());
			return nullptr;
		}
		getNextToken(true);
	}
	auto type = type_table.get_full(IdentifierStr.c_str());
	if (!type) {
		LogErrorP("Unknown type `%s`", IdentifierStr.c_str());
		return nullptr;
	}
	//if (type.type_attr)
	//	attribs |= A_signed;
	return type;
}

/// numberexpr ::= number
static std::unique_ptr<ExprAST> ParseNumberExpr(llvm::Type* desired_type = nullptr,
                                                unsigned desired_attrib = 0u) {
	auto Result = std::make_unique<LiteralExprAST>(CurTok);
	getNextToken(true); // consume the number
	return Result;
}

static std::unique_ptr<ExprAST> ParseStringExpr() {
	auto Result = std::make_unique<LiteralExprAST>(CurTok);
	getNextToken(true); // consume the string
	return Result;
}

static std::unique_ptr<ExprAST> ParsePointerExpr() {
	auto Result = std::make_unique<LiteralExprAST>(CurTok);
	getNextToken(true); // consume the pointer
	return Result;
}

/// parenexpr ::= '(' expression ')'
static std::unique_ptr<ExprAST> ParseParenExpr(llvm::Type* desired_type = nullptr,
                                               unsigned desired_attrib = 0u) {
	getNextToken(); // eat (.
	auto V = ParseExpression();
	if (!V)
		return nullptr;

	if (CurTok.kind != ')')
		return LogError("expected ')'");
	getNextToken(true); // eat ).
	return V;
}

static std::vector<std::unique_ptr<ExprAST>> SplitExprList(std::unique_ptr<ExprAST> Arg) {
	std::vector<std::unique_ptr<ExprAST>> Args;
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

/// identifierexpr
///   ::= identifier
///   ::= identifier '(' expression* ')'
static std::unique_ptr<ExprAST> ParseIdentifierExpr(llvm::Type* desired_type = nullptr,
                                                    unsigned desired_attrib = 0u) {
	std::string IdName = IdentifierStr;

	SourceLocation LitLoc = CurLoc;

	getNextToken(true); // eat identifier.

	if (CurTok.kind != '(') { // Simple variable ref.
		auto var_expr = std::make_unique<VariableExprAST>(LitLoc, IdName);
		// if (!var_expr->type) // variable name not found
		//	return nullptr;
		return var_expr;
	}

	// Call.
	getNextToken(); // eat '('
	if (CurTok.kind != ')') {
		if (auto Arg = ParseExpression()) {
			Expect(')', true);
			auto Args = SplitExprList(std::move(Arg));
			auto call_expr = std::make_unique<CallExprAST>(LitLoc, IdName, std::move(Args));
			if (!call_expr || !call_expr->ft || !call_expr->ft->type) // Used to signal failure, e.g. IdName was not found
				return nullptr;
			return call_expr;
		} else {
			return nullptr;
		}
	} else {
		// no call arguments
		getNextToken(true); // eat ')';
		return std::make_unique<CallExprAST>(LitLoc, IdName);
	}
}

static std::unique_ptr<ExprAST> ParseAggregateExpr(llvm::Type* desired_type = nullptr,
                                                   unsigned desired_attrib = 0u) {
	bool is_dynamic;
	AggregateKind kind;
	TokenKind closing;
	switch (CurTok.kind) {
	case '{':
		is_dynamic = true;
		kind = AnyDyn;
		closing = (TokenKind)'}';
		break;
	case '[':
		is_dynamic = false;
		kind = AnyFixed;
		closing = (TokenKind)']';
		break;
	default:
		return LogError("AggregateExpr: unexpected \"%s%\" (expected '{' or '[')", CurTok.str().c_str());
	}
	SourceLocation loc = CurLoc;
	getNextToken(); // eat '{'/'['
	if (CurTok.kind == closing) {
		getNextToken(true);
		// TODO: parse type and given elements
		return std::make_unique<AggregateExprAST>(loc, kind);
	}
	if (auto Elem = ParseExpression()) {
		Expect(closing, true);
		auto Elems = SplitExprList(std::move(Elem));
		dprt("Array initialized with %d elements\n", Elems.size());
		if (auto bin_expr = dynamic_cast<BinaryExprAST*>(Elems[0].get())) {
			if (bin_expr->Op[0] == ':') { // struct or map
				if (auto ident = dynamic_cast<VariableExprAST*>(bin_expr->LHS.get())) {
					if (is_dynamic) {
						kind = Array;
						
					} else {
						kind = Struct;
					}
				} else if (auto key = dynamic_cast<LiteralExprAST*>(bin_expr->LHS.get())) {
					kind = is_dynamic ? Map : FixedArray;
				} else {
					return LogError("AggregateExpr: illegal expression before ':'");
				}
			}
		}
		return std::make_unique<AggregateExprAST>(loc, /* kind */ FixedArray, std::move(Elems));
	} else {
		return LogError("AggregateExpr: unexpected \"%s%\" (expected expression)", CurTok.str().c_str());
	}
}

static std::pair<std::vector<std::unique_ptr<ExprAST>>, int> ParseExprList(llvm::Type* desired_type, unsigned desired_attr);

/// ifexpr ::= 'if' expression 'then' expression 'else' expression
static std::unique_ptr<ExprAST> ParseIfExpr(llvm::Type* desired_type = nullptr,
                                            unsigned desired_attr = 0u) {
	SourceLocation IfLoc = CurLoc;

	getNextToken(); // eat the if.

	// condition - expect bool.
	auto Cond = ParseExpression(llvm::Type::getInt1Ty(*Context.getContext()));
	if (!Cond)
		return nullptr;

	if (CurTok.kind != tok_then)
		return LogError("expected then");
	getNextToken(); // eat the then

	auto Then = ParseExprList(desired_type, desired_attr);
	if (Then.first.size()) {
		desired_type = Then.first.back()->ft->type;
		desired_attr = Then.first.back()->ft->type_attr;
	} else {
		desired_type = nullptr;
		desired_attr = 0;
	}
	if (Then.second != tok_else)
		getNextToken();
	if (CurTok.kind != tok_else)
	 	return LogError("expected else");
	getNextToken();

	auto Else = ParseExprList(desired_type, desired_attr);
	if (Else.second == tok_end)
		getNextToken();
	return std::make_unique<IfExprAST>(IfLoc, std::move(Cond), std::move(Then.first),
	                                   std::move(Else.first), Then.second, Else.second);
}

/// forexpr ::= 'for' identifier '=' expr ',' expr (',' expr)? 'in' expression
static std::unique_ptr<ExprAST> ParseForExpr() {
	getNextToken(); // eat the for.

	if (CurTok.kind != tok_identifier)
		return LogError("expected identifier after for");

	std::string IdName = IdentifierStr;
	getNextToken(true); // eat identifier.

	if (CurTok.kind != tok_assign)
		return LogError("expected '=' after for");
	getNextToken(); // eat '='.

	auto Start = ParseExpression();
	if (!Start)
		return nullptr;
	if (CurTok.kind != ',')
		return LogError("expected ',' after for start value");
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

	if (CurTok.kind != tok_in)
		return LogError("expected 'in' after for");
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
static std::unique_ptr<ExprAST> ParsePrimary(llvm::Type* desired_type = nullptr,
                                             unsigned desired_attrib = 0u) {
	switch (CurTok.kind) {
	case tok_eof:
		eprt("EOF when expecting an expression\n");
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
	case '{':
	case '[':
		return ParseAggregateExpr();
	case tok_if:
		return ParseIfExpr();
	case tok_for:
		return ParseForExpr();
	default:
		auto err = LogError("unknown token %d '%s' when expecting an expression", CurTok.kind, CurTok.str().c_str());
		purgeLine();
		return err;
	}
}

/// unary
///   ::= primary
///   ::= '!' unary
static std::unique_ptr<ExprAST> ParseUnary(llvm::Type* desired_type = nullptr,
                                           unsigned desired_attrib = 0u) {
	// If the current token is not an operator, it must be a primary expr.
	if (CurTok.kind != tok_unary || CurTok.kind == '(' || CurTok.kind == ',')
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
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
                                              std::unique_ptr<ExprAST> LHS, llvm::Type* desired_type = nullptr, unsigned desired_attrib = 0) {
	// If this is a binop, find its precedence.
	while (true) {
		int TokPrec = GetTokPrecedence();
		// If this is a binop that binds at least as tightly as the current binop,
		// consume it, otherwise we are done.
		if (TokPrec < ExprPrec) {
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
		int NextPrec = GetTokPrecedence();
		if (TokPrec < NextPrec) {
			RHS = ParseBinOpRHS(TokPrec + ((CurTok.kind == tok_assign) ? -1 : 1), std::move(RHS));
			if (!RHS)
				return nullptr;
		}
		// Merge LHS/RHS.
		// save types befor objects are moved
		auto LHS_type = LHS->ft->type;
		auto LHS_attr = LHS->ft->type_attr;
		auto LHS_is_unknown_type = LHS->is_unknown_type;
		auto RHS_type = RHS->ft->type;
		auto RHS_attr = RHS->ft->type_attr;
		auto RHS_is_unknown_type = RHS->is_unknown_type;
		dprt("LHS: %s RHS: %s\n", type_table.get_name(LHS_type, LHS_attr & A_signed), type_table.get_name(RHS_type, RHS_attr & A_signed));
		if (inside_function && BinOp == ":=") {
			dprt("got :=\n");
			if (auto VarL = dynamic_cast<VariableExprAST*>(LHS.get())) {
				auto type_descr = MakeType(RHS_type, RHS_attr & A_signed, RHS_is_unknown_type);
				llvm::Type* type = std::get<0>(type_descr);
				bool is_signed = std::get<2>(type_descr);
				FullVar fv = {
					.ft = {
						.type = type,
						.type_attr = is_signed ? 1U : 0U
					}
				};
				if (!locals_table.back().insert(VarL->Name.c_str(), fv)) {
					eprt("variable %s already exists in current scope\n", VarL->Name.c_str());
					return nullptr;
				} else {
					dprt("inserted local %s\n", VarL->Name.c_str());
				}
			} else {
				eprt("left operand of \":=\" must be a variable\n");
				return nullptr;
			}
		} else if (!LHS_type) {
			if (auto V = dynamic_cast<VariableExprAST*>(LHS.get())) {
				if (BinOp[0] == '\0') {
					auto Args = SplitExprList(std::move(RHS));
					LHS = std::make_unique<CallExprAST>(V->Loc, V->Name, std::move(Args));
					continue;
				}
			}
		}
		LHS = std::make_unique<BinaryExprAST>(BinLoc, BinOp.c_str(), std::move(LHS), std::move(RHS),
		                                      convBinOp(LHS_type, RHS_type, LHS_attr, RHS_attr,
		                                                LHS_is_unknown_type, RHS_is_unknown_type, BinOp.c_str()),
		                                      desired_type, desired_attrib);
	}
}

/// expression
///   ::= unary binoprhs
///
static std::unique_ptr<ExprAST> ParseExpression(llvm::Type* desired_type, unsigned desired_attrib) {
	auto LHS = ParseUnary(desired_type, desired_attrib);
	if (!LHS)
		return nullptr;

	return ParseBinOpRHS(0, std::move(LHS), desired_type, desired_attrib);
}

static std::pair<std::unique_ptr<ExprAST>, int> ParseExprOrReturn(llvm::Type* desired_type, unsigned desired_attrib) {
	while (CurTok.kind == ';')
		getNextToken();
	auto kind = CurTok.kind;
	if (kind == tok_return || kind == tok_else || kind == tok_end) {
		if (kind == tok_return) {
			getNextToken();
			return { ParseExpression(desired_type, desired_attrib), kind };
		}
		else
			return { nullptr, kind };
	} else {
		return { ParseExpression(nullptr, 0), 0 };
	}
}

static std::pair<std::vector<std::unique_ptr<ExprAST>>, int> ParseExprList(llvm::Type* desired_type, unsigned desired_attr) {
	std::vector<std::unique_ptr<ExprAST>> expr_list;
	int end_kind = 0;
	while (!end_kind) {
		auto expr = ParseExprOrReturn(desired_type, desired_attr);
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
	std::vector<volvox::FullType*> ArgTypes;
	std::vector<llvm::Type*> LLVMArgTypes;
	bool is_method;
	bool isVarArgs = false;

	switch (CurTok.kind) {
	case '(': {
		is_method = true;
		unsigned attribs = 0;
		getNextToken();
		if (CurTok.kind != tok_identifier) {
			return LogErrorP("Unexpected `%s` in method prototype - receiver name expected", CurTok.str().c_str());
		}
		ArgNames.push_back(IdentifierStr);
		getNextToken();
		auto type = ParseType(true);
		if (!type->type) {
			return LogErrorP("Unexpected `%s` in method prototype - type name expected", CurTok.str().c_str());
		}
		ArgTypes.push_back(type);
		LLVMArgTypes.push_back(type->type);
		getNextToken();
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
		if (!isascii(CurTok.kind))
			return LogErrorP("Expected unary operator");
		FnName = "unary";
		FnName += (char)CurTok.kind;
		Kind = 1;
		getNextToken();
		break;
	case tok_colon: // ... not really
		getNextToken();
		if (!isascii(CurTok.kind))
			return LogErrorP("Expected binary operator");
		FnName = "binary";
		FnName += (char)CurTok.kind;
		Kind = 2;
		getNextToken();

		// Read the precedence if present.
		if (CurTok.kind == tok_number) {
			if (CurTok.Val.Int < 1 || CurTok.Val.Int > 100)
				return LogErrorP("Invalid precedence: must be 1..100");
			BinaryPrecedence = (unsigned)CurTok.Val.Int;
			getNextToken();
		}
		break;
	default:
		return LogErrorP("Expected function name in prototype");
	}
	if (!Expect('(')) return nullptr;
	if (CurTok.kind == ')')
		goto noargs;
	for (;;) {
		if (CurTok.kind != tok_identifier) {
			if (CurTok.kind == tok_ellipsis) {
				isVarArgs = true;
				getNextToken();
				if (CurTok.kind != ')')
					return LogErrorP("Unexpected `%s` after `...` - `)` expected\n", CurTok.str().c_str());
				else
					break;
			}
			return LogErrorP("Unexpected `%s` in function arg list - arg name expected\n", CurTok.str().c_str());
		}	
		ArgNames.push_back(IdentifierStr);
		getNextToken();
		auto type = ParseType(true);
		if (!type) {
			return LogErrorP("Unexpected `%s` in function arg list - type name expected\n", CurTok.str().c_str());
		}
		ArgTypes.push_back(type);
		LLVMArgTypes.push_back(type->type);
		getNextToken();
		if (CurTok.kind == ')')
			break;
		Eat(',');
	}
noargs:
	getNextToken(); // eat ')'.
	// parse return type(s)
	std::vector<volvox::FullType*> RetTypes;
	while (CurTok.kind != ';') {
		auto type = ParseType(true);
		if (!type)
			return LogErrorP("error parsing return type of function prototype");
		RetTypes.push_back(type);
		getNextToken(true);
	}
	getNextToken();
	// Verify right number of names for operator.
	if (Kind && ArgNames.size() != Kind)
		return LogErrorP("Invalid number of operands for operator");

	return std::make_unique<PrototypeAST>(FnLoc, FnName, ArgNames, Kind != 0, RetTypes, ArgTypes, LLVMArgTypes, isVarArgs);
}

/// definition ::= 'fn' prototype expression
std::unique_ptr<FunctionAST> ParseDefinition() {
	getNextToken(); // eat fn.
	auto Proto = ParsePrototype();
	if (!Proto)
		return nullptr;
	auto sz = Proto->Args.size();
	// initialize local vars lookup table with function arguments
	for (int i=0; i<sz; i++) {
		FullVar fv = {
			.ft = *Proto->ArgTypes[i]
		};
		bool is_new = locals_table.back().insert(Proto->Args[i].c_str(), fv);
		if (!is_new) {
			LogError("duplicat function arg \"%s\"\n", Proto->Args[i].c_str());
			return nullptr;
		}
	}
	auto ProtoRef = Proto.get();
	FunctionProtos[Proto->getName()] = std::move(Proto);
	std::pair<std::vector<std::unique_ptr<ExprAST>>, int> Elist = ParseExprList(ProtoRef->RetTypes[0]->type, ProtoRef->RetTypes[0]->type_attr);
	if (Elist.first.size()) {
		return std::make_unique<FunctionAST>(ProtoRef, std::move(Elist.first), Elist.second);
	}
	return nullptr;
}

std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
	SourceLocation FnLoc = CurLoc;
	if (auto E = ParseExpression()) {
		if (!E->ft || !E->ft->type) {
			if (auto B = dynamic_cast<BinaryExprAST*>(E.get())) {
				if (B->conv.compat.err_msg)
					return AutoErr(B->Loc, B->LHS->ft->type, B->RHS->ft->type, B->LHS->ft->type_attr, B->RHS->ft->type_attr, B->conv.compat.err_msg);
				if (!strcmp(B->Op, ":="))
					return HandleGlobalVariable(B);
			} else {
				eprt("Could not deduce type of expression\n");
				return nullptr;
			}
		}
		dprt("anonymous expression - TypeID: %u\n", E->ft->type->getTypeID());
		// Make an anonymous proto.
		uint64_t res_size = TheModule->getDataLayout().getTypeStoreSize(E->ft->type);
		if (res_size > 8) {
			uint64_t alloc_size = TheModule->getDataLayout().getTypeAllocSize(E->ft->type);
		}
		std::vector<volvox::FullType*> TheType = { type_table.get_full("bool") };
		auto Proto = std::make_unique<PrototypeAST>(FnLoc, "__anon_expr",
		                                            std::vector<std::string>(),
		                                            false, TheType);
		llvm::Constant* rttype_ptr = getRtType(E->ft);
		if (E->ft->type->isAggregateType())
			// pass by reference
			E = std::make_unique<UnaryExprAST>("&", std::move(E));
		std::string volvox_println = "_ZN6volvox7printlnEPKcPKNS_6RtTypeEz";
		std::vector<std::unique_ptr<ExprAST>> PrintArgs;
		PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token(std::string("Result: >")))));
		PrintArgs.push_back(std::move(std::make_unique<ConstExprAST>(rttype_ptr)));
		// println requires parameters for width, precision and flags - pass 0s (and signed bit) to get defaults
		PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token(0LL))));
		PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token(0LL))));
		PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token(0LL))));
		PrintArgs.push_back(std::move(E));
		PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token(std::string("<")))));
		PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token((void*)0))));
		auto print_call = std::make_unique<CallExprAST>(FnLoc, volvox_println, std::move(PrintArgs));
		std::vector<std::unique_ptr<ExprAST>> GlobalExprList;
		GlobalExprList.push_back(std::move(print_call));
		auto ProtoRef = Proto.get();
		FunctionProtos[Proto->getName()] = std::move(Proto);
		auto tmp_function = std::make_unique<FunctionAST>(ProtoRef, std::move(GlobalExprList), tok_return);
		return tmp_function;
	}
	return nullptr;
}

/// external ::= 'extern' prototype
std::unique_ptr<PrototypeAST> ParseExtern() {
	getNextToken(); // eat extern.
	return ParsePrototype();
}
