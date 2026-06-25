/*
 * Copyright © Uwe Krüger 2021, 2022, 2023, 2024, 2025
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include "../include/volvox.hh"

//===----------------------------------------------------------------------
// type conversion handling
//===----------------------------------------------------------------------

unsigned anon_struct_nr = 0;

volvoxc::FTListElem* anon_types = nullptr;
volvoxc::FTListElem** anon_types_end = &anon_types;
std::vector<volvoxc::FullType*> all_interfaces;
std::map<volvoxc::FullType*,int> all_interface_idxs;

void ConversionErr(SourceLocation Loc, llvm::Type* expr_type, llvm::Type* desired_type,
                   bool expr_is_signed, bool desired_is_signed, const char* reason, bool is_explicit) {
	auto expr_type_name = lex.get_type_name((llvm::Type*)((uintptr_t)expr_type | (expr_is_signed ? A_signed : 0)));
	auto desired_type_name = lex.get_type_name((llvm::Type*)((uintptr_t)desired_type | (desired_is_signed ? A_signed : 0)));
	errs() << Loc << ": cannot " << (is_explicit ? "" : "automatically ") << "convert "
	       << expr_type_name << "/" << desired_type_name;
	if (reason)
		errs() <<  ": " << reason;
	errs() << "\n";
	abort();
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
	// case llvm::Type::FixedVectorTyID:
	// 	return getBitWidth(llvm::cast<llvm::FixedVectorType>(type)->getElementType());
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
	llvm::Type* expr_type, llvm::Type* desired_type, SourceLocation Loc, unsigned expr_attr,
	unsigned desired_attr, bool is_explicit, bool is_unknown_type, conv_match_t* match)
{
	bool expr_is_signed = expr_attr & A_signed;
	bool expr_is_imag = (expr_attr & A_imaginary) && (expr_type->isFloatTy() || expr_type->isDoubleTy());
	bool expr_is_complex = (bool)(expr_attr & A_complex);
	bool desired_is_signed = desired_attr & A_signed;
	bool desired_is_imag = (desired_attr & A_imaginary) && (desired_type->isFloatTy() || desired_type->isDoubleTy());
	bool desired_is_complex = (bool)(desired_attr & A_complex);
	if (!expr_type)
		return nullptr;
	if (expr_is_imag != desired_is_imag)
		return nullptr;
	if (expr_type == desired_type && (expr_is_signed == desired_is_signed || !expr_type->isIntegerTy())) {
		if (match)
			*match = exact_match;
		return NoConversion;
	}
	if (match)
		*match = conversion_match;
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
				mangled << struct_type->getName()  << "C1E";
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
			return ExplicitErr(Loc, expr_type, desired_type, expr_is_signed, desired_is_signed, "no conversion defined");
		}
	}
no_explicit_constructor:
	if (auto struct_type = llvm::dyn_cast<llvm::StructType>(expr_type)) {
		if (expr_type == llvm_string_type && desired_type->isPointerTy()
		    /* && (desired_attr & A_cstring) */) {
			return [=](llvm::Value* v) { return Volvox2CStr(Builder->CreateExtractValue(v, 0)); };
		}
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
			return ExplicitErr(Loc, expr_type, desired_type, expr_is_signed, desired_is_signed, "no conversion defined");
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
			if (match)
				*match = exact_match;
			return NoConversion;
		} else {
			return nullptr;
		}
	}
	if (desired_type->isPointerTy()) {
		if (is_explicit) // e.g. voidptr(0)
			return [=](llvm::Value* v) { return Builder->CreateIntToPtr(v, llvm_ptr_type, "inttoptr"); };
		else
			return AutoErr(Loc, expr_type, desired_type, expr_is_signed, desired_is_signed, "int -> voidptr");
	} else if (expr_type->isPointerTy()) {
		if (is_explicit) {
			if (desired_type == llvm_size_type)
				return [=](llvm::Value* v) { return Builder->CreatePtrToInt(v, desired_type, "ptrtoint"); };
			else if (desired_bitwidth == 1)
				return [=](llvm::Value* v) { return Builder->CreateIsNotNull(Builder->CreatePtrToInt(v, llvm_size_type), "ptrtobool"); };
			else
				return ExplicitErr(Loc, expr_type, desired_type, expr_is_signed, desired_is_signed, "no known conversion");
		} else {
			return AutoErr(Loc, expr_type, desired_type, expr_is_signed, desired_is_signed, "voidptr -> value");
		}
	}
	if (!desired_bitwidth || !expr_bitwidth)
		return nullptr;
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
		else if (llvm::isa<llvm::IntegerType>(expr_type))
			if (is_explicit || is_unknown_type || desired_bitwidth >= expr_bitwidth
			    || desired_bitwidth >= 53) { // always allow conversion to f64
				if (desired_bitwidth < 53 && desired_bitwidth < expr_bitwidth)
					if (match)
						*match = untyped_match;
				if (expr_is_signed)
					return [=](llvm::Value* v) { return Builder->CreateSIToFP(v, desired_type, "convsfptmp"); };
				else
					return [=](llvm::Value* v) { return Builder->CreateUIToFP(v, desired_type, "convufptmp"); };
			}
			else
				return AutoErr(Loc, expr_type, desired_type, expr_is_signed, desired_is_signed, "int->float would lose precision");
		else
			return ExplicitErr(Loc, expr_type, desired_type, expr_is_signed, desired_is_signed, "conversion to float not possible");
	else if (expr_type->isIntegerTy() || expr_type->isFloatTy() || expr_type->isDoubleTy())
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
						if (is_explicit || is_unknown_type) {
							if (match)
								*match = untyped_match;
							return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, false, "trunctmp"); };
						}
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
	else
		return ExplicitErr(Loc, expr_type, desired_type, expr_is_signed, desired_is_signed, "no known conversion");
}

