#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"

//===----------------------------------------------------------------------
// type conversion handling
//===----------------------------------------------------------------------

unsigned anon_struct_nr = 0;

volvox::FTListElem* anon_types = nullptr;
volvox::FTListElem** anon_types_end = &anon_types;

std::nullptr_t AutoErr(SourceLocation Loc, llvm::Type* expr_type, llvm::Type* desired_type,
                              unsigned expr_attr, unsigned desired_attr, const char* reason) {
	errs() << Loc << ": cannot automatically convert "
	       << type_table.get_name((llvm::Type*)((uintptr_t)expr_type | (expr_attr & A_signed))) << "/"
	       << type_table.get_name((llvm::Type*)((uintptr_t)desired_type | (desired_attr & A_signed)));
	if (reason)
		errs() << reason;
	errs() << "\n";
	return nullptr;
}

static std::nullptr_t ExplicitErr(SourceLocation Loc, llvm::Type* expr_type, llvm::Type* desired_type,
                                  unsigned expr_attr, unsigned desired_attr, const char* reason) {
	errs() << Loc << ": cannot convert "
	       << type_table.get_name((llvm::Type*)((uintptr_t)expr_type | (expr_attr & A_signed))) << "/"
	       << type_table.get_name((llvm::Type*)((uintptr_t)desired_type | (desired_attr & A_signed)));
	if (reason)
		errs() << reason;
	errs() << "\n";
	return nullptr;
}

static llvm::Value* NoConversion(llvm::Value* v) { return v; }

// returns { significant_bits, is_float }
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
	if (is_float && bitwidth > 1) // bitwidth = 1 is always bool, i.e. u1
		if (bitwidth > 8)
			if (bitwidth > 24)
				return llvm::Type::getDoubleTy(Context);
			else
				return llvm::Type::getFloatTy(Context);
		else
			return llvm::Type::getBFloatTy(Context);
	else
		return llvm::IntegerType::get(Context, bitwidth);
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
	SourceLocation Loc, bool is_explicit, bool is_unknown_type)
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
			else if (is_explicit || is_unknown_type || desired_bitwidth >= expr_bitwidth)
				return [=](llvm::Value* v) { return Builder->CreateFPCast(v, desired_type, "convfptmp"); };
			else
				return AutoErr(Loc, expr_type, desired_type, expr_attr, desired_attr, "float truncation");
		else
			if (is_explicit || is_unknown_type || desired_bitwidth >= expr_bitwidth)
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
					if (!is_explicit && !is_unknown_type)
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
						if (is_explicit || is_unknown_type)
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
						if (is_explicit || is_unknown_type)
							return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, true, "trunctmp"); };
						else
							return AutoErr(Loc, expr_type, desired_type, expr_attr, desired_attr, "would truncate upper bits");
					else
						return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, true, "expandstmp"); };
				else
					// unsigned -> signed
					if (desired_bitwidth <= expr_bitwidth)
						if (is_explicit || is_unknown_type)
							if (desired_bitwidth == expr_bitwidth)
								return NoConversion;
							else
								return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, false, "trunctmp"); };
						else
							return AutoErr(Loc, expr_type, desired_type, expr_attr, desired_attr, "would truncate/reinterpret upper bits");
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

