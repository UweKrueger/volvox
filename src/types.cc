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
	SourceLocation Loc, bool is_explicit)
{
	const char* reason = "";
	auto desired_descr = getBitWidth(desired_type);
	unsigned desired_bitwidth = desired_descr.first;
	bool float_desired = desired_descr.second;
	auto expr_descr = getBitWidth(expr_type);
	unsigned expr_bitwidth = expr_descr.first;
	bool float_expr = expr_descr.second;
	if (float_desired)
		if (float_expr)
			if (desired_bitwidth == expr_bitwidth)
				return NoConversion;
			else if (is_explicit || desired_bitwidth >= expr_bitwidth)
				return [=](llvm::Value* v) { return Builder->CreateFPCast(v, desired_type, "convfptmp"); };
			else
				return AutoErr(Loc, expr_type, desired_type, expr_attr, desired_attr, "float truncation");
		else
			if (is_explicit || desired_bitwidth >= expr_bitwidth)
				if (expr_attr & A_signed)
					return [=](llvm::Value* v) { return Builder->CreateSIToFP(v, desired_type, "convsfptmp"); };
				else
					return [=](llvm::Value* v) { return Builder->CreateUIToFP(v, desired_type, "convufptmp"); };
			else
				return AutoErr(Loc, expr_type, desired_type, expr_attr, desired_attr, "int->float would lose precision");
	else
		if (float_expr)
			if (is_explicit)
				if (desired_attr & A_signed)
					return [=](llvm::Value* v) { return Builder->CreateFPToSI(v, desired_type, "convfpstmp"); };
				else
					return [=](llvm::Value* v) { return Builder->CreateFPToUI(v, desired_type, "convfputmp"); };
			else 
				return AutoErr(Loc, expr_type, desired_type, expr_attr, desired_attr, "float -> integer");
		else
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

inline static unsigned max(unsigned a, unsigned b) { return (a > b) ? a : b; }

// min result, ideal result, result is signed, errormessage
std::tuple<llvm::Type*, llvm::Type*, unsigned, const char*> getResType(unsigned left_bitwidth, bool left_is_float, bool left_is_signed, unsigned right_bitwidth, bool right_is_float, bool right_is_signed, const char* Op) {
	/*
	auto left_descr = getBitWidth(left_type);
	unsigned left_bitwidth = left_descr.first;
	bool left_is_float = left_descr.second;
	bool left_is_signed = left_attr & A_signed;
	auto right_descr = getBitWidth(right_type);
	unsigned right_bitwidth = right_descr.first;
	bool right_is_float = right_descr.second;
	bool right_is_signed = right_attr & A_signed;
	*/
	unsigned res_bitwidth_min = max(right_bitwidth, left_bitwidth);
	unsigned res_bitwidth = res_bitwidth_min; // will be refined based on operator
	unsigned res_is_float = left_is_float || right_is_float;
	bool res_is_signed = !res_is_float && (left_is_signed || right_is_signed);
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
	return { getFittingType(res_bitwidth_min, res_is_float), getFittingType(res_bitwidth, res_is_float), res_is_signed, nullptr };
prec_err:
	return { nullptr, nullptr, 0, "illegal usage of %s: RHS would degrade\n" };
}

// compute the conversion functions for binary Operators
BinOpConvSet convBinOp(llvm::Type* left_type, llvm::Type* right_type, unsigned left_attr, unsigned right_attr,
                       const char* Op, SourceLocation Loc)
{
	auto left_descr = getBitWidth(left_type);
	unsigned left_bitwidth = left_descr.first;
	bool left_is_float = left_descr.second;
	bool left_is_signed = left_attr & A_signed;
	auto right_descr = getBitWidth(right_type);
	unsigned right_bitwidth = right_descr.first;
	bool right_is_float = right_descr.second;
	bool right_is_signed = right_attr & A_signed;
	std::tuple<llvm::Type*, llvm::Type*, unsigned, const char*> res_t = getResType(left_bitwidth, left_is_float, left_is_signed, right_bitwidth, right_is_float, right_is_signed, Op);
	if (std::get<3>(res_t))
		return {{ nullptr, nullptr, nullptr, 0, std::get<3>(res_t) }, { nullptr, nullptr, nullptr, 0, std::get<3>(res_t) }};
	unsigned res_bitwidth_min = getBitWidth(std::get<0>(res_t)).first;
	unsigned res_bitwidth = getBitWidth(std::get<1>(res_t)).first;
	auto left_conv = getConv(left_type, std::get<0>(res_t), left_attr, std::get<2>(res_t), Loc, false);
	auto right_conv = getConv(right_type, std::get<0>(res_t), right_attr, std::get<2>(res_t), Loc, false);
	if (res_bitwidth < res_bitwidth_min) // downgrading operation, e.g. >
		return {{ left_conv, right_conv, std::get<0>(res_t), std::get<2>(res_t) }, { left_conv, right_conv, std::get<1>(res_t), std::get<2>(res_t) }};
	else
		return {{ left_conv, right_conv, std::get<0>(res_t), std::get<2>(res_t) },
		        { getConv(left_type, std::get<1>(res_t), left_attr, std::get<2>(res_t), Loc, false),
		          getConv(right_type, std::get<1>(res_t), right_attr, std::get<2>(res_t), Loc, false), std::get<1>(res_t), std::get<2>(res_t) }};
}
