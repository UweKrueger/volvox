/*
 * Copyright © Uwe Krüger 2021, 2022
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"

//===----------------------------------------------------------------------
// type conversion handling
//===----------------------------------------------------------------------

unsigned anon_struct_nr = 0;

volvoxc::FTListElem* anon_types = nullptr;
volvoxc::FTListElem** anon_types_end = &anon_types;

std::nullptr_t AutoErr(SourceLocation Loc, llvm::Type* expr_type, llvm::Type* desired_type,
                              bool expr_is_signed, bool desired_is_signed, const char* reason) {
	errs() << Loc << ": cannot automatically convert "
	       << lex.get_type_name((llvm::Type*)((uintptr_t)expr_type | (expr_is_signed ? A_signed : 0))) << "/"
	       << lex.get_type_name((llvm::Type*)((uintptr_t)desired_type | (desired_is_signed ? A_signed : 0))) << ' ';
	if (reason)
		errs() << reason;
	errs() << "\n";
	return nullptr;
}

static std::nullptr_t ExplicitErr(SourceLocation Loc, llvm::Type* expr_type, llvm::Type* desired_type,
                              bool expr_is_signed, bool desired_is_signed, const char* reason) {
	errs() << Loc << ": cannot convert "
	       << lex.get_type_name((llvm::Type*)((uintptr_t)expr_type | (expr_is_signed ? A_signed : 0))) << "/"
	       << lex.get_type_name((llvm::Type*)((uintptr_t)desired_type | (desired_is_signed ? A_signed : 0))) << ' ';
	if (reason)
		errs() << reason;
	errs() << "\n";
	return nullptr;
}

llvm::Value* NoConversion(llvm::Value* v) { return v; }

// returns { significant_bits, is_float }
std::pair<unsigned, bool> getBitWidth(llvm::Type* type) {
	switch(type->getTypeID()) {
	case llvm::Type::IntegerTyID:
		return { type->getIntegerBitWidth(), false };
	case llvm::Type::DoubleTyID:
		return { 53, true };
	case llvm::Type::FloatTyID:
		return { 24, true };
	case llvm::Type::X86_FP80TyID:
		return { 64, true };
	case llvm::Type::FP128TyID:
		return { 113, true };
	case llvm::Type::BFloatTyID:
		return { 8, true };
	default:
		return { 0, false };
	}
}
			
static llvm::Type* getFittingType(unsigned bitwidth, bool is_float = false) {
	if (bitwidth == 1) // bitwidth = 1 is always bool, i.e. u1
		return llvm::Type::getInt1Ty(Context);
	if (is_float)
		if (bitwidth > 53) // only used for intermediate results during comparisons
			if (support_fp80 && bitwidth <= 64)
				return getX86_FP80Ty(Context);
			else
				return getFP128Ty(Context);
		else
			if (bitwidth > 24)
				return llvm::Type::getDoubleTy(Context);
			else if (bitwidth > 8)
				return llvm::Type::getFloatTy(Context);
			else
				return llvm::Type::getBFloatTy(Context);
	else
		if (bitwidth > 32)
			if (bitwidth > 64)
				return llvm::IntegerType::get(Context, 65); // only for signed/unsigned comparison
			else
				return llvm::Type::getInt64Ty(Context);
		else
			if (bitwidth > 16)
				return llvm::Type::getInt32Ty(Context);
			else if (bitwidth > 8)
				return llvm::Type::getInt16Ty(Context);
			else
				return llvm::Type::getInt8Ty(Context);
		return llvm::IntegerType::get(Context, bitwidth);
}

// is the definition area bigger (not the precision)
// input: type, is_signed
// results: a fits completely (into b), a fits with presision loss
std::pair<bool, bool> analyze_types(std::pair<llvm::Type*, bool> a, std::pair<llvm::Type*, bool> b) {
	auto a_id = a.first->getTypeID();
	auto b_id = b.first->getTypeID();
	auto a_descr = getBitWidth(a.first);
	auto b_descr = getBitWidth(b.first);
	// signed type have a slightly smaller effective bitwidth
	unsigned a_bitwidth = a_descr.first;
	unsigned b_bitwidth = b_descr.first;
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
	llvm::Type* expr_type, llvm::Type* desired_type, SourceLocation Loc, bool expr_is_signed,
	bool desired_is_signed, bool is_explicit, bool is_unknown_type)
{
	const char* reason = "";
	auto desired_descr = getBitWidth(desired_type);
	unsigned desired_bitwidth = desired_descr.first;
	bool float_desired = desired_descr.second;
	auto expr_descr = getBitWidth(expr_type);
	unsigned expr_bitwidth = expr_descr.first;
	bool float_expr = expr_descr.second;
	if (is_explicit) {
		// find type conversion constructor
		if (auto struct_type = llvm::dyn_cast<llvm::StructType>(desired_type)) {
			if (struct_type->hasName()) {
				llvm::SmallString<128> buf = llvm::StringRef("_ZN");
				llvm::raw_svector_ostream mangled(buf);
				mangled << struct_type->getName()  << "C2E";
				auto ft = new_FullType(expr_type, expr_is_signed ? A_signed : 0);
				std::string m_name;
				if (auto expr_struct = llvm::dyn_cast<llvm::StructType>(expr_type)) {
					if (!expr_struct->hasName())
						goto no_explicit_constructor;
					m_name = expr_struct->getName();
					ft->mangled_name = m_name.c_str();
				}
				mangled << ft;
				m_name = buf.c_str();
				auto convFN = getAutoMethod(m_name);
				if (convFN)
					return [=](llvm::Value* v) { return Builder->CreateCall(convFN, { v }); };
			}
		}
	}
no_explicit_constructor:
	if (auto struct_type = llvm::dyn_cast<llvm::StructType>(expr_type)) {
		if (!struct_type->hasName())
			return NoConversion; // TODO: return nullptr - and handle this in caller
		llvm::SmallString<128> buf = llvm::StringRef("_ZN");
		llvm::raw_svector_ostream mangled(buf);
		mangled << struct_type->getName() << "cv";
		auto ft = new_FullType(desired_type, desired_is_signed ? A_signed : 0);
		std::string m_name;
		if (auto desired_struct = llvm::dyn_cast<llvm::StructType>(desired_type)) {
			if (!desired_struct->hasName())
				return NoConversion;
			m_name = desired_struct->getName();
			ft->mangled_name = m_name.c_str();
		}
		mangled << ft;
		m_name = buf.c_str();
		auto convFN = getAutoMethod(m_name);
		if (convFN)
			return [=](llvm::Value* v) { return Builder->CreateCall(convFN, { v }); };
		
	}
	if (float_desired)
		if (float_expr)
			if (desired_bitwidth == expr_bitwidth)
				return NoConversion;
			else if (is_explicit || is_unknown_type || desired_bitwidth >= expr_bitwidth)
				return [=](llvm::Value* v) { return Builder->CreateFPCast(v, desired_type, "convfptmp"); };
			else
				return AutoErr(Loc, expr_type, desired_type, expr_is_signed, desired_is_signed, "float truncation");
		else
			if (is_explicit || is_unknown_type || desired_bitwidth >= expr_bitwidth
			    || desired_bitwidth >= 53) // always allow conversion to f64
				if (expr_is_signed)
					return [=](llvm::Value* v) { return Builder->CreateSIToFP(v, desired_type, "convsfptmp"); };
				else
					return [=](llvm::Value* v) { return Builder->CreateUIToFP(v, desired_type, "convufptmp"); };
			else
				return AutoErr(Loc, expr_type, desired_type, expr_is_signed, desired_is_signed, "int->float would lose precision");
	else
		if (float_expr)
			if (is_explicit)
				if (desired_is_signed)
					return [=](llvm::Value* v) { return Builder->CreateFPToSI(v, desired_type, "convfpstmp"); };
				else
					return [=](llvm::Value* v) { return Builder->CreateFPToUI(v, desired_type, "convfputmp"); };
			else 
				return AutoErr(Loc, expr_type, desired_type, expr_is_signed, desired_is_signed, "float -> integer");
		else
			if (!desired_is_signed)
				if (expr_is_signed)
					// signed -> unsigned
					// if (!is_explicit && !is_unknown_type)
					// 	return AutoErr(Loc, expr_type, desired_type, expr_is_signed, desired_is_signed, "signed->unsigned");
					// else
						if (desired_bitwidth == expr_bitwidth)
							return NoConversion;
						else
							// design decision: make this a signed conversion so that `u64(-1) -> 0xffffffffffffffff`
							return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, true, "convstmp"); };
				else
					// unsigned -> unsigned
					if (desired_bitwidth < expr_bitwidth)
						if (is_explicit || is_unknown_type)
							return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, false, "trunctmp"); };
						else
							return AutoErr(Loc, expr_type, desired_type, expr_is_signed, desired_is_signed, "would truncate upper bits");
					else
						if (desired_bitwidth == expr_bitwidth)
							return NoConversion;
						else
							return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, false, "expandutmp"); };
			else
				if (expr_is_signed)
					// signed -> signed
					if (desired_bitwidth < expr_bitwidth)
						if (is_explicit || is_unknown_type)
							return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, true, "trunctmp"); };
						else
							return AutoErr(Loc, expr_type, desired_type, expr_is_signed, desired_is_signed, "would truncate upper bits");
					else
						return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, true, "expandstmp"); };
				else
					// unsigned -> signed
					if (desired_bitwidth < expr_bitwidth)
						if (is_explicit || is_unknown_type)
							if (desired_bitwidth == expr_bitwidth)
								return NoConversion;
							else
								return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, false, "trunctmp"); };
						else
							return AutoErr(Loc, expr_type, desired_type, expr_is_signed, desired_is_signed, "would truncate/reinterpret upper bits");
					else
						return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, false, "expandstmp"); };
}

std::function<llvm::Value*(llvm::Value*)> getBestPreConv(SourceLocation Loc, llvm::Type* desired_type, llvm::Type* min_type, llvm::Type* ideal_type,
                                                      std::function<llvm::Value*(llvm::Value*)> min_conv,
                                                      std::function<llvm::Value*(llvm::Value*)> ideal_conv, bool is_signed) {
	if (!desired_type || desired_type == min_type)
		return min_conv;
	auto ana_min = analyze_types({min_type, is_signed}, {desired_type, is_signed});
	auto ana_ideal = analyze_types({ideal_type, is_signed}, {desired_type, is_signed});
	if (ana_ideal.first)
		return ideal_conv;
	if (!ana_min.first) {
		if (ana_ideal.second)
			return ideal_conv;
		else
			return min_conv;
	}
	errs() << Loc << " cannot convert " << *min_type << " to " << *desired_type << '\n';
	return nullptr;
}

inline static unsigned Max(unsigned a, unsigned b) { return (a > b) ? a : b; }
inline static unsigned Min(unsigned a, unsigned b) { return (a < b) ? a : b; }

// classificatio of binary operators with result type calculation in mind
enum OpClass {
	OpNormal,
	OpAssign,
	OpComparison,
	OpShift,
	OpLogical,
	OpBitwise,
	OpExponentiation
};

static inline OpClass getOpClass(const char* Op) {
	switch (Op[1]) {
	case '<':
		if (Op[0] == '>')
			return OpBitwise;
	case '>':
		if (Op[1] == Op[0]) {
			if (!Op[2])
				return OpShift;
			else
				return OpAssign; // <<=, >>=
		}
	case '=':
		switch (Op[0]) {
		case '=':
		case '!':
		case '>':
		case '<':
			return OpComparison;
		default:
			return OpAssign; // +=, -=, ...
		}
	case '&':
	case '|':
		if (!Op[2])
			return OpLogical;
		else
			return OpAssign; // &&=, ||=
	case '\0':
		switch (Op[0]) {
		case '>':
		case '<':
			return OpComparison;
		case '=':
			return OpAssign;
		case '&':
		case '|':
			return OBitwise;
		case '=':
			return OpAssign;
		case '^':
			return OpExponentiation;
		default:
			OpNormal;
		}
	default:
		abort();
}

// desired_left_type, desired_right_type, left_signed, right_signed, errormessage
std::tuple<llvm::Type*, llvm::Type*, bool, bool, const char*> getDesiredTypes(llvm::Type* desired_res,
	        llvm::Type* left_type, llvm::Type* right_type, const char* Op, bool desired_signed,
	        bool left_signed, bool right_signed, bool left_is_unknown_type, bool right_is_unknown_type)
{
	auto [left_bitwidth, left_is_float] = getBitWidth(left_type);
	auto [right_bitwidth, right_is_float] = getBitWidth(right_type);
	auto [desired_res_bitwidth, desired_res_is_float] = desired_res ? getBitWidth(desired_res) : { 0U, false };
	unsigned res_bitwidth_min = right_is_unknown_type ?
		(left_is_unknown_type ? (desired_res_bitwidth ? desired_res_bitwidth : Max(right_bitwidth, left_bitwidth))
		 : left_bitwidth) :
		left_is_unknown_type ? right_bitwidth : Max(right_bitwidth, left_bitwidth);
	unsigned res_bitwidth = res_bitwidth_min; // will be refined based on operator
	bool res_is_float = left_is_float || right_is_float;
	bool res_ideal_is_float = res_is_float;
	bool res_is_signed = left_is_signed && !left_is_unknown_type || right_is_signed && !right_is_unknown_type || left_is_signed && right_is_signed; 
	bool is_shift = false;
	bool is_logical = false;
	// in simple cases one operand is converted to the type of the other
	// here we calculate the ideal result bitwidth to prevent data loss due to overflow
	unsigned opsz = strlen(Op);
	if (Op[opsz-1] == '=') {
		if (opsz == 2 && Op[0] == '>' || Op[0] == '<' || Op[0] == '!' || Op[0] == '=') {
			// comparison
				if (left_is_signed != right_is_signed && !left_is_unknown_type && !right_is_unknown_type) {
					res_is_signed = true;
					res_bitwidth_min++;
				}
				res_bitwidth = 1;
			goto comparison;
		}
		res_bitwidth_min = res_bitwidth = left_bitwidth;
		if (right_bitwidth > left_bitwidth && !right_is_unknown_type || !left_is_float && right_is_float)
			return { nullptr, nullptr, false, false, "illegal usage of %s: RHS would degrade\n" };
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
		res_ideal_is_float = true;
		res_bitwidth = 53; // double
	case '%':
		res_bitwidth = left_bitwidth;
		break;
	case '^':
		if (!left_is_float && !right_is_float) {
			res_bitwidth = left_bitwidth;
			res_bitwidth_min = left_bitwidth;
		} else {
			res_bitwidth = 53;
			res_bitwidth_min = left_bitwidth;
			res_ideal_is_float = true;
			res_is_float = true;
		}
		break;
	case '>':
		if (Op[1] == '<')
			goto op_xor;
	case '<':
	case '=':
	comparison:
		if (Op[1] == Op[0] && Op[0] != '=') {
			is_shift = true; // to allow signed / unsigend mismatch
			res_bitwidth_min = left_bitwidth;
			res_bitwidth = Op[0] == '<' ? 64 : res_bitwidth_min;
			res_is_signed = left_is_signed;
		} else {
			if (Op[0] == '=' && !Op[1]) {
				// this is an assignment by default, i.e. if no bool result is expected
				res_bitwidth = res_bitwidth_min = left_bitwidth;
				res_is_float = left_is_float;
				res_is_signed = left_is_signed;
			} else {
				if (left_is_signed != right_is_signed && !left_is_unknown_type && !right_is_unknown_type) {
					res_is_signed = true;
					res_bitwidth_min++;
				}
				res_bitwidth = 1;
			}
		}
		break;
	case '&':
	case '|':
	op_xor:
		res_is_signed = false; // default to unsigned for (possibly bitwise) &, |, ><
		break;
	default:
		;
	}
calc_types:
	res_ideal_is_float = res_ideal_is_float || res_is_float;
	bool left_is_promoted = (res_bitwidth_min > left_bitwidth || res_is_float && !left_is_float) && !left_is_unknown_type;
	bool right_is_promoted = (res_bitwidth_min > right_bitwidth || res_is_float && !right_is_float) && !is_shift && !left_is_unknown_type;
	llvm::Type* def_type = (left_is_promoted && right_is_promoted && res_bitwidth != 1) ?
		nullptr : // forbid both-side promotion as default
		getFittingType(res_bitwidth_min, res_is_float);
	return { def_type, getFittingType(res_bitwidth, res_ideal_is_float), res_is_signed,
		left_is_unknown_type && (right_is_unknown_type || is_shift),
		def_type ? nullptr : "would require promotions on both sides" };
}

// get "natural" result type for binary operators, i.e if desired type is not known (yet)
// this is usually the "biggest" operand type - an error is returned if converting the
// smaller one would mean precision loss. This error is not printed because once the
// desire type is known the operation might still turn out to be valid
//
// result_type, result is signed, result is unknown type, errormessage
std::tuple<llvm::Type*, bool, bool, const char*> getResType(
	llvm::Type* left_type, llvm::Type* right_type, const char* Op,
	bool left_signed, bool right_signed, bool left_is_unknown_type, bool right_is_unknown_type)
{
	auto [left_bitwidth, left_is_float] = getBitWidth(left_type);
	auto [right_bitwidth, right_is_float] = getBitWidth(right_type);
	unsigned res_bitwidth;
	bool res_is_unknown_type = left_is_unknown_type & right_is_unknown_type;
	bool res_is_float = left_is_float || right_is_float;
	bool res_is_signed = left_is_signed && !left_is_unknown_type || right_is_signed && !right_is_unknown_type || left_is_signed && right_is_signed;
	auto opclass = getOpClass(Op);
	switch (opclass) {
	case OpComparison:
		return { llvm::Type::getInt1Ty(Context), false, false, nullprt };
	case OpAssign:
		res_bitwidth = left_bitwidth;
		if (right_bitwidth > left_bitwidth && !right_is_unknown_type || !left_is_float && right_is_float)
			return { nullptr, false, false, "illegal usage of %s: RHS would degrade\n" };
		break;
	case OpShift:
		if (res_is_float)
			return { nullptr, false, false, "shift operator %s can only be used with integer operands\n" };
	case OpExponentiation:
		res_bitwidth = left_bitwidth;
		res_is_signed = left_is_signed;
		break;
	case OpLogical:
		if (left_bitwidth != 1 || right_bitwidth != 1)
			return { nullptr, false, false, "logical operator %s can only be used with bool operands\n" };
		res_bitwidth = 1;
		res_is_signed = false;
		break;
	case OpBitwise:
		res_is_signed = false; // default to unsigned for (possibly bitwise) &, |, ><
		// fallthrough
	default:
		res_bitwidth = right_is_unknown_type ?
		(left_is_unknown_type ? Max(right_bitwidth, left_bitwidth)
		 : left_bitwidth) :
		left_is_unknown_type ? right_bitwidth : Max(right_bitwidth, left_bitwidth);;
	}
	return { getFittingType(res_bitwidth, res_is_float), res_is_signed, res_is_unknown_type, nullptr };
}

// compute the conversion functions for binary Operators
BinOpConvSet convBinOp(llvm::Type* left_type, llvm::Type* right_type, bool left_is_signed, bool right_is_signed,
                       bool left_is_unknown_type, bool right_is_unknown_type,
                       const char* Op, SourceLocation Loc)
{
	if (!strcmp(Op, ":=") || !left_type || Op[0] == ',') // variable declaration, i.e. := operator
		return {{ nullptr, nullptr, nullptr, nullptr, false, false }, { nullptr, nullptr, nullptr, nullptr, false, false }};
	if (!left_type->isSingleValueType() || !right_type || !right_type->isSingleValueType())
		return {{ nullptr, nullptr, left_type, nullptr, left_is_signed, false }, { nullptr, nullptr, left_type, nullptr, left_is_signed, false }};
	auto left_descr = getBitWidth(left_type);
	unsigned left_bitwidth = left_descr.first;
	bool left_is_float = left_descr.second;
	auto right_descr = getBitWidth(right_type);
	unsigned right_bitwidth = right_descr.first;
	bool right_is_float = right_descr.second;

	auto [res_type_min, res_type, res_is_signed, res_is_unknown_type, err_msg] = getResType(
		left_bitwidth, left_is_float, left_is_signed, left_is_unknown_type,
		right_bitwidth, right_is_float, right_is_signed, right_is_unknown_type, Op);

	unsigned res_bitwidth_min = res_type_min ? getBitWidth(res_type_min).first : 0;
	unsigned res_bitwidth = getBitWidth(res_type).first;
	std::function<llvm::Value*(llvm::Value*)> right_conv_min;
	std::function<llvm::Value*(llvm::Value*)> right_conv;
	if (Op[0] == '^' && !right_is_float) { // special treatment for a^b whene b is int
		auto desired_exp_type = (right_bitwidth <= 16) ? llvm::Type::getInt16Ty(Context) : llvm::Type::getInt32Ty(Context);
		right_conv_min = right_conv = [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_exp_type, true, "convpowexp"); };
	} else {
		right_conv_min = res_type_min ? getConv(right_type, res_type_min, Loc, right_is_signed, res_is_signed, false, right_is_unknown_type) : nullptr;
		right_conv = res_bitwidth_min <= res_bitwidth ? getConv(right_type, res_type, Loc, right_is_signed, res_is_signed, false, right_is_unknown_type) : nullptr;
	}
	auto left_conv_min = res_type_min ? getConv(left_type, res_type_min, Loc, left_is_signed, res_is_signed, false, left_is_unknown_type) : nullptr;
	auto left_conv = res_bitwidth_min <= res_bitwidth ? getConv(left_type, res_type, Loc, left_is_signed, res_is_signed, false, left_is_unknown_type) : nullptr;
	return {{ left_conv_min, right_conv_min, res_type_min, err_msg, res_is_signed, res_is_unknown_type },
	        { left_conv, right_conv, res_type, nullptr, res_is_signed && res_bitwidth > 1, res_is_unknown_type }};
}

std::tuple<llvm::Type*, std::function<llvm::Value*(llvm::Value*)>, bool> MakeType(llvm::Type* type, bool is_signed, bool is_unknown_type) {
	if(!is_unknown_type)
		return { type, NoConversion, is_signed };
	if (type->isIntegerTy())
		return { llvm::Type::getInt32Ty(Context), [=](llvm::Value* v) { return Builder->CreateSExtOrTrunc(v, llvm::Type::getInt32Ty(Context), "convintinit"); } , true };
	else
		return { type, NoConversion, false };
}

volvoxc::FullType* MakeType(volvoxc::FullType* base, bool is_unknown_type) {
	if (is_unknown_type && base->type->isIntegerTy()) {
		volvoxc::FullType* new_type = lex.get_full_type("i32");
		if (!new_type) {
			errs() <<"Fatal: Could not find i32 type!\n";
			return nullptr;
		}
		return new_type;
	} else {
		return base;
	}
}

// get element type of an array
std::pair<volvoxc::FullType*,std::vector<std::function<llvm::Value*(llvm::Value*)>>> AggregateExprAST::getArrayConv(
	ListExprAST* List, llvm::Type* elem_type, unsigned elem_attr) {
	std::vector<std::function<llvm::Value*(llvm::Value*)>> conv;
	bool is_signed = elem_attr & A_signed;
	bool is_float = false;
	unsigned bitwidth = 0;
	if (elem_type && elem_type->isSingleValueType()) {
		auto bw = getBitWidth(elem_type);
		bitwidth = bw.first;
		is_float = bw.second;
	}
	SourceLocation MaxBWLoc;
	volvoxc::FullType* res_ft = nullptr;
	conv.reserve(valid_exprs.size());
	if (!bitwidth) {
		for (auto& elem: valid_exprs) {
			if (elem) {
				if (res_ft) {
					if (elem->ft->type != res_ft->type) { // TODO: implement FullType comparison
						errs() << elem->Loc << ": array element types do not match\n";
						return { nullptr, conv };
					}
				} else {
					if (elem->ft->type->isSingleValueType()) {
						auto bw = getBitWidth(elem->ft->type);
						if (bw.first > bitwidth) {
							bitwidth = bw.first;
							MaxBWLoc = elem->Loc;
						}
						is_float = is_float || bw.second;
						is_signed = is_signed || (elem->ft->type_attr & A_signed);
					} else {
						res_ft = elem->ft;
					}
				}
			}
		}
	}
	if (res_ft) {
		return { res_ft, conv };
	} else {
		if (!bitwidth) {
			errs() << "no valid element in array initialization\n";
			return { nullptr, conv };
		} else {
			if (is_float && bitwidth > 53) {
				errs() << MaxBWLoc << ": 64 bit integer in array initialization not compatible with float type(s)\n";
				return { nullptr, conv };
			}
			llvm::Type* res_type = getFittingType(bitwidth, is_float);
			auto type_name = lex.get_type_name(res_type, is_signed && !is_float);
			// TODO: implement full type lookup that doesn't need getting name string
			res_ft = lex.get_full_type(type_name);
			for (auto& elem: valid_exprs)
				conv.push_back(getConv(elem->ft->type, res_type, elem->Loc, elem->ft->type_attr & A_signed, is_signed && !is_float));
			return { res_ft, conv };
		}
	}
}

llvm::Constant* getRtType(volvoxc::FullType* ft) {
	union {
		VOLVOX_gen_val_type_t llvmtype;
		unsigned key;
	};
	unsigned subclassdata = 0; // bitwidth for int types, order for arrays, number of elements for structs
	unsigned additional_attribs = 0; // e.g. A_packed for structs
	llvm::Type* elem_type = ft->type;
	llvm::SmallVector<size_t, 16> Dims; // for multi dimensional arrays
	while (auto array_type = llvm::dyn_cast<llvm::ArrayType>(elem_type)) {
		subclassdata++;
		Dims.push_back(array_type->getNumElements());
		elem_type = array_type->getElementType();
	}
	if (!subclassdata) {
		if (auto struct_type = llvm::dyn_cast<llvm::StructType>(ft->type)) {
			subclassdata = struct_type->getNumElements();
			if (struct_type->isPacked())
				additional_attribs |= A_packed;
		} else {
			// if it's no array nor a struct use LLVM's subclassdata (e.g. integer bitwidth)
			subclassdata = ((genType*)ft->type)->SubClassData();
		}
	}
	llvmtype = VOLVOX_gen_val_type_t{ .ID = (VOLVOX_TypeID)ft->type->getTypeID(), .SubclassData = subclassdata };
	llvm::SmallVector<llvm::Constant*, 16> fields;
	fields.push_back(llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), (uint64_t)key));
	fields.push_back(llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), (uint64_t)(ft->type_attr | additional_attribs)));
	if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(ft->type)) {
		auto dim_array = llvm::ConstantDataArray::get(Context, Dims);
		auto GV = new llvm::GlobalVariable(*TheModule, dim_array->getType(), true, llvm::GlobalValue::PrivateLinkage,
		                                        dim_array, "", nullptr, llvm::GlobalVariable::NotThreadLocal, 0);
		GV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
		GV->setAlignment(llvm::Align(sizeof(void*)));
		llvm::Constant *Zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 0);
		llvm::Constant *Indices[] = {Zero, Zero};
		fields.push_back(llvm::ConstantExpr::getInBoundsGetElementPtr(GV->getValueType(), GV, Indices));
	} else {
		fields.push_back(llvm::ConstantInt::get(llvm::Type::getInt64Ty(Context), (uint64_t)(
			                                        ft->type->isFunctionTy() ? sizeof(char*) : TheModule->getDataLayout().getTypeAllocSize(ft->type))));
	}
	llvm::Constant* TypeName;
	if (auto struct_type = llvm::dyn_cast<llvm::StructType>(ft->type)) {
		if (struct_type->hasName()) {
			TypeName = Builder->CreateGlobalStringPtr(struct_type->getName(), "", 0, TheModule.get());
		} else {
			TypeName = llvm::ConstantPointerNull::get(llvm::Type::getInt8PtrTy(Context));
		}
		fields.push_back(TypeName);
		auto num_fields = struct_type->getNumElements();
		auto idx_offs = fields.size();
		for (unsigned k=0; k<num_fields; k++)
			fields.push_back(nullptr);
		for (auto struct_field = ft->first(); struct_field; ++struct_field) {
			unsigned index = struct_field.getIndex();
			volvoxc::FullType* field_ft = struct_field.getFt();
			const char* field_name = struct_field.getKey();
			llvm::SmallVector<llvm::Constant*, 2> fld_descr;
			fld_descr.push_back(Builder->CreateGlobalStringPtr(field_name, "", 0, TheModule.get()));
			fld_descr.push_back(getRtType(field_ft));
			llvm::Constant* field = llvm::ConstantStruct::getAnon(Context, fld_descr, true);
			fields[idx_offs+index] = field;
		}
	} else {
		TypeName = llvm::ConstantPointerNull::get(llvm::Type::getInt8PtrTy(Context));
		fields.push_back(TypeName);
		if (llvmtype.ID == VOLVOX_ArrayTyID) {
			fields.push_back(getRtType(ft->elem_type));
		} else {
			fields.push_back(llvm::ConstantPointerNull::get(llvm::Type::getInt8PtrTy(Context)));
		}
	}
	llvm::Constant* rt_const = llvm::ConstantStruct::getAnon(Context, fields, true);
	auto *GV = new llvm::GlobalVariable(*TheModule, rt_const->getType(), true, llvm::GlobalValue::PrivateLinkage, rt_const);
	llvm::Constant *Zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 0);
	llvm::Constant *Indices[] = {Zero, Zero};
	return llvm::ConstantExpr::getInBoundsGetElementPtr(GV->getValueType(), GV,
	                                                    Indices);
}

void volvoxc::FullType::dump(int fd) {
	llvm::raw_fd_ostream eout(fd, false, true, llvm::raw_ostream::OStreamKind::OK_FDStream);
	llvm::StringRef TypeName;
	if (type) {
		if (auto struct_type = llvm::dyn_cast<llvm::StructType>(type)) {
			if (struct_type->hasName()) {
				TypeName = struct_type->getName();
				goto type_name_set;
			}
		}
	}
	TypeName = "<none>";
type_name_set:
	eout << "FullName: " << TypeName << '\n';
	eout << "MangledName: " << (mangled_name ? mangled_name : "<none>") << '\n';
	eout << "LLVMType: ";
	if (type)
		type->print(eout);
	else
		eout << "<nil>";
	eout << "\nAttr: " << type_attr << "\n";
	if (type && type->isArrayTy()) {
		eout << "Elements:\n";
		elem_type->dump();
	}
}

llvm::ArrayType* MakeInterfaceArrayType(llvm::ArrayType* array_type) {
	llvm::Type* elem_type;
	unsigned depth = 0;
	do {
		depth++;
		elem_type = array_type->getElementType();
		array_type = llvm::dyn_cast<llvm::ArrayType>(elem_type);
	} while (array_type);
	llvm::ArrayType* res_type;
	for (unsigned j = 0;;) {
		res_type = llvm::ArrayType::get(elem_type, 0);
		if (++j >= depth)
			break;
		elem_type = res_type;
	}
	return res_type;
}

PrototypeAST::PrototypeAST(SourceLocation Loc, const std::string &Name,
                           std::vector<std::string> Args, unsigned visibility, SourceLocation retLoc,
                           bool IsOperator, volvoxc::FullType* RetType_,
                           std::vector<volvoxc::FullType*> _ArgTypes,
                           std::vector<SourceLocation> _ArgPos, bool IsVarArgs)
		: Name(Name), Args(Args), IsOperator(IsOperator), retLoc(retLoc),
		  Line(Loc.Line), RetType(RetType_ ? RetType_ : void_type), ArgTypes(std::move(_ArgTypes)),
		  ArgPos(std::move(_ArgPos)), IsVarArgs(IsVarArgs), visibility(visibility)
{
	size_t ret_size = RetType->type->isSized() ?
		TheModule->getDataLayout().getTypeAllocSize(RetType->type) :
		0;
	llvm::Type* llvm_ret_type;
	for (auto& argtype: ArgTypes) {
		llvm::Type* fn_arg_type = argtype->type;
		size_t argsize = argtype->type->isSized() ?
			TheModule->getDataLayout().getTypeAllocSize(fn_arg_type) : 0;
		if (argtype->type_attr & A_ref) {
			ArgAttrs.push_back(llvm::AttributeSet::get(Context, llvm::ArrayRef<llvm::Attribute>{
						llvm::Attribute::getWithByRefType(Context, argtype->type),
						llvm::Attribute::getWithDereferenceableBytes(Context, argsize) }));
			fn_arg_type = fn_arg_type->getPointerTo();
		} else {
			if (argsize > 16) { // Arguments > 16 bytes are always passed as pointer using copy-on-write
				ArgAttrs.push_back(llvm::AttributeSet::get(Context, llvm::ArrayRef<llvm::Attribute>{
							llvm::Attribute::getWithByValType(Context, argtype->type) }));
				fn_arg_type = fn_arg_type->getPointerTo();
			} else {
				ArgAttrs.push_back(llvm::AttributeSet());
			}
		}
		LLVMArgTypes.push_back(fn_arg_type);
	}
	if (ret_size <= 16) {
		llvm_ret_type = RetType->type;
	} else {
		IsStructRet = true;
		llvm::Type* struct_ret_type = RetType->type->getPointerTo();
		LLVMArgTypes.insert(LLVMArgTypes.begin(), struct_ret_type);
		ArgAttrs.insert(ArgAttrs.begin(), llvm::AttributeSet::get(Context, llvm::ArrayRef<llvm::Attribute>{
					llvm::Attribute::getWithStructRetType(Context, RetType->type) }));
		llvm_ret_type = llvm::Type::getVoidTy(Context);
	}
	FT = llvm::FunctionType::get(llvm_ret_type, LLVMArgTypes, IsVarArgs);
}

void destroy_FV(MapValue* mapval) {
	auto var = (FullVar*)((char*)mapval + mapval->offset);
	var->destroy();
}

llvm::Function* getDestructor(volvoxc::FullType* ft, bool is_created, bool is_constructor) {
	if (!ft->mangled_name || !(ft->type_attr & A_destructor) && !is_created)
		return nullptr;
	std::string fn_name = "_Z";
	if (ft->mangled_name[0] == 'N') {
		fn_name += ft->mangled_name;
		unsigned sz = fn_name.size() - 1;
		if (fn_name[sz] == 'E')
			fn_name.resize(sz);
		else {
			errs() << "inconsistent mangled type name '" << ft->mangled_name << "'\n";
			return nullptr;
		}
	} else {
		fn_name += 'N';
		fn_name += ft->mangled_name;
	}
	fn_name += (is_constructor ? "C2Ev" : "D2Ev");
	if (!is_created)
		if (auto F = TheModule->getFunction(fn_name))
			return F;
	auto FT = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), { ft->type->getPointerTo() }, false);
	auto F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage, fn_name, TheModule.get());
	auto thisarg = F->getArg(0);
	auto argsize = ft->type->isSized() ?
		TheModule->getDataLayout().getTypeAllocSize(ft->type) : 0;
	auto attr_set = llvm::AttributeSet::get(Context, llvm::ArrayRef<llvm::Attribute>{
			llvm::Attribute::getWithByRefType(Context, ft->type),
			llvm::Attribute::getWithDereferenceableBytes(Context, argsize)
		});
#if LLVM_VERSION_MAJOR >= 14
	llvm::AttrBuilder attr_builder(Context, attr_set);
	thisarg->addAttrs(attr_builder);
#else
	for (auto attr: attr_set)
		thisarg->addAttr(attr);
#endif
	thisarg->setName("this");
	return F;
}
