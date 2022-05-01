#include "../include/volvox.hh"
#include "global.h"

llvm::raw_ostream& operator<<(llvm::raw_ostream& out, std::pair<volvoxc::FullType*,bool> ft) {
	if (ft.second) // reference
		out << 'R';
	if (ft.first->type->isPointerTy())
		out << 'P' << std::pair<volvoxc::FullType*, bool>{ ft.first->elem_type, false };
	else
		if (ft.first->type->isDoubleTy())
			out << 'd';
		else if (ft.first->type->isFloatTy())
			out << 'f';
		else if (auto intty = llvm::dyn_cast<llvm::IntegerType>(ft.first->type)) {
			auto bitwidth = intty->getBitWidth();
			auto is_signed = (bool)(ft.first->type_attr & A_signed);
			if (bitwidth <= 8)
				out << (is_signed ? 'c' : 'h');
			else if (bitwidth <= 16)
				out << (is_signed ? 's' : 't');
			else if (bitwidth <= 32)
				out << (is_signed ? 'i' : 'j');
			else // i64
				out << (is_signed ? 'x' : 'y');
		} else {
			if (!ft.first->type_name || !*ft.first->type_name) {
				errs() << "Cannot mangle type " << *ft.first->type << '\n';
				return out;
			}
			out << strlen(ft.first->type_name) << ft.first->type_name;
		}
	return out;
}

llvm::SmallString<128> Mangle(std::vector<const char*>& names, std::vector<std::pair<volvoxc::FullType*,bool>>& arg_types) {
	llvm::SmallString<128> buf = llvm::StringRef("_Z");
	llvm::raw_svector_ostream mangled(buf);
	if (names.size() > 1)
		mangled << 'N';
	for (auto& name : names)
		mangled << strlen(name) << name;
	if (names.size() > 1)
		mangled << 'E';
	if (arg_types.size() > 0)
		for (auto type : arg_types)
			mangled << type;
	else
		mangled << 'v';
	return buf;
}
