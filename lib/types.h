#pragma once

#include "map.h"
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>

struct FullType {
	llvm::Type* type;
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