inline static unsigned Max(unsigned a, unsigned b) { return (a > b) ? a : b; }
inline static unsigned Min(unsigned a, unsigned b) { return (a < b) ? a : b; }

// classification of binary operator with result type calculation in mind
OpClass getOpClass(const char* Op) {
	if (!Op[0])
		return OpNormal;
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
		case '?':
			return OpCmpExchange;
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
		case '?':
			return OpTernary;
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
	case OpCmpExchange:
	case OpModAssign:
		desired_left_type = nullptr;
		desired_right_type = left_type;
		goto normal_return;
	case OpDeclAssign:
		return { nullptr, nullptr, nullptr };
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

// When imaginary objects are involved things are somewhat different

// result_type, result attributes, result is unknown type, Operator Class, errormessage
std::tuple<llvm::Type*, unsigned, bool, OpClass, const char*> getComplexRes(
	llvm::Type* left_type, llvm::Type* right_type, const char* Op,
	unsigned left_attr, unsigned right_attr, bool left_is_unknown_type, bool right_is_unknown_type)
{
	auto [left_bitwidth, left_is_float] = getBitWidth(left_type);
	auto [right_bitwidth, right_is_float] = getBitWidth(right_type);
	auto opclass = getOpClass(Op);
	unsigned res_bitwidth;
	bool left_is_imag = left_is_float && (left_attr & A_imaginary);
	bool right_is_imag = right_is_float && (right_attr & A_imaginary);
	bool res_is_complex = false;
	bool res_is_imag = false;
	if (right_bitwidth == 1 || left_bitwidth == 1)
		return { nullptr, 0, false, opclass, "operator '%s' not supported for bool/imaginary\n" };
	switch (opclass) {
	case OpComparison:
		if (left_is_imag != right_is_imag)
			return { nullptr, 0, false, opclass, "invalid real / imaginary '%s' comparison\n" };
		if (!strcmp(Op, "==") && !strcmp(Op, "!="))
			return { nullptr, 0, false, opclass, "invalid '%s' comparison of imaginarys - only '==', '!=' allowed\n" };
		return { llvm::Type::getInt1Ty(Context), 0, false, opclass, nullptr };
	case OpDeclAssign:
		// nullptr as type is reserved for this particular case
		return { nullptr, 0, false, opclass, nullptr };
	case OpAssign:
	case OpModAssign:
		if (!strcmp(Op, "=") || !strcmp(Op, "+=") || !strcmp(Op, "-=")) {
			if (left_is_imag != right_is_imag)
				return { nullptr, 0, false, opclass, "LHS/RHS of '%s' real/imaginary mismatch\n" };
		} else if(!strcmp(Op, "*=") || !strcmp(Op, "/=")) {
			if (right_is_imag)
				return { nullptr, 0, false, opclass, "RHS of '%s' must not be imaginary\n" };
		} else {
			return { nullptr, 0, false, opclass, "operator '%s' not supported with imaginary numbers\n" };
		}
		if (left_bitwidth)
			if (right_bitwidth > left_bitwidth && !right_is_unknown_type)
				return { nullptr, 0, false, opclass, "LHS of '%s' has a lower bit width than RHS - automatic conversion not possible\n" };
		return { left_type, left_is_imag ? A_signed : 0, false, opclass, nullptr };
	case OpCmpExchange:
		if (left_is_imag != right_is_imag)
			return { nullptr, 0, false, opclass, "operator '%s': real / imaginary mismatch\n" };
		return { llvm::Type::getInt1Ty(Context), 0, false, opclass, nullptr };
	case OpExponentiation:
		// special cases like constexpr integer exponent mus be handled elsewhere
		res_is_complex = true;
		break;
	case OpNormal:
		if (!strcmp(Op, "+") || !strcmp(Op, "-")) {
			if (left_is_imag != right_is_imag)
				res_is_complex = true;
			else
				res_is_imag = left_is_imag;
		} else if (!strcmp(Op, "*") || !strcmp(Op, "/") || Op[0] == '\0') {
			res_is_imag = left_is_imag ^ right_is_imag;
		} else {
			return { nullptr, 0, false, opclass, "operator '%s' not supported with imaginary numbers\n" };
		}
		res_bitwidth = right_is_unknown_type ?
		(left_is_unknown_type ? Max(right_bitwidth, left_bitwidth)
		 : left_bitwidth) :
		left_is_unknown_type ? right_bitwidth : Max(right_bitwidth, left_bitwidth);
		break;
	default:
		return { nullptr, 0, false, opclass, "operator '%s' not supported with imaginary numbers\n" };
	}
	res_bitwidth = Min(res_bitwidth, 53); // limit to f64
	unsigned res_attr = 0;
	if (res_is_imag)
		res_attr |= A_imaginary;
	if (res_is_complex) {
		const char* complex_ty = (res_bitwidth >= 32) ? "complex" : "c32";
		auto ct = lex.get_full_type(complex_ty);
		if (!ct) {
			errs() << "cannot find declaration of builtin type " << complex_ty << "\n";
			abort();
		}
		return { ct->type, A_complex, false, opclass, nullptr };
	}
	return { getFittingType(res_bitwidth, true), res_attr, false, opclass, nullptr };
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
	auto opclass = getOpClass(Op);
	if (!left_type) {
		if (opclass == OpColon || opclass == OpComma || opclass == OpTernary)
			return { llvm::Type::getVoidTy(Context), left_attr, false, opclass, nullptr };
		else
			return { nullptr, 0, false, opclass, "no LHS type\n" };
	}
	if (left_type->isStructTy() || left_type->isArrayTy() || left_type->isPointerTy()) {
		if (right_type == left_type)
			return { left_type, left_attr & right_attr, false, opclass, nullptr };
		else
			return { nullptr, 0, false, opclass, "non-numeric LHS and RHS do not match\n" };
	}
	bool left_is_signed = (left_attr & A_signed);
	bool right_is_signed = (right_attr & A_signed);
	auto [left_bitwidth, left_is_float] = getBitWidth(left_type);
	auto [right_bitwidth, right_is_float] = getBitWidth(right_type);
	if (opclass != OpComma && opclass != OpBitwise && opclass != OpColon && opclass != OpTernary) {
		if (left_bitwidth == 1 && right_bitwidth != 1)
			return { nullptr, 0, false, opclass, "LHS of type 'bool' cannot be combined with numeric RHS value\n" };
		if (right_bitwidth == 1 && left_bitwidth != 1)
			return { nullptr, 0, false, opclass, "RHS of type 'bool' cannot be combined with numeric LHS value\n" };
	}
	unsigned res_bitwidth;
	bool res_is_unknown_type = left_is_unknown_type & right_is_unknown_type;
	bool res_is_float = left_is_float || right_is_float;
	bool res_is_signed = (left_is_signed && !left_is_unknown_type || right_is_signed && !right_is_unknown_type || left_is_signed && right_is_signed) && !res_is_float;
	bool left_is_imag = left_is_float & left_is_signed;
	bool right_is_imag = right_is_float & right_is_signed;
	if (left_is_imag || right_is_imag)
		return getComplexRes(left_type, right_type, Op, left_attr, right_attr, left_is_unknown_type, right_is_unknown_type);
	switch (opclass) {
	case OpComparison:
	case OpCmpExchange:
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
	case OpTernary:
		res_bitwidth = right_bitwidth;
		res_is_float = right_is_float;
		res_is_signed = right_is_signed;
		res_is_unknown_type = false;
		break;
	case OpBitwise:
		res_is_signed = false; // default to unsigned for (possibly bitwise) &, |, ><
		// fallthrough
	default:
		res_bitwidth = right_is_unknown_type ?
		(left_is_unknown_type ? Max(right_bitwidth, left_bitwidth)
		 : left_bitwidth) :
		left_is_unknown_type ? right_bitwidth : Max(right_bitwidth, left_bitwidth);
	}
	if (res_is_float)
		res_bitwidth = Min(res_bitwidth, 53); // limit to f64
	unsigned res_attr = 0;
	if (res_is_float)
		res_attr &= ~A_signed;
	else if (res_is_signed)
		res_attr |= A_signed;
	return { getFittingType(res_bitwidth, res_is_float), res_attr, res_is_unknown_type, opclass, nullptr };
}

std::tuple<llvm::Type*, unsigned> MakeType(llvm::Type* type, unsigned is_signed, bool is_unknown_type) {
	if(!is_unknown_type)
		return { type, is_signed };
	if (type->isIntegerTy())
		return { llvm::Type::getInt32Ty(Context) , A_signed };
	else
		return { type, 0 };
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

volvoxc::FullType* getCommonType(std::vector<std::unique_ptr<ExprAST>>& valid_exprs) {
	std::vector<ExprAST*> expr_ptrs;
	expr_ptrs.reserve(valid_exprs.size());
	for (auto& expr: valid_exprs)
		expr_ptrs.push_back(expr.get());
	return getCommonType(expr_ptrs);
}

// get runtime type, i.e. a description that is is passed at run time along with interface objects
llvm::Constant* getRtType(volvoxc::FullType* ft, llvm::Constant* vtable) {
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
	if (vtable)
		fields.push_back(vtable);
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
		if (ft->type_attr & A_thread) {
			TypeName = Builder->CreateGlobalString(
				"thread[" + ft->elem_type->str() + "]", "", 0, TheModule.get());
			ft = lex.source_stack.front().module->type_table.get_full("__thread");
		} else if (struct_type->hasName()) {
			TypeName = Builder->CreateGlobalString(struct_type->getName(), "", 0, TheModule.get());
		} else {
			TypeName = llvm::ConstantPointerNull::get(llvm_ptr_type);
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
			fld_descr.push_back(Builder->CreateGlobalString(field_name, "", 0, TheModule.get()));
			fld_descr.push_back(getRtType(field_ft));
			llvm::Constant* field = llvm::ConstantStruct::getAnon(Context, fld_descr, true);
			fields[idx_offs+index] = field;
		}
	} else {
		TypeName = llvm::ConstantPointerNull::get(llvm_ptr_type);
		fields.push_back(TypeName);
		if (llvmtype.ID == VOLVOX_ArrayTyID) {
			fields.push_back(getRtType(new_FullType(elem_type, ft->type_attr & (A_signed | A_complex))));
		} else {
			fields.push_back(llvm::ConstantPointerNull::get(llvm_ptr_type));
		}
	}
	llvm::Constant* rt_const = llvm::ConstantStruct::getAnon(Context, fields, true);
	if (false && comp_mode == comp_jit) { // we may need this later for dynamic types
		size_t rt_sz = TheModule->getDataLayout().getTypeAllocSize(rt_const->getType());
		void* GV_rt = malloc(rt_sz);
		auto GV = llvm::cast< llvm::Constant>(Builder->CreateIntToPtr(getSize((uintptr_t)GV_rt), llvm_ptr_type));
		Builder->CreateStore(rt_const, GV, true);
		return GV;
	}
	auto *GV = new llvm::GlobalVariable(*TheModule, rt_const->getType(), true, llvm::GlobalValue::PrivateLinkage, rt_const);
	GV->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(rt_const->getType()));
	llvm::Constant *Zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 0);
	llvm::Constant *Indices[] = {Zero, Zero};
	return llvm::ConstantExpr::getInBoundsGetElementPtr(GV->getValueType(), GV,
	                                                    Indices);
}

