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
Token getNextToken(bool expectBinary) { return CurTok = lex.gettok(expectBinary); }

/// GetTokPrecedence - Get the precedence of the pending binary operator token.
static inline int GetTokPrecedence() {
	return (CurTok.type < 0 && CurTok.type > tok_last_op) ? (-CurTok.type) << 8 : -256;
}

/// LogError* - These are little helper functions for error handling.
std::unique_ptr<ExprAST> LogErrorGen(const char *Str, va_list ap) {
	fprintf(stderr, "Error: ");
	vfprintf(stderr, Str, ap);
	fprintf(stderr, "\n");
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
	bool res = CurTok.type == tok;
	if (res) {
		getNextToken(expectBinary);
	} else {
		LogErrorP("unexpected `%s` - expected `%s`", CurTok.str().c_str(), Token::tokName(tok).c_str());
	}
	return res;
}

static void Eat(int tok, bool expectBinary = false) {
	if (CurTok.type == tok)
		getNextToken(expectBinary);
}

std::pair<llvm::Type*, unsigned> ParseType() {
	unsigned attribs = 0;
	while (CurTok.type != tok_identifier) {
		switch (CurTok.type) {
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
		case '&':
			do {
				attribs = (attribs & 0xffff) | ((attribs & 0xffff0000) + 0x10000);
				getNextToken();
			} while (CurTok.type == '&');
			if (CurTok.type == tok_identifier)
				break;
			// else fallthough to error
		default:
			LogErrorP("Unexpected `%s` - type name expected", CurTok.str().c_str());
			return { nullptr, 0 };
		}
	}
	auto type = type_table.get_raw(IdentifierStr.c_str());
	if (!type) {
		LogErrorP("Unknown type `%s` %p", IdentifierStr.c_str(), type);
		return { nullptr, 0 };
	}
	return { type, attribs };
}

static std::unique_ptr<ExprAST> ParseExpression();

/// numberexpr ::= number
static std::unique_ptr<ExprAST> ParseNumberExpr() {
	auto Result = std::make_unique<LiteralExprAST>(CurTok);
	getNextToken(true); // consume the number
	return std::move(Result);
}

static std::unique_ptr<ExprAST> ParseStringExpr() {
	auto Result = std::make_unique<LiteralExprAST>(CurTok);
	getNextToken(true); // consume the string
	return std::move(Result);
}

/// parenexpr ::= '(' expression ')'
static std::unique_ptr<ExprAST> ParseParenExpr() {
	getNextToken(); // eat (.
	auto V = ParseExpression();
	if (!V)
		return nullptr;

	if (CurTok.type != ')')
		return LogError("expected ')'");
	getNextToken(true); // eat ).
	return V;
}

/// identifierexpr
///   ::= identifier
///   ::= identifier '(' expression* ')'
static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
	std::string IdName = IdentifierStr;

	SourceLocation LitLoc = CurLoc;

	getNextToken(true); // eat identifier.

	if (CurTok.type != '(') // Simple variable ref.
		return std::make_unique<VariableExprAST>(LitLoc, IdName);

	// Call.
	getNextToken(); // eat (
	std::vector<std::unique_ptr<ExprAST>> Args;
	if (CurTok.type != ')') {
		while (true) {
			if (auto Arg = ParseExpression())
				Args.push_back(std::move(Arg));
			else
				return nullptr;

			if (CurTok.type == ')')
				break;

			if (CurTok.type != ',')
				return LogError("Expected ')' or ',' in argument list");
			getNextToken();
		}
	}

	// Eat the ')'.
	getNextToken(true);

	return std::make_unique<CallExprAST>(LitLoc, IdName, std::move(Args));
}

/// ifexpr ::= 'if' expression 'then' expression 'else' expression
static std::unique_ptr<ExprAST> ParseIfExpr() {
	SourceLocation IfLoc = CurLoc;

	getNextToken(); // eat the if.

	// condition.
	auto Cond = ParseExpression();
	if (!Cond)
		return nullptr;

	if (CurTok.type != tok_then)
		return LogError("expected then");
	getNextToken(); // eat the then

	auto Then = ParseExpression();
	if (!Then)
		return nullptr;

	if (CurTok.type != tok_else)
		return LogError("expected else");

	getNextToken();

	auto Else = ParseExpression();
	if (!Else)
		return nullptr;

	return std::make_unique<IfExprAST>(IfLoc, std::move(Cond), std::move(Then),
									   std::move(Else));
}

