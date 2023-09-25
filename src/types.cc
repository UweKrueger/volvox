/*
 * Copyright © Uwe Krüger 2021, 2022, 2023
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

void ConversionErr(SourceLocation Loc, llvm::Type* expr_type, llvm::Type* desired_type,
                   bool expr_is_signed, bool desired_is_signed, const char* reason, bool is_explicit) {
	errs() << Loc << "cannot " << (is_explicit ? "" : "automatically ") << "convert "
	       << lex.get_type_name((llvm::Type*)((uintptr_t)expr_type | (expr_is_signed ? A_signed : 0))) << "/"
	       << lex.get_type_name((llvm::Type*)((uintptr_t)desired_type | (desired_is_signed ? A_signed : 0))) << ' ';
	if (reason)
		errs() << reason;
	errs() << "\n";
}

llvm::Value* NoConversion(llvm::Value* v) { return v; }

// returns { significant_bits, is_float }
std::pair<unsigned, bool> getBitWidth(llvm::Type* type) {
	if (!type)
		return { 0, false };
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
			
static llvm::Type* getFittingType(unsigned bitwidth, bool is_float = false, bool maybe_f80 = false) {
	if (bitwidth == 1) // bitwidth = 1 is always bool, i.e. u1
		return llvm::Type::getInt1Ty(Context);
	if (is_float)
		if (bitwidth > 53 && maybe_f80) // only used for intermediate results during comparisons
			if (support_fp80 && bitwidth <= 64)
				return llvm::Type::getX86_FP80Ty(Context);
			else
				return llvm::Type::getFP128Ty(Context);
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

// Try to convert 'expr_type' to 'desired_type'
// return an error if not possible or no explicit conversion
// is requested but precision would be lost
std::function<llvm::Value*(llvm::Value*)> getConv(
	llvm::Type* expr_type, llvm::Type* desired_type, SourceLocation Loc, bool expr_is_signed,
	bool desired_is_signed, bool is_explicit, bool is_unknown_type, bool* exact_match)
{
	if (!expr_type)
		return nullptr;
	if (expr_type == desired_type && (expr_is_signed == desired_is_signed || !expr_type->isIntegerTy())) {
		if (exact_match)
			*exact_match = true;
		return NoConversion;
	}
	if (exact_match)
		*exact_match = false;
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
					m_name = expr_struct->getName().str();
					ft->mangled_name = m_name.c_str();
				}
				mangled << ft;
				m_name = buf.c_str();
				auto convFN = getConversion(m_name);
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
			m_name = desired_struct->getName().str();
			ft->mangled_name = m_name.c_str();
		}
		mangled << ft;
		m_name = buf.c_str();
		auto convFN = getConversion(m_name);
		if (convFN)
			return [=](llvm::Value* v) { return Builder->CreateCall(convFN, { v }); };
		else
			return nullptr;
	}
	if (auto expr_array = llvm::dyn_cast<llvm::ArrayType>(expr_type)) {
		auto desired_array = llvm::dyn_cast<llvm::ArrayType>(desired_type);
		llvm::Type* expr_elem;
		llvm::Type* desired_elem;
		do {
			if (!desired_array)
				return nullptr;
			auto n_elem_expr = expr_array->getNumElements();
			auto n_elem_desired = desired_array->getNumElements();
			// if both dimensions in this level are known at compile time they must match
			if (n_elem_expr && n_elem_desired && n_elem_expr != n_elem_desired)
				return nullptr;
			expr_elem = expr_array->getElementType();
			desired_elem = desired_array->getElementType();
			expr_array = llvm::dyn_cast<llvm::ArrayType>(expr_elem);
			desired_array = llvm::dyn_cast<llvm::ArrayType>(desired_elem);
		} while (expr_array);
		if (expr_elem == desired_elem && (expr_is_signed == desired_is_signed || !expr_elem->isIntegerTy())) {
			if (exact_match)
				*exact_match = true;
			return NoConversion;
		} else {
			return nullptr;
		}
	}
	if (desired_bitwidth == 1) {
		if (expr_bitwidth == 1)
			return NoConversion;
		if (!is_explicit) {
			if (float_expr)
				return AutoErr(Loc, expr_type, desired_type, expr_is_signed, desired_is_signed, "float -> bool");
			if (expr_is_signed)
				return AutoErr(Loc, expr_type, desired_type, expr_is_signed, desired_is_signed, "signed -> bool");
		}
		if (float_expr)
			return [=](llvm::Value* v) { return Builder->CreateFCmpONE(v, llvm::Constant::getNullValue(v->getType()), "convfltbool"); };
		else
			return [=](llvm::Value* v) { return Builder->CreateIsNotNull(v, "convintbool"); };
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
					if (!is_explicit && !is_unknown_type)
						return AutoErr(Loc, expr_type, desired_type, expr_is_signed, desired_is_signed, "signed->unsigned");
					else
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

inline static unsigned Max(unsigned a, unsigned b) { return (a > b) ? a : b; }
inline static unsigned Min(unsigned a, unsigned b) { return (a < b) ? a : b; }

// classification of binary operator with result type calculation in mind
OpClass getOpClass(const char* Op) {
	switch (Op[1]) {
	case '<':
		if (Op[0] == '>') {
			if (!Op[2])
				return OpBitwise;
			else
				return OpModAssign; // ><=
		}
	case '>':
		if (Op[1] == Op[0]) {
			if (!Op[2])
				return OpShift;
			else
				return OpModAssign; // <<=, >>=
		}
	case '=':
		switch (Op[0]) {
		case '=':
		case '!':
		case '>':
		case '<':
			return OpComparison;
		case ':':
			return OpDeclAssign;
		default:
			return OpModAssign; // +=, -=, ...
		}
	case ':':
		return OpDeclAssign;
	case '&':
	case '|':
		if (!Op[2])
			return OpLogical;
		else
			return OpModAssign; // &&=, ||=
	case '.':
		return OpRange; // ..
	case '\0':
		switch (Op[0]) {
		case '>':
		case '<':
			return OpComparison;
		case '=':
			return OpAssign;
		case '&':
		case '|':
			return OpBitwise;
		case '^':
			return OpExponentiation;
		case ':':
			return OpColon;
		case ',':
			return OpComma;
		default:
			return OpNormal; // +, -, *, /, %
		}
	case 'f':
		return OpNormal; // "if", (also "while")
	default:
		return OpInvalid;
	}
}

// desired_left_type, desired_right_type, errormessage
std::tuple<llvm::Type*, llvm::Type*, const char*> getDesiredTypes(llvm::Type* res_type, llvm::Type* desired_res,
	        llvm::Type* left_type, llvm::Type* right_type, OpClass opclass, bool res_min_is_signed,
	        bool left_is_signed, bool right_is_signed, bool left_is_unknown_type, bool right_is_unknown_type)
{
	auto [left_bitwidth, left_is_float] = getBitWidth(left_type);
	if (!left_bitwidth)
		return { nullptr, nullptr, nullptr };
	auto [right_bitwidth, right_is_float] = getBitWidth(right_type);
	if (!right_bitwidth)
		return { nullptr, nullptr, nullptr };
	auto [desired_res_bitwidth, desired_res_is_float] = desired_res ? getBitWidth(desired_res)
		: std::pair<unsigned,bool>{ 0, false };
	unsigned desired_bitwidth;
	if (desired_res_bitwidth && (desired_res_bitwidth != 1 &&
	                             (left_is_unknown_type || left_bitwidth < desired_res_bitwidth || desired_res_is_float && !left_is_float) &&
	                             (right_is_unknown_type || right_bitwidth < desired_res_bitwidth || desired_res_is_float && !right_is_float)
	                             || opclass == OpLogical))
		desired_bitwidth = desired_res_bitwidth;
	else if (left_is_unknown_type)
		if (right_is_unknown_type)
			desired_bitwidth = Max(right_bitwidth, left_bitwidth);
		else
			desired_bitwidth = right_bitwidth;
	else if (right_is_unknown_type)
		desired_bitwidth = left_bitwidth;
	else
		desired_bitwidth = Max(right_bitwidth, left_bitwidth);
	bool res_is_float = desired_res_is_float || left_is_float || right_is_float;
	llvm::Type* desired_left_type = nullptr;
	llvm::Type* desired_right_type = nullptr;
	switch (opclass) {
	case OpAssign:
	case OpModAssign:
		desired_left_type = nullptr;
		desired_right_type = left_type;
		goto normal_return;
		return { nullptr, desired_right_type != right_type ? desired_right_type : nullptr, nullptr };
	case OpDeclAssign:
		return { nullptr, nullptr, nullptr };
	case OpComparison:
		if (left_type == right_type && left_is_signed == right_is_signed) {
			return { left_type, right_type, nullptr };
		} else {
			auto [left_bitwidth, left_is_float] = getBitWidth(left_type);
			auto [right_bitwidth, right_is_float] = getBitWidth(right_type);
			// desired_bitwidth = Max(left_bitwidth, right_bitwidth);
			bool desire_float = left_is_float || right_is_float;
			if (!desire_float) {
				if (left_is_signed && !left_is_unknown_type || right_is_signed && !right_is_unknown_type) {
					if (!left_is_signed && left_bitwidth >= right_bitwidth
					    || !right_is_signed && right_bitwidth >= left_bitwidth)
						desired_bitwidth++;
					desired_left_type = desired_right_type = getFittingType(desired_bitwidth, desire_float, true);
					goto normal_return;
				}
			}
		}
		break;
	case OpShift:
	case OpExponentiation:
		if (desired_res_bitwidth) {
			if (res_is_float) {
				desired_left_type = getFittingType(desired_res_bitwidth, true);
				if (right_is_float)
					desired_right_type = desired_left_type;
				else
					desired_right_type = nullptr; // float^int no pre-conversion on RHS necessary
			} else {
				desired_left_type = desired_right_type = getFittingType(desired_bitwidth, false);
			}
		} else {
			if (res_is_float) {
				desired_left_type = getFittingType(left_bitwidth, true);
				if (right_is_float)
					desired_right_type = desired_left_type;
				else
					// float^(int/int) is detected in codegen and desired_right_type will be set to float
					desired_right_type = nullptr; // float^int no pre-conversions necessary
			} else
				if (opclass == OpShift)
					desired_right_type = left_type;
				else
					desired_left_type = desired_right_type = getFittingType(desired_bitwidth >= 32 ? desired_bitwidth : 32, false);
		}
		goto normal_return;
	case OpLogical:
		desired_right_type = desired_left_type = llvm::Type::getInt1Ty(Context);
		goto normal_return;
	default:
		;
	}
	if (desired_bitwidth)
		desired_left_type = desired_right_type = getFittingType(desired_bitwidth, res_is_float);
normal_return:
	return { desired_left_type, desired_right_type, nullptr };
}

// result_type, result attributes, result is unknown type, Operator Class, errormessage
static std::tuple<llvm::Type*, unsigned, bool, OpClass, const char*> getStringRes(
	llvm::Type* left_type, llvm::Type* right_type, 
	const char* Op, unsigned left_attr, unsigned right_attr)
{
	auto opclass = getOpClass(Op);
	const char* err_msg = nullptr;
	if (!strcmp(Op, "+") || !strcmp(Op, "=")) {
		if (left_attr & right_attr & A_string)
			return { llvm::Type::getInt8PtrTy(Context), A_string, false, opclass, nullptr };
		if (!(left_attr & A_string))
			err_msg = "LHS of operator '%s' is no string (but RHS is)\n";
		else
			err_msg = "RHS of operator '%s' is no string (but LHS is)\n";
	} else if (!strcmp(Op, "*")) {
		if (left_type->isIntegerTy() || right_type->isIntegerTy())
			return { llvm::Type::getInt8PtrTy(Context), A_string, false, opclass, nullptr };
		if (left_attr & A_string)
			err_msg = "RHS of operator '%s' must be an integer as LHS is a string\n";
		else
			err_msg = "LHS of operator '%s' must be an integer as RHS is a string\n";
	} else if (opclass == OpColon || opclass == OpDeclAssign || opclass == OpComma)
		return { nullptr, 0, false, opclass, nullptr };
	else if (opclass == OpComparison)
		return { llvm::Type::getInt1Ty(Context), 0, false, opclass, nullptr };
	return { nullptr, 0, false, opclass, err_msg };
}

// get "natural" result type for binary operators, i.e if desired type is not known (yet)
// this is usually the "biggest" operand type - an error is returned if converting the
// smaller one would mean precision loss. This error is not printed because once the
// desire type is known the operation might still turn out to be valid
//
// result_type, result attributes, result is unknown type, Operator Class, errormessage
std::tuple<llvm::Type*, unsigned, bool, OpClass, const char*> getResType(
	llvm::Type* left_type, llvm::Type* right_type, const char* Op,
	unsigned left_attr, unsigned right_attr, bool left_is_unknown_type, bool right_is_unknown_type)
{
	if ((left_attr & A_string) || (right_attr & A_string))
		return getStringRes(left_type, right_type, Op, left_attr, right_attr);
	bool left_is_signed = left_attr & A_signed;
	bool right_is_signed = right_attr & A_signed;
	auto [left_bitwidth, left_is_float] = getBitWidth(left_type);
	auto [right_bitwidth, right_is_float] = getBitWidth(right_type);
	auto opclass = getOpClass(Op);
	unsigned res_bitwidth;
	bool res_is_unknown_type = left_is_unknown_type & right_is_unknown_type;
	bool res_is_float = left_is_float || right_is_float;
	bool res_is_signed = left_is_signed && !left_is_unknown_type || right_is_signed && !right_is_unknown_type || left_is_signed && right_is_signed;
	switch (opclass) {
	case OpComparison:
		return { llvm::Type::getInt1Ty(Context), 0, false, opclass, nullptr };
	case OpDeclAssign:
		// nullptr as type is reserved for this particular case
		return { nullptr, 0, false, opclass, nullptr };
	case OpAssign:
	case OpModAssign:
		if (!left_is_float && right_is_float)
			return { nullptr, 0, false, opclass, "LHS of '%s' is of integer type - cannot automatically convert float RHS\n" };
		if (!left_is_signed && !left_is_float && right_is_signed && !right_is_unknown_type)
			return { nullptr, 0, false, opclass, "LHS of '%s' is of unsigned type - cannot automatically convert signed RHS\n" };
		if (left_bitwidth)
			if (right_bitwidth > left_bitwidth && !right_is_unknown_type && !(left_bitwidth == 1 && !right_is_signed))
				return { nullptr, 0, false, opclass, "LHS of '%s' has a lower bit width than RHS - automatic conversion not possible\n" };
		return { left_type, left_is_signed ? A_signed : 0, false, opclass, nullptr };
	case OpShift:
		if (res_is_float)
			return { nullptr, 0, false, opclass, "shift operator '%s' can only be used with integer operands\n" };
	case OpExponentiation:
		if (!right_is_unknown_type && right_bitwidth > left_bitwidth && (right_is_float || !left_is_float))
			res_bitwidth = right_bitwidth;
		else
			res_bitwidth = left_bitwidth;
		res_is_signed = left_is_signed;
		break;
	case OpLogical:
		if (left_is_float || (left_bitwidth != 1 && left_is_signed)
		    || right_is_float || (right_bitwidth != 1 && right_is_signed))
			return { nullptr, 0, false, opclass, "logical operator '%s' can only be used with bool or unsigned operands\n" };
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
	if (res_is_float)
		res_bitwidth = Min(res_bitwidth, 53); // limit to f64
	unsigned res_attr = 0;
	if (res_is_signed)
		res_attr |= A_signed;
	return { getFittingType(res_bitwidth, res_is_float), res_attr, res_is_unknown_type, opclass, nullptr };
}

std::tuple<llvm::Type*, bool> MakeType(llvm::Type* type, bool is_signed, bool is_unknown_type) {
	if(!is_unknown_type)
		return { type, is_signed };
	if (type->isIntegerTy())
		return { llvm::Type::getInt32Ty(Context) , true };
	else
		return { type, false };
}

// get element type of an array
volvoxc::FullType* getCommonType(std::vector<ExprAST*>& valid_exprs) {
	bool is_unsigned = false;
	bool is_signed = false;
	bool is_float = false;
	unsigned bitwidth = 0;
	SourceLocation MaxBWLoc;
	volvoxc::FullType* res_ft = nullptr;
	for (auto& elem: valid_exprs) {
		if (!elem || !elem->ft || ! elem->ft->type)
			return nullptr;
		if (res_ft) {
			if (elem->ft->type != res_ft->type) { // TODO: implement FullType comparison
				errs() << elem->Loc << ": array element types do not match\n";
				return nullptr;
			}
		} else {
			if (elem->ft->type->isFloatingPointTy() || elem->ft->type->isIntegerTy()) {
				auto bw = getBitWidth(elem->ft->type);
				if (bw.first > bitwidth) {
					bitwidth = bw.first;
					MaxBWLoc = elem->Loc;
				}
				is_float = is_float || bw.second;
				if (!elem->is_unknown_type) {
					if (elem->ft->type_attr & A_signed)
						is_signed = true;
					else
						is_unsigned = true;
				}
			} else {
				if (bitwidth) {
					errs() << elem->Loc << ": type of element does not match previous element types in same aggregate\n";
					return nullptr;
				}
				res_ft = elem->ft;
			}
		}
	}
	if (res_ft)
		return res_ft;
	if (!bitwidth) {
		errs() << "no valid element in array initialization\n";
		return nullptr;
		}
	if (is_float && bitwidth > 53) {
		errs() << MaxBWLoc << ": 64 bit integer in array initialization not compatible with float type(s)\n";
		return nullptr;
	}
	llvm::Type* res_type = getFittingType(bitwidth, is_float);
	auto type_name = lex.get_type_name(res_type, (!is_unsigned || is_signed) && !is_float);
	// TODO: implement full type lookup that doesn't need getting name string
	res_ft = lex.get_full_type(type_name);
	// errs() << List->Loc << " got fitting type for array elements: " << *res_ft << '\n';
	return res_ft;
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
		GV->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(dim_array->getType()));
		llvm::Constant *Zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 0);
		llvm::Constant *Indices[] = {Zero, Zero};
		fields.push_back(llvm::ConstantExpr::getInBoundsGetElementPtr(GV->getValueType(), GV, Indices));
	} else {
		fields.push_back(llvm::ConstantInt::get(llvm_size_type, (uint64_t)(
			                                        ft->type->isFunctionTy() ? target_bytes : TheModule->getDataLayout().getTypeAllocSize(ft->type))));
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
			if (ft->type_attr & A_union)
				// only index '0' should be printed as struct field
				if (index)
					continue;
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
			fields.push_back(getRtType(new_FullType(elem_type, ft->type_attr & A_signed)));
		} else {
			fields.push_back(llvm::ConstantPointerNull::get(llvm::Type::getInt8PtrTy(Context)));
		}
	}
	llvm::Constant* rt_const = llvm::ConstantStruct::getAnon(Context, fields, true);
	auto *GV = new llvm::GlobalVariable(*TheModule, rt_const->getType(), true, llvm::GlobalValue::PrivateLinkage, rt_const);
	GV->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(rt_const->getType()));
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
		  ArgPos(std::move(_ArgPos)), IsVarArgs(IsVarArgs), visibility(visibility), link_typ(link_type(visibility))
{
	size_t ret_size = (!(visibility & A_constructor) && RetType->type->isSized()) ?
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
	if (visibility & (A_constructor | A_destructor)) {
		llvm_ret_type = llvm::Type::getVoidTy(Context);
	} else if (ret_size <= 16) {
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
	if (!ft->mangled_name || !(ft->type_attr & (is_constructor ? A_constructor : A_destructor)) && !is_created)
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

// there is another "FullType" printing routine in mangler.cc - use reference here to distinguish
llvm::raw_ostream& print_ft(llvm::raw_ostream& out, llvm::Type* type, unsigned type_attr, volvoxc::FullType* ft_elem_type) {
	if (!type)
		return out << "<nil>";
	// print LLVM type for now - TODO: print canonical Volvox names instead
	if (type->isBFloatTy())
		return out << "f16";
	if (type->isFloatTy())
		return out << "f32";
	if (type->isDoubleTy())
		return out << "f64";
	if (type->isX86_FP80Ty())
		return out << "f80";
	if (type->isFP128Ty())
		return out << "f128";
	if (auto inttype = llvm::dyn_cast<llvm::IntegerType>(type)) {
		unsigned bw = inttype->getBitWidth();
		if (bw == 1)
			return out << "bool";
		else
			return out << ((type_attr & A_signed) ? 'i' : 'u') << bw;
	}
	if (auto arraytype = llvm::dyn_cast<llvm::ArrayType>(type)) {
		llvm::Type* elem_type;
		do {
			uint64_t n_elem = arraytype->getNumElements();
			out << '[';
			if (n_elem)
				out << n_elem;
			out << ']';
			elem_type = arraytype->getElementType();
			arraytype = llvm::dyn_cast<llvm::ArrayType>(elem_type);
		} while (arraytype);
		if (ft_elem_type) {
			return out << *ft_elem_type;
		} else {
			return print_ft(out, elem_type, type_attr);
		}
	}
	if (auto structtype = llvm::dyn_cast<llvm::StructType>(type)) {
		if (structtype->hasName())
			return out << structtype->getName();
		else
			return out << "<anonymous struct>";
	}
	return out << *type;
}

std::tuple<volvoxc::FullType*,volvoxc::FullType*,llvm::Type*> getKeyValueIteratorTypes(volvoxc::FullType* IteratorType, SourceLocation Loc) {
	if (!IteratorType || !IteratorType->type)
		return { nullptr, nullptr, nullptr };
	if (IteratorType->type_attr & A_map)
		return { new_FullType(IteratorType->type, IteratorType->type_attr & A_signed),
		         IteratorType->elem_type, llvm::Type::getInt8PtrTy(Context) };
	if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(IteratorType->type))
		// array could in principle be size_t, but only in rare cases
		// we default to int - if a 64-bit type is needed, it has to be predefined
		return { integer_type, IteratorType->elem_type, llvm::Type::getInt8PtrTy(Context) };
	if (IteratorType->type->isSingleValueType())
		// a single int/float 'n' can be used as an iterator for the range 0..(n-1)
		return { nullptr, IteratorType, nullptr };
	if (llvm::isa<llvm::StructType>(IteratorType->type)) {
		MapValue* mv = map_string_get(IteratorType->fields, "min");
		if (mv) {
			auto adr = (char*)mv + mv->offset + 4;
			volvoxc::FullType* ft;
			memcpy(&ft, adr, sizeof(void*));
			return { nullptr, ft, nullptr };
		}
		if (IteratorType->mangled_name) {
			auto protos = MethodProtos.find({IteratorType->mangled_name, "min"});
			if (protos != MethodProtos.end()) {
				std::vector<FnArg> fn_args = { FnArg{ nullptr, IteratorType->type,
				                                      static_cast<bool>(IteratorType->type_attr & A_signed), false } };
				auto selected_proto = selectProto(&protos->second, "min", fn_args, Loc);
				// errs() << "found " << protos->second.size() << " protos for 'min', selected #" << selected_proto << "\n";
				if (selected_proto >= 0)
					return { nullptr, protos->second[selected_proto]->RetType, nullptr };
			}
		}
	}
	return { nullptr, nullptr, nullptr };
}