std::string volvoxc::FullType::str() {
	std::string buf;
	llvm::raw_string_ostream out(buf);
	out << *this;
	return buf;
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

std::pair<llvm::Type*,llvm::Type*> getReferenceType(llvm::Type* nominal_type) {
	if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(nominal_type)) {
		unsigned n_var_dim = 0;
		llvm::Type* el_type;
		do {
			uint64_t n_elem = array_type->getNumElements();
			if (!n_elem)
				n_var_dim++;
			el_type = array_type->getElementType();
			array_type = llvm::dyn_cast<llvm::ArrayType>(el_type);
		} while (array_type);
		if (n_var_dim) {
			std::vector<llvm::Type*> ref_el_types(n_var_dim+1, llvm_size_type);
			ref_el_types[n_var_dim] = llvm_ptr_type;
			auto struct_type = llvm::StructType::get(Context, ref_el_types);
			return { struct_type, el_type };
		} else {
			return { llvm_ptr_type, el_type };
		}
	} else if (nominal_type->isFunctionTy() || nominal_type == llvm_closure_type) {
		return { /* llvm_ptr_type */ llvm_closure_type, nullptr };
	} else {
		return { llvm_ptr_type, nullptr };
	}
}

PrototypeAST::PrototypeAST(SourceLocation Loc, const std::string &Name,
                           std::vector<std::string> _Args, unsigned visibility, SourceLocation retLoc,
                           unsigned IsOperator, volvoxc::FullType* RetType_,
                           std::vector<volvoxc::FullType*> _ArgTypes,
                           std::vector<arg_needs_constructor_t> _ArgNeedsConstructor,
                           std::vector<SourceLocation> _ArgPos, std::string _returnName,
                           bool IsVarArgs)
	: Name(Name), Args(std::move(_Args)), IsOperator(IsOperator), retLoc(retLoc),
	  Line(Loc.Line), RetType(RetType_ ? RetType_ : void_type), ArgTypes(std::move(_ArgTypes)),
	  ArgNeedsConstructor(std::move(_ArgNeedsConstructor)),
	  ArgPos(std::move(_ArgPos)), IsVarArgs(IsVarArgs), returnName(std::move(_returnName)),
	  visibility(visibility), link_typ(link_type(visibility))
{
	size_t ret_size = ((!(visibility & A_constructor) || !RetType->type->isStructTy()) && RetType->type->isSized()) ?
		TheModule->getDataLayout().getTypeAllocSize(RetType->type) :
		0;
	llvm::Type* llvm_ret_type;
	for (auto& argtype: ArgTypes) {
		llvm::Type* fn_arg_type = argtype->type;
		size_t argsize = argtype->type->isSized() ?
			TheModule->getDataLayout().getTypeAllocSize(fn_arg_type) : 0;
		if ((argtype->type_attr & A_ref) && !(argtype->type_attr & A_by_value)) {
			if (argsize) {
				ArgAttrs.push_back(llvm::AttributeSet::get(Context, llvm::ArrayRef<llvm::Attribute>{
							llvm::Attribute::getWithByRefType(Context, argtype->type),
							llvm::Attribute::getWithDereferenceableBytes(Context, argsize) }));
				fn_arg_type = llvm_ptr_type;
			} else {
				auto [ ref_type, el_type ] = getReferenceType(fn_arg_type);
				if (ref_type->isPointerTy())
					ArgAttrs.push_back(llvm::AttributeSet::get(Context, llvm::ArrayRef<llvm::Attribute>{
								llvm::Attribute::getWithByRefType(Context, argtype->type) }));
				else
					ArgAttrs.push_back(llvm::AttributeSet());
				fn_arg_type = ref_type;
			}
		} else {
			if (!argsize) {
				auto [ ref_type, el_type ] = getReferenceType(fn_arg_type);
				fn_arg_type = ref_type;
				ArgAttrs.push_back(llvm::AttributeSet());
			} else if (argtype->type_attr & A_by_value) { // Arguments > sret_limit bytes are always passed as pointer using copy-on-write
				if (argtype->type_attr & A_constructor)
					ArgAttrs.push_back(llvm::AttributeSet::get(Context, llvm::ArrayRef<llvm::Attribute>{
							llvm::Attribute::getWithByRefType(Context, argtype->type),
							llvm::Attribute::getWithDereferenceableBytes(Context, argsize) }));
				else
					ArgAttrs.push_back(llvm::AttributeSet::get(Context, llvm::ArrayRef<llvm::Attribute>{
							llvm::Attribute::getWithByValType(Context, argtype->type) }));
				fn_arg_type = llvm_ptr_type;
			} else {
				ArgAttrs.push_back(llvm::AttributeSet());
			}
		}
		LLVMArgTypes.push_back(fn_arg_type);
	}
	if ((visibility & (A_constructor | A_destructor)) && RetType->type->isStructTy()) {
		llvm_ret_type = llvm::Type::getVoidTy(Context);
	} else if (ret_size <= sret_limit) {
		llvm_ret_type = RetType->type;
	} else {
		IsStructRet = true;
		llvm::Type* struct_ret_type = llvm_ptr_type;
		LLVMArgTypes.insert(LLVMArgTypes.begin(), struct_ret_type);
		llvm_ret_type = llvm::Type::getVoidTy(Context);
	}
	// Volvox variadic function are passed an interface array as last argument
	// So only C-API functions are variadic for LLVM
	FT = llvm::FunctionType::get(llvm_ret_type, LLVMArgTypes, IsVarArgs && (visibility & A_c_api));
}