/// forexpr ::= 'for' identifier '=' expr ',' expr (',' expr)? 'in' expression
static std::unique_ptr<ExprAST> ParseForExpr() {
	getNextToken(); // eat the for.

	if (CurTok.type != tok_identifier)
		return LogError("expected identifier after for");

	std::string IdName = IdentifierStr;
	getNextToken(true); // eat identifier.

	if (CurTok.type != tok_assign)
		return LogError("expected '=' after for");
	getNextToken(); // eat '='.

	auto Start = ParseExpression();
	if (!Start)
		return nullptr;
	if (CurTok.type != ',')
		return LogError("expected ',' after for start value");
	getNextToken();

	auto End = ParseExpression();
	if (!End)
		return nullptr;

	// The step value is optional.
	std::unique_ptr<ExprAST> Step;
	if (CurTok.type == ',') {
		getNextToken();
		Step = ParseExpression();
		if (!Step)
			return nullptr;
	}

	if (CurTok.type != tok_in)
		return LogError("expected 'in' after for");
	getNextToken(); // eat 'in'.

	auto Body = ParseExpression();
	if (!Body)
		return nullptr;

	return std::make_unique<ForExprAST>(IdName, std::move(Start), std::move(End),
										std::move(Step), std::move(Body));
}

/// varexpr ::= 'var' identifier ('=' expression)?
//                    (',' identifier ('=' expression)?)* 'in' expression
static std::unique_ptr<ExprAST> ParseVarExpr() {
	getNextToken(); // eat the var.

	std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames;

	// At least one variable name is required.
	if (CurTok.type != tok_identifier)
		return LogError("expected identifier after var");

	while (true) {
		std::string Name = IdentifierStr;
		getNextToken(true); // eat identifier.

		// Read the optional initializer.
		std::unique_ptr<ExprAST> Init = nullptr;
		if (CurTok.type == tok_assign) {
			getNextToken(); // eat the '='.

			Init = ParseExpression();
			if (!Init)
				return nullptr;
		}

		VarNames.push_back(std::make_pair(Name, std::move(Init)));

		// End of var list, exit loop.
		if (CurTok.type != ',')
			break;
		getNextToken(); // eat the ','.

		if (CurTok.type != tok_identifier)
			return LogError("expected identifier list after var");
	}

	// At this point, we have to have 'in'.
	if (CurTok.type != tok_in)
		return LogError("expected 'in' keyword after 'var'");
	getNextToken(); // eat 'in'.

	auto Body = ParseExpression();
	if (!Body)
		return nullptr;

	return std::make_unique<VarExprAST>(std::move(VarNames), std::move(Body));
}

/// primary
///   ::= identifierexpr
///   ::= numberexpr
///   ::= parenexpr
///   ::= ifexpr
///   ::= forexpr
///   ::= varexpr
static std::unique_ptr<ExprAST> ParsePrimary() {
	switch (CurTok.type) {
	default:
		return LogError("unknown token %d '%s' when expecting an expression", CurTok.type, CurTok.str().c_str());
	case tok_identifier:
		return ParseIdentifierExpr();
	case tok_number:
		return ParseNumberExpr();
	case tok_str_lit:
		return ParseStringExpr();
	case '(':
		return ParseParenExpr();
	case tok_if:
		return ParseIfExpr();
	case tok_for:
		return ParseForExpr();
	case tok_var:
		return ParseVarExpr();
	}
}

/// unary
///   ::= primary
///   ::= '!' unary
static std::unique_ptr<ExprAST> ParseUnary() {
	// If the current token is not an operator, it must be a primary expr.
	if (CurTok.type != tok_unary || CurTok.type == '(' || CurTok.type == ',')
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
                                              std::unique_ptr<ExprAST> LHS) {
	// If this is a binop, find its precedence.
	while (true) {
		int TokPrec = GetTokPrecedence();

		// If this is a binop that binds at least as tightly as the current binop,
		// consume it, otherwise we are done.
		if (TokPrec < ExprPrec)
			return LHS;

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
			RHS = ParseBinOpRHS(TokPrec + ((CurTok.type == tok_assign) ? -1 : 1), std::move(RHS));
			if (!RHS)
				return nullptr;
		}
		// Merge LHS/RHS.
		LHS = std::make_unique<BinaryExprAST>(BinLoc, BinOp.c_str(), std::move(LHS),
											  std::move(RHS));
	}
}