// min result, ideal result, result is signed, result has unknown type, errormessage
std::tuple<llvm::Type*, llvm::Type*, bool, bool, const char*> getResType(
	unsigned left_bitwidth, bool left_is_float, bool left_is_signed, bool left_is_unknown_type,
	unsigned right_bitwidth, bool right_is_float, bool right_is_signed, bool right_is_unknown_type, const char* Op)
{
	unsigned res_bitwidth_min = right_is_unknown_type ?
		left_is_unknown_type ? Min(right_bitwidth, left_bitwidth) : left_bitwidth
		:
		left_is_unknown_type ? right_bitwidth : Max(right_bitwidth, left_bitwidth);
	unsigned res_bitwidth = res_bitwidth_min; // will be refined based on operator
	unsigned res_is_float = left_is_float || right_is_float;
	bool res_is_signed = !res_is_float && (
		left_is_signed && !left_is_unknown_type
		|| right_is_signed && !right_is_unknown_type
		|| left_is_unknown_type && right_is_unknown_type);
	bool is_shift = false;
	bool is_logical = false;
	llvm::Type* res_type;
	// in simple cases one operand is converted to the type of the other
	// here we calculate the ideal result bitwidth to prevent data loss due to overflow
	if (Op[1] == '=') {
		if (Op[0] == '>' || Op[0] == '<' || Op[0] == '!') {
			res_bitwidth = res_bitwidth_min = 1;
			goto comparison;
		}
		res_bitwidth_min = res_bitwidth = left_bitwidth;
		if ((right_bitwidth > left_bitwidth && !right_is_unknown_type)
		    || !left_is_float &&
		    (right_is_float ||
		     !left_is_signed && right_is_signed && !right_is_unknown_type ||
		     left_is_signed && !right_is_signed && right_bitwidth >= left_bitwidth))
			return { nullptr, nullptr, 0, false, "illegal usage of %s: RHS would degrade\n" };
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
		if (Op[1] == '<')
			goto op_xor;
	case '<':
	case '=':
	comparison:
		if (Op[1] == Op[0]) {
			// <<, >> TODO: forbid float
			is_shift = true; // to allow signed / unsigend mismatch
			res_bitwidth_min = left_bitwidth;
			res_bitwidth = Op[0] == '<' ? 64 : res_bitwidth_min;
			res_is_signed = left_is_signed;
		} else {
			res_bitwidth = 1;
			if (Op[0] == '=') {
				// this is an assignment by default, i.e. if no bool result is expected
				res_bitwidth_min = left_bitwidth;
				res_is_float = left_is_float;
				res_is_signed = left_is_signed;
			} else {
				res_bitwidth_min = 1;
			}
		}
		break;
	case '&':
	case '|':
	op_xor:
		res_is_signed = left_is_signed && right_is_signed; // default to unsigned for (possibly bitwise) &, |, ><
		break;
	default:
		;
	}
calc_types:
	bool left_is_promoted = (res_bitwidth_min > left_bitwidth || res_is_float && !left_is_float) && !left_is_unknown_type;
	bool right_is_promoted = (res_bitwidth_min > right_bitwidth || res_is_float && !right_is_float) && !is_shift && !left_is_unknown_type;
	llvm::Type* def_type = (left_is_promoted && right_is_promoted) ?
		nullptr : // forbid both-side promotion as default
		getFittingType(res_bitwidth_min, res_is_float);
	return { def_type, getFittingType(res_bitwidth, res_is_float), res_is_signed,
		left_is_unknown_type && (right_is_unknown_type || is_shift),
		def_type ? nullptr : "would require promotions on both sides" };
}

// compute the conversion functions for binary Operators
BinOpConvSet convBinOp(llvm::Type* left_type, llvm::Type* right_type, unsigned left_attr, unsigned right_attr,
                       bool left_is_unknown_type, bool right_is_unknown_type,
                       const char* Op, SourceLocation Loc)
{
	if (!strcmp(Op, ":=") && left_type) {
		errs() << "internal error\n";
		abort();
	}
	if (!left_type || Op[0] == ',') {// variable declaration, i.e. := operator
		dbgs() << "### no conversion\n";
		return {{ nullptr, nullptr, nullptr, 0, false, nullptr }, { nullptr, nullptr, nullptr, 0, false, nullptr }};
	}
	if (!left_type->isSingleValueType() || !right_type->isSingleValueType()) {
		return {{ nullptr, nullptr, left_type, left_attr, false, nullptr }, { nullptr, nullptr, left_type, left_attr, false, nullptr }};
	}
	auto left_descr = getBitWidth(left_type);
	unsigned left_bitwidth = left_descr.first;
	bool left_is_float = left_descr.second;
	bool left_is_signed = left_attr & A_signed;
	auto right_descr = getBitWidth(right_type);
	unsigned right_bitwidth = right_descr.first;
	bool right_is_float = right_descr.second;
	bool right_is_signed = right_attr & A_signed;

	// TODO: use C++-17 structured bindings instead of anonymous tuple in the future - for now:
	// std::get<0>(res_t): minimum (natual) type to convert operands to
	// std::get<1>(res_t): ideal result type that avoids overflow in the result, e.g i32*i32->i64
	// std::get<2>(res_t): if the result is signed
	// std::get<3>(res_t): if the result type is unknown (i.e. consists of number literals, only)
	// std::get<4>(res_t): error message if error has occured
	std::tuple<llvm::Type*, llvm::Type*, bool, bool, const char*> res_t = getResType(
		left_bitwidth, left_is_float, left_is_signed, left_is_unknown_type,
		right_bitwidth, right_is_float, right_is_signed, right_is_unknown_type, Op);
	llvm::Type* res_type_min = std::get<0>(res_t);
	llvm::Type* res_type = std::get<1>(res_t);
	bool res_is_signed = std::get<2>(res_t);
	bool res_is_unknown_type = std::get<3>(res_t);
	const char* err_msg = std::get<4>(res_t);
	unsigned res_bitwidth_min = getBitWidth(res_type_min).first;
	unsigned res_bitwidth = getBitWidth(res_type).first;
	auto left_conv_min = getConv(left_type, res_type_min, left_attr, res_is_signed ? A_signed : 0, Loc, false, left_is_unknown_type);
	auto right_conv_min = getConv(right_type, res_type_min, right_attr, res_is_signed ? A_signed : 0, Loc, false, right_is_unknown_type);
	auto left_conv = res_bitwidth_min <= res_bitwidth ? getConv(left_type, res_type, left_attr, res_is_signed ? A_signed : 0, Loc, false, left_is_unknown_type) : nullptr;
	auto right_conv = res_bitwidth_min <= res_bitwidth ? getConv(right_type, res_type, right_attr, res_is_signed ? A_signed : 0, Loc, false, right_is_unknown_type) : nullptr;
	return {{ left_conv_min, right_conv_min, res_type_min, res_is_signed, res_is_unknown_type, err_msg },
	        { left_conv, right_conv, res_type, res_is_signed, res_is_unknown_type, nullptr }};
}