ProtoMatchKind CompareProtos(PrototypeAST* a, PrototypeAST* b) {
	unsigned arg_offset = 0;
	if ((a->visibility ^ b->visibility) & (A_method | A_constructor | A_destructor | A_setter))
		return protos_different;
	if ((a->visibility & A_method) && a->LLVMArgTypes[0] != b->LLVMArgTypes[0])
		return protos_different;
	size_t a_sz = a->ArgTypes.size();
	if (a_sz != b->ArgTypes.size())
		goto different;
	if (b->visibility & A_interface)
		arg_offset = 1; // do not compare receiver type for interface match
	for (unsigned i = arg_offset; i<a_sz; i++)
		if (FullTypes_differ(a->ArgTypes[i], b->ArgTypes[i]))
			goto different;;
	if (FullTypes_differ(a->RetType, b->RetType))
		return protos_conflicting;
	if ((a->visibility ^ b->visibility) & A_c_api)
		return (a->visibility & A_c_api) ?
			protos_conflicting_c_api_A :
			protos_conflicting_c_api_B;
	return protos_matching;
different:
	if ((a->visibility & b->visibility & A_c_api) && a->Name == b->Name)
		return protos_conflicting_c_signature;
	return protos_different;
}

llvm::Constant* getInterfaceVtable(SourceLocation Loc, volvoxc::FullType* ft,
                                   volvoxc::FullType* inter_face) {
	if (!inter_face || !inter_face->InterfaceProtos) {
		errs() << Loc << ": internal error - interface type inconsistent\n";
		abort();
	}
	llvm::ArrayType* array_type = std::get<0>(*inter_face->InterfaceProtos);
	std::map<std::string,std::vector<std::unique_ptr<PrototypeAST>>>*
		inter_protos = std::get<1>(*inter_face->InterfaceProtos).get();
	// methods[0] holds the size of the table - to find RtType at run time
	std::vector<llvm::Constant*> methods(array_type->getNumElements());
	if (!ft->mangled_name) {
		errs() << Loc << ": internal error - no mangled type name\n";
		abort();
	}
	methods[0] = llvm::cast<llvm::Constant>(
		Builder->CreateIntToPtr(getSize(target_bytes * methods.size()), llvm_ptr_type));
	std::string mangledType = ft->mangled_name;
	auto struct_type = llvm::dyn_cast<llvm::StructType>(ft->type);
	if (!struct_type) {
		errs() << Loc << ": '" << *ft << "' is not a struct type\n";
		return nullptr;
	}
	auto struct_layout = TheModule->getDataLayout().getStructLayout(struct_type);
	for (auto& [ident, protos] : *inter_protos) {
		std::vector<std::unique_ptr<PrototypeAST>> getter_setter_protos;
		volvoxc::FullType* virt_field_ft = nullptr;
		if (protos.size() == 2 && (protos[0]->visibility & A_getter)) {
			// virtual field - first check if it's a real field
			size_t idx = protos[0]->vtable_offs;
			virt_field_ft = protos[0]->RetType;
			if (MapValue* mv = map_string_get(ft->fields, ident.c_str())) {
				unsigned FieldIndex = *(unsigned*)((char*)mv + mv->offset);
				auto f_f_loc = (FieldTypeLoc*)((char*)mv + mv->offset + 4);
				if (FullTypes_differ(f_f_loc->ft, protos[0]->RetType)) {
					errs() << Loc << ": type '" << *ft << "' does not implement interface '"
					       << *inter_face << "' - type mismatch for field '" << ident << "'\n"
					       << protos[0]->retLoc << ": declared as virtual field of type '"
					       << *protos[0]->RetType << "'\n";
					errs() << f_f_loc->Loc << ": but as data field of type '" << *f_f_loc->ft << "'\n";
					return nullptr;
				}
				auto FieldOffset = struct_layout->getElementOffset(FieldIndex);
				methods[idx++] = llvm::ConstantPointerNull::get(llvm_ptr_type);
				methods[idx] = llvm::cast<llvm::Constant>(Builder->CreateIntToPtr(getSize(FieldOffset), llvm_ptr_type));
				continue;
			}
		}
		auto ft_protos = MethodProtos.find({ mangledType, ident });
		if (ft_protos == MethodProtos.end()) {
			errs() << Loc << ": type '" << *ft << "' does not implement interface '" << *inter_face
			       << (virt_field_ft ? "' - no virtual field (data field or method pair) '" : "' - no method '")
			       << ident << "'\nrequired prototypes:\n";
			printAllProtos(&protos, ident.c_str());
			return nullptr;
		}
		unsigned vecsize = protos.size();
		for (unsigned selidx = 0; selidx<vecsize; selidx++) {
			size_t idx = protos[selidx]->vtable_offs;
			for (auto& ft_proto : ft_protos->second) {
				if (CompareProtos(ft_proto.get(), protos[selidx].get()) == protos_matching) {
					auto func_expr = std::make_unique<FunctionExprAST>(Loc, ident, &ft_protos->second, selidx);
					func_expr->need_address = true;
					llvm::Value* fn_adr = func_expr->codegen_raw();
					if (!fn_adr) {
						errs() << Loc << ": cannot get address for method '" << ident << "'\n";
						return nullptr;
					}
					auto const_adr = llvm::dyn_cast<llvm::Constant>(fn_adr);
					if (!const_adr) {
						errs() << Loc << ": cannot get constant address for method '" << ident << "'\n";
						return nullptr;
					}
					methods[idx] = const_adr;
					goto match_found;
				}
			}
			errs() << Loc << ": type '" << *ft << "' does not implement interface '"
			       << *inter_face << "' - no match for method\n";
			printCandidate(protos[selidx].get(), ident.c_str());
			errs() << "candidates are:\n";
			printAllProtos(&ft_protos->second, ident.c_str());
			return nullptr;
		match_found:
			continue;
		}
	}
	return llvm::ConstantArray::get(array_type, methods);
}

