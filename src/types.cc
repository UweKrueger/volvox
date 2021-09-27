#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"

//===----------------------------------------------------------------------
// type conversion handling
//===----------------------------------------------------------------------

std::nullptr_t Error(SourceLocation Loc, const char *Str, ...) {
	fprintf(stderr, "Error (%d, %d)", Loc.Line, Loc.Col);
	va_list ap;
	va_start(ap, Str);
	vfprintf(stderr, Str, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	return nullptr;
}
	
std::nullptr_t AutoErr(SourceLocation Loc, llvm::Type* expr_type, llvm::Type* desired_type,
                              unsigned expr_attr, unsigned desired_attr, const char* reason) {
	return Error(Loc, "Cannot automatically convert %s to %s (%s)",
	             type_table.get_name((llvm::Type*)((uintptr_t)expr_type | (expr_attr & A_signed))),
	             type_table.get_name((llvm::Type*)((uintptr_t)desired_type | (desired_attr & A_signed))), reason);
}

static std::nullptr_t ExplicitErr(SourceLocation Loc, llvm::Type* expr_type, llvm::Type* desired_type,
                                  unsigned expr_attr, unsigned desired_attr, const char* reason) {
	return Error(Loc, "Cannot convert %s to %s (%s)",
	             type_table.get_name((llvm::Type*)((uintptr_t)expr_type | (expr_attr & A_signed))),
	             type_table.get_name((llvm::Type*)((uintptr_t)desired_type | (desired_attr & A_signed))), reason);
}

static llvm::Value* NoConversion(llvm::Value* v) { return v; }

std::pair<unsigned, bool> getBitWidth(llvm::Type* type) {
	switch(type->getTypeID()) {
	case llvm::Type::IntegerTyID:
		return { type->getIntegerBitWidth(), false };
	case llvm::Type::BFloatTyID:
		return { 8, true };
	case llvm::Type::FloatTyID:
		return { 24, true };
	case llvm::Type::DoubleTyID:
		return { 53, true };
	default:
		return { 64, false };
	}
}
			
static llvm::Type* getFittingType(unsigned bitwidth, bool is_float = false) {
	if (is_float)
		if (bitwidth > 8)
			if (bitwidth > 24)
				return llvm::Type::getDoubleTy(*Context.getContext());
			else
				return llvm::Type::getFloatTy(*Context.getContext());
		else
			return llvm::Type::getBFloatTy(*Context.getContext());
	else
		return llvm::IntegerType::get(*Context.getContext(), bitwidth);
}

static std::function<llvm::Value*(llvm::Value*)> getCast(llvm::Type* target_type, unsigned bitwidth, bool is_float, bool is_signed) {
	if (target_type->isIntegerTy())
		if (is_float) {
			fprintf(stderr, "internal compiler error: automatic cast float->int\n");
			return nullptr;
		}
		else
			if (target_type->getIntegerBitWidth() == bitwidth)
				return nullptr;
			else
				return [=](llvm::Value* v) { return Builder->CreateIntCast(v, target_type, is_signed, is_signed ? "intscasttmp" : "intucasttmp"); };
	else
		if (is_float)
			if (getFittingType(bitwidth, true) == target_type)
				return nullptr;
			else
				return [=](llvm::Value* v) { return Builder->CreateFPCast(v, target_type, "fpcasttmp"); };
		else
			if (is_signed)
				return [=](llvm::Value* v) { return Builder->CreateSIToFP(v, target_type, "sitofptmp"); };
			else
				return [=](llvm::Value* v) { return Builder->CreateUIToFP(v, target_type, "uitofptmp"); };
}

// is the definition area bigger (not the precision)
// input: type, is_signed
// results: b fits completely, b fits with presision loss
std::pair<bool, bool> analyze_types(std::pair<llvm::Type*, bool> a, std::pair<llvm::Type*, bool> b) {
	auto a_id = a.first->getTypeID();
	auto b_id = b.first->getTypeID();
	auto a_descr = getBitWidth(a.first);
	auto b_descr = getBitWidth(b.first);
	// signed type have a slightly smaller effective bitwidth
	unsigned a_bitwidth = a.second ? (a_descr.first - 1) : a_descr.first;
	unsigned b_bitwidth = b.second ? (b_descr.first - 1) : b_descr.first;
	// cannot convert a signed to an unsigned
	bool ill_i_u = (!b_descr.second && !b.second) && a.second;
	// cannot convert float to int
	bool ill_f_i = a_descr.second && !b_descr.second;
	// source exceeds target range
	bool overflow = a_bitwidth > b_bitwidth && !(b_descr.second && !a_descr.second);
	bool convertable = !overflow && !ill_f_i && !ill_f_i;
	return { convertable && b_bitwidth <= a_bitwidth, convertable };
}

// Try to convert 'expr_type' to 'desired_type'
// return an error if not possible or no explicit conversion
// is requested but precision would be lost
std::function<llvm::Value*(llvm::Value*)> getConv(
	llvm::Type* expr_type, llvm::Type* desired_type, unsigned expr_attr, unsigned desired_attr,
	SourceLocation Loc = CurLoc, bool is_explicit = false)
{
	const char* reason = "";
	if (desired_type->isIntegerTy()) {
		unsigned desired_bitwidth = desired_type->getIntegerBitWidth();
		if (expr_type->isIntegerTy()) {
			unsigned expr_bitwidth = expr_type->getIntegerBitWidth();
			if (!(desired_attr & A_signed))
				if (expr_attr & A_signed)
					// signed -> unsigned
					if (!is_explicit)
						return AutoErr(Loc, expr_type, desired_type, expr_attr, desired_attr, "signed->unsigned");
					else
						if (desired_bitwidth == expr_bitwidth)
							return NoConversion;
						else
							// design decision: make this a signed conversion so that `u64(-1) -> 0xffffffffffffffff`
							return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, true, "convstmp"); };
				else
					// unsigned -> unsigned
					if (desired_bitwidth < expr_bitwidth)
						if (is_explicit)
							return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, false, "trunctmp"); };
						else
							return AutoErr(Loc, expr_type, desired_type, expr_attr, desired_attr, "would truncate upper bits");
					else
						if (desired_bitwidth == expr_bitwidth)
							return NoConversion;
						else
							return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, false, "expandutmp"); };
			else
				if (expr_attr & A_signed)
					// signed -> signed
					if (desired_bitwidth < expr_bitwidth)
						if (is_explicit)
							return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, true, "trunctmp"); };
						else
							return AutoErr(Loc, expr_type, desired_type, expr_attr, desired_attr, "would truncate upper bits");
					else
						return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, true, "expandstmp"); };
				else
					// unsigned -> signed
					if (desired_bitwidth <= expr_bitwidth)
						if (is_explicit)
							if (desired_bitwidth == expr_bitwidth)
								return NoConversion;
							else
								return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, false, "trunctmp"); };
						else
							return AutoErr(Loc, expr_type, desired_type, expr_attr, desired_attr, "would truncate/reinterpret upper bits");
					else
						return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, false, "expandstmp"); };
		}
	}
	return NoConversion;		
}

