/*
 * Copyright © Uwe Krüger 2021, 2022, 2023
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include "../include/volvox.hh"
#include "global.h"

llvm::raw_ostream& operator<<(llvm::raw_ostream& out, volvoxc::FullType* ft) {
	if (ft->type_attr & A_ref) // reference
		out << 'R';
	if (ft->type->isPointerTy()) {
		if (ft->type_attr & A_map) {
			out << 'M' << ft->elem_type;
		} else {
			out << 'P';
			if (ft->type_attr & A_shared)
				out << 'S';
			if (ft->type_attr & (A_const | A_cstring))
				out << 'K'; // C++ 'const char*' is what C functions typically expect - use 'PKc' for that
			if (ft->type_attr & (A_string | A_cstring))
				out << 'c'; // C++ 'char*' - this is not really the same as Volvox 'string', but close
			else if (!ft->elem_type || ft->elem_type->type->isVoidTy())
				out << 'v'; // C++ 'void*' - Volvox 'voidptr' - used for any C-specific pointer
			else {
				if (!(ft->type_attr & (A_unique | A_shared | A_const))) {
					errs() << "mangler: inconsistend type - pointers must be 'unique', 'shared' or 'const'" << ft << '\n';
					abort();
				}
				out << ft->elem_type; // A "native Volvox pointer" - used for unique objects... ;-)
			}
		}
	} else {
		auto is_signed = (bool)(ft->type_attr & A_signed);
		if (ft->type_attr & A_complex)
			if (ft->type->isStructTy())
				out << "Cd";
			else
				out << "Cf";
		else if (ft->type->isDoubleTy())
			out << (is_signed ? "Gd" : "d");
		else if (ft->type->isFloatTy())
			out << (is_signed ? "Gf" : "f");
		else if (auto intty = llvm::dyn_cast<llvm::IntegerType>(ft->type)) {
			auto bitwidth = intty->getBitWidth();
			if (bitwidth == 1)
				out << 'b';
			else if (bitwidth <= 8)
				out << (is_signed ? 'c' : 'h');
			else if (bitwidth <= 16)
				out << (is_signed ? 's' : 't');
			else if (bitwidth <= 32)
				out << (is_signed ? 'i' : 'j');
			else // i64
				out << (is_signed ? 'x' : 'y');
		} else if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(ft->type)) {
			// this is not defined in the Itanium mangle standard - we have to make up something on our own
			do {
				out << 'A' << array_type->getNumElements();
				array_type = llvm::dyn_cast<llvm::ArrayType>(array_type->getElementType());
			} while (array_type);
			out << ft->elem_type;
		} else {
			if (!ft->mangled_name || !*ft->mangled_name) {
				errs() << "Cannot mangle type " << *ft->type << '\n';
				return out;
			}
			out << ft->mangled_name;
		}
	}
	return out;
}

llvm::SmallString<128> MangleOp(llvm::SmallString<128> buf, const std::string& name, bool reverse, bool unary) {
	std::string Op;
	// strip off leading prefix from name ("unary+" -> "+")
	if (unary)
		Op = name.substr(5, name.length() - 5);
	else if (reverse)
		Op = name.substr(7, name.length() - 7);
	else  // binary
		Op = name.substr(6, name.length() - 6);
	llvm::raw_svector_ostream mangled(buf);
	if (unary) {
		if (Op == "+")
			mangled << "ps";
		else if (Op == "-")
			mangled << "ng";
		else {
			errs() << "mangler: unexpected unary operator \"" << Op << "\"\n";
			abort();
		}
	} else {
		if (Op == "+")
			mangled << "pl";
		else if (Op == "-")
			mangled << "mi";
		else if (Op == "*")
			mangled << "ml";
		else if (Op == "/")
			mangled << "dv";
		else if (Op == "%")
			mangled << "rm";
		else if (Op == "^")
			mangled << "eo";
		else if (Op == "=")
			mangled << "aS";
		else if (Op == "+=")
			mangled << "pL";
		else if (Op == "-=")
			mangled << "mI";
		else if (Op == "*=")
			mangled << "mL";
		else if (Op == "/=")
			mangled << "dV";
		else if (Op == "%=")
			mangled << "rM";
		else if (Op == "==")
			mangled << "eq";
		else if (Op == "!=")
			mangled << "ne";
		else if (Op == "<")
			mangled << "lt";
		else if (Op == ">")
			mangled << "gt";
		else if (Op == "<=")
			mangled << "le";
		else if (Op == ">=")
			mangled << "ge";
		else if (Op == "<=>")
			mangled << "ss";
		else {
			errs() << "mangler: unexpected binary operator \"" << Op << "\"\n";
			abort();
		}
	}
	return buf;
}

llvm::SmallString<128> MangleBase(llvm::SmallString<128> buf, const std::vector<std::string>& path,
                                  const std::string& name, const char* receiver_type_name, unsigned flags,
                                  bool is_op, bool reverse, bool unary) {
	llvm::raw_svector_ostream mangled(buf);
	if (!path.empty() || (flags & A_method)) {
		mangled << 'N';
		for (auto& dir : path)
			mangled << dir.size() << dir;
		if (receiver_type_name) {
			if (!isdigit(receiver_type_name[0]))
				mangled << strlen(receiver_type_name);
			mangled << receiver_type_name;
		}
	}
	if (flags & A_destructor)
		// strip '~' from beginning of unmangled name
		mangled << name.size()-1 << name.c_str()+1 << "D2";
	else if (flags & A_constructor)
		mangled << name.size() << name << "C2";
	else if (is_op)
		buf = MangleOp(buf, name, reverse, unary);
	else
		mangled << name.size() << name;
	if (!path.empty() || (flags & A_method) && !(flags & A_conversion))
		mangled << 'E';
	return buf;
}

llvm::SmallString<128> Mangle(const std::vector<std::string>& path, const std::string& name,
                              std::vector<volvoxc::FullType*>& arg_types, unsigned flags) {
	llvm::SmallString<128> buf = llvm::StringRef("_Z");
	char op = name[name.length()-1];
	bool reverse = false;
	bool unary = false;
	bool is_op = false;
	if (!isalnum(op) && op != '_') {
		is_op = true;
		if (name[0] == 'u')
			unary = true;
		else if (name[0] == 'r')
			reverse = true;
	}
	const char* rec_tname;
	if ((flags & A_method) && !(flags & (A_destructor | A_constructor | A_conversion)))
		if (reverse)
			rec_tname = arg_types[1]->mangled_name;
		else
			rec_tname = arg_types[0]->mangled_name;
	else
		rec_tname = nullptr;
	buf = MangleBase(buf, path, name, rec_tname, flags, is_op, reverse, unary);
	llvm::raw_svector_ostream mangled(buf);
	if (flags & A_conversion)
		mangled << "cv";
	if (arg_types.size() > 0 && !(flags & A_method) || arg_types.size() > 1) {
		unsigned idx = 0;
		if (reverse)
			mangled << arg_types[0];
		else
			for (auto type : arg_types) {
				if (idx++ || !(flags & A_method) || (flags & A_constructor_value_return))
					mangled << type;
			}
	} else
		mangled << 'v';
	if (flags & A_conversion)
		mangled << "Ev";
	return buf;
}

// Even anonymous functions and closures should have names that can be used
// as identification keys. Theses names will be "__anon_fn.0", "__anon_fn.1", ...
// so we maintain an index here

llvm::SmallString<16> createAnonFnName() {
	static unsigned anon_fn_idx = 0;
	llvm::SmallString<16> buf = llvm::StringRef("__anon_fn.");
	llvm::raw_svector_ostream key(buf);
	key << anon_fn_idx++;
	return buf;
}