void destroy_FV(MapValue* mapval) {
	auto var = (FullVar*)((char*)mapval + mapval->offset);
	var->destroy();
}

llvm::Function* createConstructorOrDestructorFnProto(volvoxc::FullType* ft, bool is_constructor, bool is_basic) {
	if (!ft->mangled_name)
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
	fn_name += (is_constructor ? (is_basic ? "C2Ev" : "C1Ev") : "D1Ev");
	auto FT = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), { llvm_ptr_type }, false);
	auto F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage, fn_name, TheModule.get());
	auto thisarg = F->getArg(0);
	auto argsize = ft->type->isSized() ?
		TheModule->getDataLayout().getTypeAllocSize(ft->type) : 0;
	auto attr_set = llvm::AttributeSet::get(Context, llvm::ArrayRef<llvm::Attribute>{
			llvm::Attribute::getWithByRefType(Context, ft->type),
			llvm::Attribute::getWithDereferenceableBytes(Context, argsize)
		});
	llvm::AttrBuilder attr_builder(Context, attr_set);
	thisarg->addAttrs(attr_builder);
	thisarg->setName("this");
	return F;
}

// there is another "FullType" printing routine in mangler.cc - use reference here to distinguish
llvm::raw_ostream& print_ft(llvm::raw_ostream& out, llvm::Type* type, unsigned type_attr, const volvoxc::FullType* ft_elem_type) {
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
	if (type == llvm_map_type) {
		return out << "map[" << ft_elem_type[0] << "]" << ft_elem_type[1];
	}
	if (auto structtype = llvm::dyn_cast<llvm::StructType>(type)) {
		if (structtype->hasName())
			return out << structtype->getName();
		else
			return out << "<anonymous struct>";
	} else if (llvm::isa<llvm::PointerType>(type)) {
		if (type_attr & A_cstring)
			return out << "cstring";
	}
	return out << *type;
}

