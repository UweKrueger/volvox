#include "../include/volvox.hh"
#include "global.h"

llvm::SmallString<128> Mangle(std::vector<const char*>& names, std::vector<volvoxc::FullType*>& arg_types) {
	llvm::SmallString<128> mangled_name = llvm::StringRef("");
	llvm::raw_svector_ostream name(mangled_name);
	for (int i=0; i<100; i++)
		name << "Hallo duda ";
	return mangled_name;
}