/// expression
///   ::= unary binoprhs
///
static std::unique_ptr<ExprAST> ParseExpression() {
	auto LHS = ParseUnary();
	if (!LHS)
		return nullptr;

	return ParseBinOpRHS(0, std::move(LHS));
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
	std::vector<llvm::Type*> ArgTypes;
	std::vector<unsigned> ArgAttribs;
	bool is_method;

	switch (CurTok.type) {
	case '(': {
		is_method = true;
		unsigned attribs = 0;
		getNextToken();
		if (CurTok.type != tok_identifier) {
			return LogErrorP("Unexpected `%s` in method prototype - receiver name expected", CurTok.str().c_str());
		}
		ArgNames.push_back(IdentifierStr);
		getNextToken();
		auto type = ParseType();
		if (!type.first) {
			return LogErrorP("Unexpected `%s` in method prototype - type name expected", CurTok.str().c_str());
		}
		ArgTypes.push_back(type.first);
		ArgAttribs.push_back(type.second);
		FullType full_type = {
			.type = type.first,
			.type_attr = type.second
		};
		getNextToken();
		Expect(')');
	}
	default:
		is_method = false;
	}
	switch (CurTok.type) {
	case tok_identifier:
		FnName = IdentifierStr;
		Kind = 0;
		fprintf(stderr, "fn declaration \"%s\"\n", FnName.c_str());
		getNextToken();
		break;
	case tok_unary:
		getNextToken();
		if (!isascii(CurTok.type))
			return LogErrorP("Expected unary operator");
		FnName = "unary";
		FnName += (char)CurTok.type;
		Kind = 1;
		getNextToken();
		break;
	case tok_colon: // ... not really
		getNextToken();
		if (!isascii(CurTok.type))
			return LogErrorP("Expected binary operator");
		FnName = "binary";
		FnName += (char)CurTok.type;
		Kind = 2;
		getNextToken();

		// Read the precedence if present.
		if (CurTok.type == tok_number) {
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
	if (CurTok.type == ')')
		goto noargs;
	for (;;) {
		if (CurTok.type != tok_identifier)
			return LogErrorP("Unexpected `%s` in function arg list - arg name expected", CurTok.str().c_str());
		fprintf(stderr, "fn arg: >%s<\n", CurTok.str().c_str());
		ArgNames.push_back(IdentifierStr);
		getNextToken();
		fprintf(stderr, "fn type: >%s<\n", CurTok.str().c_str());
		auto type = ParseType();
		if (!type.first) {
			return LogErrorP("Unexpected `%s` in function arg list - type name expected", CurTok.str().c_str());
		}
		ArgTypes.push_back(type.first);
		ArgAttribs.push_back(type.second);
		FullType full_type = {
			.type = type.first,
			.type_attr = type.second
		};
		getNextToken();
		if (CurTok.type == ')')
			break;
		Eat(tok_comma);
	}
noargs:
	getNextToken(); // eat ')'.

	// Verify right number of names for operator.
	if (Kind && ArgNames.size() != Kind)
		return LogErrorP("Invalid number of operands for operator");

	return std::make_unique<PrototypeAST>(FnLoc, FnName, ArgNames, Kind != 0, llvm::Type::getDoubleTy(*Context.getContext()), 0, ArgTypes, ArgAttribs);
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
		FullType ft = {
			.type = Proto->ArgTypes[i],
			.type_attr = Proto->ArgAttribs[i]
		};
		bool is_new = map_string_insert(&locals_table, Proto->Args[i].c_str(), (MapValue){ .src_ptr = &ft }, sizeof(FullType));
		if (!is_new) {
			LogError("duplicat function arg \"%s\"\n", Proto->Args[i].c_str());
			return nullptr;
		}
	}
	auto E = ParseExpression();
	if (E)
		return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
	return nullptr;
}

/// toplevelexpr ::= expression
std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
	SourceLocation FnLoc = CurLoc;
	if (auto E = ParseExpression()) {
		// Make an anonymous proto.
		char* name = nullptr;
		auto Proto = std::make_unique<PrototypeAST>(FnLoc, "__anon_expr",
													std::vector<std::string>(),
													false, E->type, E->type_attr);
		return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
	}
	return nullptr;
}

/// external ::= 'extern' prototype
std::unique_ptr<PrototypeAST> ParseExtern() {
	getNextToken(); // eat extern.
	return ParsePrototype();
}