std::tuple<volvoxc::FullType*,volvoxc::FullType*,llvm::Type*> getKeyValueIteratorTypes(volvoxc::FullType* IteratorType, SourceLocation Loc) {
	if (!IteratorType || !IteratorType->type)
		return { nullptr, nullptr, nullptr };
	if (IteratorType->type == llvm_map_type)
		return { &IteratorType->elem_type[0], &IteratorType->elem_type[1], llvm_ptr_type };
	if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(IteratorType->type))
		// array could in principle be size_t, but only in rare cases
		// we default to int - if a 64-bit type is needed, it has to be predefined
		return { integer_type, IteratorType->elem_type, llvm_ptr_type };
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

volvoxc::FullType* new_FullType(llvm::Type* type, unsigned type_attr,
                                llvm::DIType* ditype, MapNode* fields, volvoxc::FullType* elem_type,
                                StructFieldType* fields_by_idx) {
	volvoxc::FTListElem* new_node = (volvoxc::FTListElem*)malloc(sizeof(volvoxc::FTListElem));
	new_node->next = nullptr;
	new_node->ft.type = type;
	new_node->ft.type_attr = type_attr;
	new_node->ft.selected_proto = 0;
	new_node->ft.mangled_name = nullptr; // it's an anonymous type
	new_node->ft.ditype = ditype;
	new_node->ft.fields = fields;
	new_node->ft.fields_by_idx = fields_by_idx;
	new_node->ft.elem_type = elem_type;
	*anon_types_end = new_node;
	anon_types_end = &new_node->next;
	return &new_node->ft;
}