inline static unsigned max(unsigned a, unsigned b) { return (a > b) ? a : b; }

std::tuple<llvm::Type*, llvm::Type*, unsigned> getResType(llvm::Type* left_type, llvm::Type* right_type, unsigned left_attr, unsigned right_attr, const char* Op) {
	auto left_descr = getBitWidth(left_type);
	unsigned left_bitwidth = left_descr.first;
	bool left_is_float = left_descr.second;
	bool left_is_signed = left_attr & A_signed;
	auto right_descr = getBitWidth(right_type);
	unsigned right_bitwidth = right_descr.first;
	bool right_is_float = right_descr.second;
	bool right_is_signed = right_attr & A_signed;
	unsigned res_bitwidth_min = max(right_bitwidth, left_bitwidth);
	unsigned res_bitwidth = res_bitwidth_min; // will be refined based on operator
	unsigned res_is_float = left_is_float || right_is_float;
	bool res_is_signed = !res_is_float && ((left_attr & A_signed ) || (right_attr & A_signed));
	// in simple cases one operand is converted to the type of the other
	// here we calculate the ideal result bitwidth to prevent data loss due to overflow
	if (Op[1] == '=') {
		if (Op[0] == '>' || Op[0] == '<')
			goto comparison;
		res_bitwidth = left_bitwidth;
		if (right_bitwidth > left_bitwidth || !left_is_float && (right_is_float || !left_is_signed && right_is_signed ||
		                                                        left_is_signed && !right_is_signed && right_bitwidth >= left_bitwidth))
			goto prec_err;
		goto calc_types;
	}
	switch (Op[0]) {
	case '+':
	case '-':
		res_bitwidth++;
		break;
	case '*':
		if (Op[1] != '*') {
			res_bitwidth = left_bitwidth + right_bitwidth;
			goto calc_types;
		}
		// fallthrough for **
	case '/':
	case '%':
		res_bitwidth = left_bitwidth;
		break;
	case '>':
	case '<':
	case '=':
	comparison:
		res_bitwidth_min = res_bitwidth = 1;
		if (Op[0] == '=') {
			// this is an assignment by default, i.e. if no bool result is expected
			res_bitwidth_min = left_bitwidth;
			res_is_signed = left_is_signed;
		}
		break;
	case '&':
	case '|':
	case '^':
	default:
		;
	}
calc_types:
	return { getFittingType(res_bitwidth_min, res_is_float), getFittingType(res_bitwidth, res_is_float), res_is_signed };
prec_err:
	fprintf(stderr,  "illegal usage of %s: RHS would degrade\n", Op);
	return { nullptr, nullptr, 0 };
}