std::tuple<llvm::Type*, std::function<llvm::Value*(llvm::Value*)>, bool> MakeType(llvm::Type* type, bool is_signed, bool is_unknown_type) {
	if(!is_unknown_type)
		return { type, NoConversion, is_signed };
	if (type->isIntegerTy())
		return { llvm::Type::getInt32Ty(Context), [=](llvm::Value* v) { return Builder->CreateSExtOrTrunc(v, llvm::Type::getInt32Ty(Context), "convintinit"); } , true };
	else
		return { type, NoConversion, false };
}

volvox::FullType* MakeType(volvox::FullType* base, bool is_unknown_type) {
	if (is_unknown_type && base->type->isIntegerTy()) {
		volvox::FullType* new_type = type_table.get_full("i32");
		if (!new_type) {
			errs() <<"Fatal: Could not find i32 type!\n";
			return nullptr;
		}
		return new_type;
	} else {
		return base;
	}
}

const char* AggregateExprAST::KindName() {
	switch(kind) {
	case FixedArray:
		return "FixedArray";
	case Struct:
		return "Struct";
	case FixedMatrix:
		return "FixedMatrix";
	case FixedVector:
		return "FixedVector";
	case Interval:
		return "Interval";
	case AnyFixed:
		return "AnyFixed";
	case Array:
		return "Array";
	case Vector:
		return "Vector";
	case Map:
		return "Map";
	case Matrix:
		return "Matrix";
	case AnyDyn:
		return "AnyDyn";
	default:
		return "<Internal Error>";
	}
}

llvm::Constant* getRtType(volvox::FullType* ft) {
	union {
		volvox::gen_val_type_t llvmtype;
		unsigned key;
	};
	llvmtype = volvox::gen_val_type_t{ .ID = ft->type->getTypeID(), .SubclassData = ((genType*)ft->type)->SubClassData() };
	llvm::SmallVector<llvm::Constant*, 16> fields;
	fields.push_back(llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), (uint64_t)key));
	fields.push_back(llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), (uint64_t)ft->type_attr));
	fields.push_back(llvm::ConstantInt::get(llvm::Type::getInt64Ty(Context), ft->num_fields));
	fields.push_back(llvm::ConstantInt::get(llvm::Type::getInt64Ty(Context), (uint64_t)(
		                                        ft->type->isFunctionTy() ? sizeof(char*) : TheModule->getDataLayout().getTypeAllocSize(ft->type))));
	fields.push_back(ft->type_name ? Builder->CreateGlobalStringPtr(ft->type_name, "", 0, TheModule.get()) : llvm::ConstantPointerNull::get(llvm::Type::getInt8PtrTy(Context)));
	if (llvmtype.ID == llvm::Type::ArrayTyID) {
		fields.push_back(getRtType(ft->elem_type));
	} else {
		fields.push_back(llvm::ConstantPointerNull::get(llvm::Type::getInt8PtrTy(Context)));
	}
	llvm::Constant* rt_const = llvm::ConstantStruct::getAnon(Context, fields, true);
	auto *GV = new llvm::GlobalVariable(*TheModule, rt_const->getType(), true, llvm::GlobalValue::PrivateLinkage, rt_const);
	llvm::Constant *Zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 0);
	llvm::Constant *Indices[] = {Zero, Zero};
	return llvm::ConstantExpr::getInBoundsGetElementPtr(GV->getValueType(), GV,
	                                                    Indices);
}

void volvox::FullType::dump(int fd) {
	llvm::raw_fd_ostream eout(fd, false, true
#if LLVM_VERSION_MAJOR >= 12
	                          , llvm::raw_ostream::OStreamKind::OK_FDStream
#endif
		);
	eout << "FullType " << (type_name ? type_name : "<anonymous>") << "\n";
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