volvoxc::FullType* new_FullType(const volvoxc::FullType& orig, unsigned add_attr, unsigned add_fields) {
	volvoxc::FTListElem* new_node = (volvoxc::FTListElem*)malloc(sizeof(volvoxc::FTListElem) + add_fields * sizeof(volvoxc::FullType));
	new_node->next = nullptr;
	new_node->ft = orig;
	new_node->ft.type_attr |= add_attr;
	*anon_types_end = new_node;
	anon_types_end = &new_node->next;
	return &new_node->ft;
}

void destroy_full_type(MapValue* mapval) {
	auto ft = (volvoxc::FullType*)((char*)mapval + mapval->offset);
	if (ft->fields_by_idx) {
		;
		// free(ft->fields_by_idx);
		// errs() << "++++++++++ have ptr: " << (void*)ft->fields_by_idx << "\n";
	}
	// In order to really free melloc'ed arrays of FullType we would have
	// to do a deep copy above or do some reference counting...
	// Not freeing them is a memory leak - in theory...
	// However, we keep these structures to the end of the program, instead,
	// so Valgrind considers them as "still reachable"...
	// They are ultimately free'ed by the OS...
}

uint64_t get_ref_alloc_sz(llvm::Type* type) {
	uint64_t sz = target_bytes;
	if (auto arr = llvm::dyn_cast<llvm::ArrayType>(type)) {
		do {
			auto dim = arr->getNumElements();
			if (!dim)
				sz += target_bytes;
			arr = llvm::dyn_cast<llvm::ArrayType>(arr->getElementType());
		} while(arr);
	} else if (type->isFunctionTy()) {
		sz += target_bytes; // sizeof(__closure)
	}
	return sz;
}