// compute the conversion functions for binary Operators
BinOpConvSet convBinOp(llvm::Type* left_type, llvm::Type* right_type, unsigned left_attr, unsigned right_attr,
                       const char* Op, SourceLocation Loc)
{
	// no result type known - deduce from "biggest" operand
	auto left_descr = getBitWidth(left_type);
	unsigned left_bitwidth = left_descr.first;
	bool left_is_float = left_descr.second;
	auto right_descr = getBitWidth(right_type);
	unsigned right_bitwidth = right_descr.first;
	bool right_is_float = right_descr.second;
	unsigned res_bitwidth_min = max(right_bitwidth, left_bitwidth);
	unsigned res_bitwidth = res_bitwidth_min; // will be refined based on operator
	unsigned res_is_float = left_is_float || right_is_float;
	
	// in simple cases one operand is converted to the type of the other
	// here we calculate the ideal result bitwidth to prevent data loss due to overflow
	switch (Op[0]) {
	case '*':
		switch(Op[1]) {
		case '\0': // a * b
			res_bitwidth = left_bitwidth + right_bitwidth; // for ideal result type without overflow
			break;
		case '*': // a ** b
			res_bitwidth = 64;
			break;
		default:
			// TODO: handle +=, *=, ...
			fprintf(stderr, "internal error\n");
		}
		break;
	case '/':
	case '%':
		res_bitwidth = left_bitwidth;
		break;
	case '+':
	case '-':
		res_bitwidth = ((left_bitwidth > right_bitwidth) ? left_bitwidth : right_bitwidth) + 1;
		break;
	case '|':
	case '&':
	case '^':
		switch(Op[1]) {
		case '\0':
			res_bitwidth = ((left_bitwidth > right_bitwidth) ? left_bitwidth : right_bitwidth);
			break;
		case '=':
			res_bitwidth = left_bitwidth;
			break;
		case '&':
		case '|': // &&, ||, &&=, ||=
			if (left_bitwidth != 1 || right_bitwidth != 1)
				return {{ nullptr, nullptr, nullptr, 0, "boolean operands expected" }, { nullptr, nullptr, nullptr, 0, "boolean operands expected" }};
			else
				res_bitwidth = 1;
			break;
		}
		break;
	case '=':
		switch(Op[1]) {
		case '\0':
			res_bitwidth = left_bitwidth;
			break;
		case '=':
			res_bitwidth = 1;
			break;
		default:
			fprintf(stderr, "%s-Operator not implemented, yet\n", Op);
		}
		break;
	case '<':
	case '>':
		res_bitwidth = 1;
		break;
	default:
		fprintf(stderr, "%s-Operator not implemented, yet\n", Op);
	}
	switch (left_type->getTypeID()) {
	case llvm::Type::IntegerTyID:
		switch (right_type->getTypeID()) {
		case llvm::Type::IntegerTyID:
			if (left_attr & A_signed)
				if (right_attr & A_signed)
					// signed # signed
					if (left_bitwidth == right_bitwidth)
						return {{ nullptr, nullptr, left_type, A_signed }, { nullptr, nullptr, nullptr, 0 }};
					else if (left_bitwidth > right_bitwidth)
						return {{ nullptr,
								[=](llvm::Value* v) { return Builder->CreateIntCast(v, left_type, true, "expandstmp"); },
								left_type, A_signed }, { nullptr, nullptr, nullptr, 0 }};
					else
						return {{
								[=](llvm::Value* v) { return Builder->CreateIntCast(v, right_type, true, "expandstmp"); },
								nullptr, right_type, A_signed }, { nullptr, nullptr, nullptr, 0 }};
				else
					// signed # unsigned
					if (left_bitwidth > right_bitwidth)
						return {{ nullptr,
								[=](llvm::Value* v) { return Builder->CreateIntCast(v, left_type, false, "expandstmp"); },
								left_type, A_signed }, { nullptr, nullptr, nullptr, 0 }};
					else
						return {{ nullptr, nullptr, nullptr, 0, "would truncate/reinterpret upper bits" }, { nullptr, nullptr, nullptr, 0 }};
			else
				if (right_attr & A_signed)
					// unsigned # signed
					if (left_bitwidth < right_bitwidth)
						return {{
								[=](llvm::Value* v) { return Builder->CreateIntCast(v, right_type, false, "expandstmp"); },
								nullptr, right_type, A_signed }, { nullptr, nullptr, nullptr, 0 }};
					else
						return {{ nullptr, nullptr, nullptr, 0, "would truncate/reinterpret upper bits" }, { nullptr, nullptr, nullptr, 0 }};
				else
					// unsigned # unsigned
					if (left_bitwidth == right_bitwidth)
						return {{ nullptr, nullptr, left_type, 0 }, { nullptr, nullptr, nullptr, 0 }};
					else if (left_bitwidth > right_bitwidth)
						return {{ nullptr,
								[=](llvm::Value* v) { return Builder->CreateIntCast(v, left_type, true, "expandstmp"); },
								left_type, 0 }, { nullptr, nullptr, nullptr, 0 }};
					else
						return {{
								[=](llvm::Value* v) { return Builder->CreateIntCast(v, right_type, true, "expandstmp"); },
								nullptr, right_type, 0 }, { nullptr, nullptr, nullptr, 0 }};
		case llvm::Type::HalfTyID:
			right_bitwidth = 11;
			goto right_real;
		case llvm::Type::BFloatTyID:
			right_bitwidth = 8;
			goto right_real;
		case llvm::Type::FloatTyID:
			right_bitwidth = 24;
			goto right_real;
		case llvm::Type::DoubleTyID:
			right_bitwidth = 53;
		right_real:
			if (right_bitwidth >= (left_bitwidth - (left_attr & A_signed)))
				if (left_attr & A_signed)
					return {{ [=](llvm::Value* v) { return Builder->CreateSIToFP(v, right_type, "convsrealtmp"); },
							nullptr, right_type, 0 }, { nullptr, nullptr, nullptr, 0 }};
				else
					return {{ [=](llvm::Value* v) { return Builder->CreateUIToFP(v, right_type, "convurealtmp"); },
							nullptr, right_type, 0 }, { nullptr, nullptr, nullptr, 0 }};
			else
				return {{ nullptr, nullptr, nullptr, 0, "int->float would lose precision" }, { nullptr, nullptr, nullptr, 0 }};
			break;
		default:
			return {{ nullptr, nullptr, nullptr, 0, "no known automatic conversion" }, { nullptr, nullptr, nullptr, 0 }};
		}
	case llvm::Type::HalfTyID:
		left_bitwidth = 11;
		goto left_real;
	case llvm::Type::BFloatTyID:
		left_bitwidth = 8;
		goto left_real;
	case llvm::Type::FloatTyID:
		left_bitwidth = 24;
		goto left_real;
	case llvm::Type::DoubleTyID:
		left_bitwidth = 53;
	left_real:
		switch (right_type->getTypeID()) {
		case llvm::Type::IntegerTyID:
			right_bitwidth = right_type->getIntegerBitWidth();
			if (left_bitwidth >= (right_bitwidth - (right_attr & A_signed)))
				if (right_attr & A_signed)
					return {{ nullptr, [=](llvm::Value* v) { return Builder->CreateSIToFP(v, left_type, "convsrealtmp"); },
							left_type, 0 }, { nullptr, nullptr, nullptr, 0 }};
				else
					return {{ nullptr, [=](llvm::Value* v) { return Builder->CreateUIToFP(v, left_type, "convurealtmp"); },
							left_type, 0 }, { nullptr, nullptr, nullptr, 0 }};
			else
				return {{ nullptr, nullptr, nullptr, 0, "int->float would lose precision" }, { nullptr, nullptr, nullptr, 0 }};
		case llvm::Type::HalfTyID:
			right_bitwidth = 11;
			goto right_real2;
		case llvm::Type::BFloatTyID:
			right_bitwidth = 8;
			goto right_real2;
		case llvm::Type::FloatTyID:
			right_bitwidth = 24;
			goto right_real2;
		case llvm::Type::DoubleTyID:
			right_bitwidth = 53;
		right_real2:
			if (right_bitwidth == left_bitwidth)
				return {{ nullptr, nullptr, left_type, 0 }, { nullptr, nullptr, nullptr, 0 }};
			else if (left_bitwidth > right_bitwidth)
				return {{ nullptr,
						[=](llvm::Value* v) { return Builder->CreateFPCast(v, left_type, "fpcasttmp"); },
						left_type, 0 }, { nullptr, nullptr, nullptr, 0 }};
			else
				return {{ [=](llvm::Value* v) { return Builder->CreateFPCast(v, right_type, "fpcasttmp"); }, nullptr, right_type, 0 }, { nullptr, nullptr, nullptr, 0 }};
		default:
			return {{ nullptr, nullptr, nullptr, 0, "no known automatic conversion" }, { nullptr, nullptr, nullptr, 0 }};
		}
	default:
		return {{ nullptr, nullptr, nullptr, 0, "no known automatic conversion" }, { nullptr, nullptr, nullptr, 0 }};
	}
}

