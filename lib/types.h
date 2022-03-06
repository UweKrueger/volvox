#pragma once

#include "map.h"
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>

struct gen_val_type_t {
	llvm::Type::TypeID ID : 8; // base type
	unsigned SubclassData : 24;
};

struct FullType {
	union {
		llvm::Type* type; // used by compiler
		gen_val_type_t rt_type; // used by rt-library
	};
	unsigned type_attr;
	int nrows; // nrows/ncolumns: -1 = flex-array, 0 = no array
	int ncolumns;
	int nelem; // for struct
	const char* type_name; // maybe NULL for anonymous types
	union {
		MapNode* elems; // element-name -> { index, FullType }
		FullType* elem_type;
	};
};