std::vector<llvm::Value*> ReferencableExprAST::_getAllocSize(llvm::Type** el_ty) {
	std::vector<llvm::Value*> factors;
	int n = 0;
	if (auto array_ty = llvm::dyn_cast<llvm::ArrayType>(ft->type)) {
		auto ty_ref = codegen_ref(false, true);
		// refresh type - constant dims might have been updated
		array_ty = llvm::dyn_cast<llvm::ArrayType>(ft->type);
		if (!ty_ref.second) {
			n = 1;
			if (ty_ref.first)
				return factors;
			else
				goto failure;
		}
		unsigned dim_idx = 0;
		llvm::Type* elem_ty;
		auto struct_ty = llvm::dyn_cast<llvm::StructType>(ty_ref.second->getType());
		do {
			llvm::Value* NElem;
			auto n_elem = array_ty->getNumElements();
			elem_ty = array_ty->getElementType();
			if (n_elem) {
				NElem = getSize(n_elem);
			} else {
				n = 2;
				if (!struct_ty)
					goto failure;
				NElem = Builder->CreateExtractValue(ty_ref.second, dim_idx++);
				n = 3;
				if (NElem->getType() != llvm_size_type)
					goto failure;
			}
			factors.push_back(NElem);
			array_ty = llvm::dyn_cast<llvm::ArrayType>(elem_ty);
		} while (array_ty);
		if (el_ty) {
			*el_ty = elem_ty;
		} else {
			llvm::Value* NElem = getSize(TheModule->getDataLayout().getTypeAllocSize(elem_ty));
			factors.push_back(NElem);
		}
	} else {
		if (ft->type->isSized())
			factors.push_back(getSize(TheModule->getDataLayout().getTypeAllocSize(ft->type)));
		if (el_ty)
			*el_ty = nullptr;
	}
	return factors;
failure:
	errs() << Loc << ": internal error " << n << " - inconsistent array reference\n";
	factors.clear();
	return factors;
}

llvm::Value* ReferencableExprAST::getAllocSize(llvm::Type** el_ty) {
	auto sizes = _getAllocSize(el_ty);
	llvm::Value* const_factor = nullptr;
	llvm::Value* var_factor = nullptr;
	for (auto fact: sizes) {
		// multiply size factors (multi-dimensional array)
		// separate constant factors from variable factors for efficiency reasons
		if (llvm::isa<llvm::Constant>(fact)) {
			if (const_factor)
				const_factor = Builder->CreateMul(const_factor, fact);
			else
				const_factor = fact;
		} else {
			if (var_factor)
				var_factor = Builder->CreateMul(var_factor, fact);
			else
				var_factor = fact;
		}
	}
	if (!const_factor)
		return var_factor;
	if (!var_factor)
		return const_factor;
	return Builder->CreateMul(const_factor, var_factor);
}

std::vector<llvm::Value*> IndexExprAST::_getAllocSize(llvm::Type** el_ty) {
	std::vector<llvm::Value*> sizes;
	if (auto field_array = llvm::dyn_cast<llvm::ArrayType>(Field->ft->type)) {
		if (auto ref_field = dynamic_cast<ReferencableExprAST*>(Field.get())) {
			sizes = ref_field->_getAllocSize(el_ty);
			if (!sizes.empty())
				sizes.erase(sizes.begin());
			return sizes;
		}
		errs() << Loc << ": cannot get sizes of rvalue array\n";
	}
	if (ft->type->isSized())
		sizes.push_back(getSize(TheModule->getDataLayout().getTypeAllocSize(ft->type)));
	return sizes;
}

std::string get_LLVM_TypeName(llvm::Type* typ) {
	llvm::SmallString<128> buf = llvm::StringRef();
	llvm::raw_svector_ostream iter_type_name(buf);
	iter_type_name << *typ;
	std::string TypeName;
	TypeName.reserve(16);
	int idx = buf[0] == '%' ? 1 : 0;
	while (buf[idx] && buf[idx] != ' ') {
		TypeName += buf[idx];
		idx ++;
	}
	return TypeName;
}
