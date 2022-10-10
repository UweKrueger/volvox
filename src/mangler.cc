#include "../include/volvox.hh"
#include "global.h"

llvm::raw_ostream& operator<<(llvm::raw_ostream& out, volvoxc::FullType* ft) {
	if (ft->type_attr & A_ref) // reference
		out << 'R';
	if (ft->type->isPointerTy())
		out << 'P' << ft->elem_type;
	else
		if (ft->type->isDoubleTy())
			out << 'd';
		else if (ft->type->isFloatTy())
			out << 'f';
		else if (auto intty = llvm::dyn_cast<llvm::IntegerType>(ft->type)) {
			auto bitwidth = intty->getBitWidth();
			auto is_signed = (bool)(ft->type_attr & A_signed);
			if (bitwidth <= 8)
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
				out << 'M' << array_type->getNumElements();
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
	return out;
}

llvm::SmallString<128> MangleBase(llvm::SmallString<128> buf, const std::vector<std::string>& path,
                                  const std::string& name, const char* receiver_type_name, unsigned flags) {
	llvm::raw_svector_ostream mangled(buf);
	if (!path.empty() || (flags & A_method)) {
		mangled << 'N';
		for (auto& dir : path)
			mangled << dir.size() << dir;
		if (receiver_type_name)
			mangled << receiver_type_name;
	}
	if (flags & A_destructor)
		// strip '~' from beginning of unmangled name
		mangled << name.size()-1 << name.c_str()+1 << "D2";
	else if (flags & A_constructor)
		mangled << name.size() << name << "C2";
	else
		mangled << name.size() << name;
	if (!path.empty() || (flags & A_method))
		mangled << 'E';
	return buf;
}

llvm::SmallString<128> Mangle(const std::vector<std::string>& path, const std::string& name,
                              std::vector<volvoxc::FullType*>& arg_types, unsigned flags) {
	llvm::SmallString<128> buf = llvm::StringRef("_Z");
	const char* rec_tname;
	if ((flags & A_method) && !(flags & (A_destructor | A_constructor)))
		rec_tname = arg_types[0]->mangled_name;
	else
		rec_tname = nullptr;
	buf = MangleBase(buf, path, name, rec_tname, flags);
	llvm::raw_svector_ostream mangled(buf);
	if (arg_types.size() > 0 && !(flags & A_method) || arg_types.size() > 1) {
		unsigned idx = 0;
		for (auto type : arg_types) {
			if (idx || !(flags & A_method))
				mangled << type;
		}
	} else
		mangled << 'v';
	return buf;
}
