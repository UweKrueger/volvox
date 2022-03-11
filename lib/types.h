#pragma once

#include "map.h"
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>

namespace volvox {

	struct gen_val_type_t {
		llvm::Type::TypeID ID : 8; // base type
		unsigned SubclassData : 24;
	};

	struct FullType {
		llvm::Type* type; // used by compiler
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

	struct RtType;

	struct StructField {
		RtType* FieldType;
		const char* FieldName;
	};

	struct RtType {
		union {
			unsigned key;
			gen_val_type_t type;
		};
		unsigned type_attr;
		unsigned nrows; // arrays and struct types
		unsigned ncols; // only for matrix
		const char* name;
		union {
			const StructField* fields;
			const RtType* ElementType;
		};
	};

}
