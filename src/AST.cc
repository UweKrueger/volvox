/*
 * Copyright © Uwe Krüger 2021, 2022, 2023, 2024, 2025, 2026
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include "../include/volvox.hh"

BinaryExprAST::BinaryExprAST(SourceLocation Loc, const char* _Op, std::unique_ptr<ExprAST> _LHS,
                             std::unique_ptr<ExprAST> _RHS, std::tuple<llvm::Type*, unsigned, bool, OpClass,
                             const char*> res_t)
	: ReferencableExprAST(std::get<0>(res_t), std::get<1>(res_t), Loc, std::get<2>(res_t)),
	  LHS(std::move(_LHS)), RHS(std::move(_RHS)), err_msg(std::get<4>(res_t)), opclass(std::get<3>(res_t)),
	  LREF(dynamic_cast<ReferenceExprAST*>(LHS.get()))
{
	strlcpy(Op, _Op, SZ_OPCODE);
	if (opclass == OpDeclAssign)
		LHS->ft = RHS->ft;
	if (opclass == OpRange && ft && ft->type) {
		auto limits_type_name = lex.get_type_name(ft->type, (bool)(ft->type_attr & A_signed));
		if (limits_type_name) {
#define RANGE_PREFIX "__range_"
#define RANGE_PREFIX_SIZE ARRAY_SIZE(RANGE_PREFIX) /* including terminating 0 */
			size_t name_sz = ARRAY_SIZE(RANGE_PREFIX) + strlen(limits_type_name);
			auto range_type_name = (char*)alloca(name_sz);
			strlcpy(range_type_name, RANGE_PREFIX, name_sz);
			strlcpy(range_type_name + (RANGE_PREFIX_SIZE - 1), limits_type_name, name_sz-(RANGE_PREFIX_SIZE - 1));
			ft = lex.get_full_type(range_type_name);
			if (ft)
				return;
		} else
			ft = nullptr;
		errs() << Loc << ": cannot create range from types " << *LHS->ft->type << " and " << *RHS->ft->type << "\n";
	}
}

StructExprAST::StructExprAST(SourceLocation Loc, volvoxc::FullType* ft, std::unique_ptr<ListExprAST> list)
	: ExprAST(ft, Loc), num_fields(llvm::dyn_cast<llvm::StructType>(ft->type) ?
	                               llvm::dyn_cast<llvm::StructType>(ft->type)->getNumElements() : 0),
	  initializers(num_fields)
{
	for (auto& field: list->Elements) {
		if (auto field_val = dynamic_cast<BinaryExprAST*>(field.get())) {
			if (field_val->Op[0] == ':' && !field_val->Op[1]) {
				std::string* field_key = nullptr;
				// we are only interested in the "ident" of the LHS of "ident: value"
				// the parser might have found the ident in tables so we have to handle these cases
				// it does not seem sensible to declare a common base class "NamedExprAST" to derive
				// these cases because 'VariableExprAST' is derived from 'LvalueExprAST', the others are not
				if (auto nameAST = dynamic_cast<VariableExprAST*>(field_val->LHS.get()))
					field_key = &nameAST->Name;
				else if (auto nameAST = dynamic_cast<FunctionExprAST*>(field_val->LHS.get()))
					field_key = &nameAST->Name;
				else if (auto nameAST = dynamic_cast<IdentExprAST*>(field_val->LHS.get()))
					field_key = &nameAST->Name;
				else if (auto nameAST = dynamic_cast<TypeExprAST*>(field_val->LHS.get()))
					field_key = &nameAST->Name;
				else {
					errs() << field_val->LHS->Loc << " field name expected\n";
					ft = nullptr;
					return;
				}
				if (field_key) {
					auto RHS_ptr = field_val->RHS.get();
					auto insert = Fields.try_emplace(*field_key, std::pair<std::unique_ptr<ExprAST>,SourceLocation*>{ std::move(field_val->RHS), nullptr });
					if (!insert.second) {
						errs() << field_val->LHS->Loc << ": field '" << field_key << "' already initialized\n";
						ft = nullptr;
						return;
					}
					register_usage_marker(RHS_ptr, &insert.first->second.second);
				}
				continue;
			}
			errs() << field->Loc << ": initializer with ':' expected - not '" << field_val->Op << "'\n";
		}
		errs() << field->Loc << ": binary expression as initializer expected\n";
	}
}

StructExprAST::~StructExprAST() {
	if (!codegen_done) // codege has not been processed, so clean up Fields
		for (auto& [fname, ini]: Fields)
			free(ini.second);
}

std::pair<llvm::Type*,llvm::Value*> ReferencableExprAST::codegen_ref(
	bool silent_fail, bool constref) {
	if (!ref_cache.first)
		ref_cache = codegen_ref_(silent_fail, constref);
	if (ref_cache.first) {
		if (!ref_cache.second && !silent_fail)
			errs() << Loc << ": cannot get reference\n";
	} else
		if (!ref_cache.second) // second is set for reference to set element
			errs() << Loc << ": error getting reference\n";
	return ref_cache;
}

VariableExprAST::VariableExprAST(SourceLocation Loc, const std::string &Name)
	: LvalueExprAST(Loc, Name), full_var(lookup_var(Name.c_str())) {
	if (full_var) {
		ft = &full_var->ft;
		if (ft->type_attr & A_untyped)
			is_unknown_type = true;
		if (full_var->var_usage_markers && !full_var->var_usage_markers->empty()) {
			LogicalLocation lloc(Loc, current_branch_part);
			for (auto it = full_var->var_usage_markers->begin(); it != full_var->var_usage_markers->end(); ) {
				if (lloc > it->loc) {
					if (it->flag_ptr) {
						auto save_loc = (SourceLocation*)malloc(sizeof(SourceLocation));
						*save_loc = Loc;
						*it->flag_ptr = save_loc;
					} else
						it = full_var->var_usage_markers->erase(it);
				} else
					it++;
			}
		}
	}
	// if the variable name has not found in the databases we don't generate
	// an error message here because this VariableExprAST could be the LHS of
	// an initialization e.g. `a = 42`
}

VariableExprAST::VariableExprAST(SourceLocation Loc, const std::string &Name, FullVar* fv)
	: LvalueExprAST(Loc, Name), full_var(fv) {
	if (full_var) {
		ft = &full_var->ft;
		if (ft->type_attr & A_untyped)
			is_unknown_type = true;
		if (full_var->var_usage_markers && !full_var->var_usage_markers->empty()) {
			LogicalLocation lloc(Loc, current_branch_part);
			for (auto it = full_var->var_usage_markers->begin(); it != full_var->var_usage_markers->end(); ) {
				if (lloc > it->loc) {
					auto save_loc = (SourceLocation*)malloc(sizeof(SourceLocation));
					*save_loc = Loc;
					*it->flag_ptr = save_loc;
					it = full_var->var_usage_markers->erase(it);
				} else
					it++;
			}
		}
	}
	else
		ft = nullptr;
}

SelectExprAST::SelectExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> _Struct, std::unique_ptr<IdentExprAST> _Field, bool silent_fail) :
	LvalueExprAST(Loc), Struct(std::move(_Struct)), Field(std::move(_Field))
{
	FieldName = Field->Name.c_str();
	if (Struct->ft && Struct->ft->type) {
		if (auto struct_type = llvm::dyn_cast<llvm::StructType>(Struct->ft->type)) {
			// for regular method call (having regular struct or interface as receiver)
			// 'getSelect()' in parser.cc creates a MethodExprAST (see above)
			// here we handle compiler built-in methods and regular struct fields
			if (Struct->ft->type_attr & A_thread) {
				if (!strcmp(FieldName, "wait")) {
					FieldIndex = 0;
					ft = Struct->ft->elem_type;
				} else if (!strcmp(FieldName, "kill")) {
					FieldIndex = 1;
					ft = void_type;
				} else {
					errs() << Struct->Loc << ": threads do not have a method '" << FieldName << "'\n";
					ft = nullptr;
				}
			} else if (MapValue* mv = map_string_get(&Struct->ft->fields, FieldName)) {
				FieldIndex = *(unsigned*)((char*)mv + mv->offset);
				char* adr = (char*)mv + mv->offset + 4;
				memcpy(&ft, adr, sizeof(void*));
			} else if (Struct->ft->type == llvm_vec_type) {
				if (!strcmp(FieldName, "_element_size")) {
					ft = size_type;
					FieldIndex = 0;
				}
			} else {
				llvm::StringRef struct_name = struct_type->hasName() ?
					struct_type->getName() :
					"<anonymous>";
				if (!silent_fail)
					errs() << Struct->Loc << ": struct type '" << struct_name << "' has no field named '"
					       << FieldName << "'\n";
				ft = nullptr;
			}
		} else if (Struct->ft->type == llvm_string_type) {
			if (!strcmp(FieldName, "size")) {
				FieldIndex = 0;
				ft = size_type;
			} else if (!strcmp(FieldName, "len")) {
				FieldIndex = 1;
				ft = size_type;
			} else {
				errs() << Struct->Loc << ": strings do not have a property '" << FieldName << "'\n";
				ft = nullptr;
			}
		} else if ((Struct->ft->type_attr & A_complex) && Struct->ft->type == llvm_c32_type) {
			if (!strcmp(FieldName, "real")) {
				FieldIndex = 0;
				ft = f32_type;
			} else if (!strcmp(FieldName, "imag")) {
				FieldIndex = 1;
				ft = f32_type;
			} else {
				errs() << Struct->Loc << ": c32 objects do not have a property '" << FieldName << "'\n";
				ft = nullptr;
			}
		} else if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(Struct->ft->type)) {
			if (!strcmp(FieldName, "size")) {
				FieldIndex = 0;
				ft = size_type;
			} else if (!strcmp(FieldName, "order")) {
				FieldIndex = 1;
				ft = integer_type;
			} else if (!strcmp(FieldName, "dim")) {
				auto fntype = llvm::FunctionType::get(llvm_size_type, { llvm_int_type }, false);
				ft = new_FullType(fntype, 0);
				ft->Protos = int_int_proto;
			} else {
				errs() << Struct->Loc << ": arrays do not have a property '" << FieldName << "'\n";
				ft = nullptr;
			}
		} else {
			errs() << Struct->Loc << ": LHS of '.' must be a struct (not " << *Struct->ft->type << ")\n";
			ft = nullptr;
		}
	} else {
		errs() << Struct->Loc << ": LHS of '.' has no defined type\n";
		ft = nullptr;
	}
}

IndexExprAST::IndexExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Field_,
                           std::unique_ptr<ExprAST> Index_, bool const_ref) :
	LvalueExprAST(Loc), Field(std::move(Field_)), Index(std::move(Index_)), const_ref(const_ref)
{
	if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(Field->ft->type)) {
		llvm::Type* elem_type = array_type->getElementType();
		if (elem_type == Field->ft->elem_type->type) {
			*ft = *Field->ft->elem_type;
		} else {
			*ft = *Field->ft;
			ft->type = elem_type;
		}
		return;
	} else if (Field->ft->type == llvm_map_type) {
		if (!Field->ft->elem_type[1].type) {
			ft = bool_type;
		}
		else
			ft = &Field->ft->elem_type[1];
		return;
	} else if (Field->ft->type == llvm_vec_type) {
		ft = Field->ft->elem_type;
		return;
	}
	errs() << Index->Loc << ": index for non array expression " << *Field->ft << ' ' << Field->ft->type_attr << "\n";
	ft->type = nullptr;
}

/* When variables are used as by-value function arguments or as struct field
   initializers it might be desirable to "move" these variables instead
   of making a valid copy calling the default (copy) constructor.
   To decide if this is possible we must keep track of the variable in question
   to know if it it is used later in the caller. So bool "usage_markers" are added to
   the CallExprAST and StructExprAST and pointers to these flags are added
   to the FullVar struct in the var table.
*/

void register_usage_marker(ExprAST* expr, SourceLocation** mark_ptr) {
	if (auto var_expr = dynamic_cast<VariableExprAST*>(expr)) {
		// it might be possible to move the variable if it isn't used later
		// so keep track of it
		if (var_expr->full_var && !(var_expr->full_var->ft.type_attr & (A_mainvar | A_global))) {
			if (!var_expr->full_var->var_usage_markers) {
				var_expr->full_var->var_usage_markers = new std::vector<var_usage_marker_t>();
				all_usage_markers.push_back(var_expr->full_var->var_usage_markers);
			}
			var_expr->full_var->var_usage_markers->emplace_back(
				var_expr->Loc, current_branch_part, mark_ptr);
		}
	}
}

/* When doing "codegen*()" for CallExprAST or StructExprAST we can find out
   based on these usage markers if we need to call the copy constructor
   and if the destructor needs to be called later (!is_moved). */

std::pair<bool,bool> needs_constructor_call_or_is_moved(
	arg_needs_constructor_t arg_needs_constructor,
	bool is_referenced_after_call)
{
	bool needs_constructor_call =
		arg_needs_constructor != arg_is_borrowed_or_pod
		&& (is_referenced_after_call || !inside_function) // TODO: force move when constructor invalidated
		&& (get_arg_flag(arg_needs_constructor, arg_is_owned)
		    || get_arg_flag(arg_needs_constructor, maybe_arg_is_owned));
	bool is_moved = (get_arg_flag(arg_needs_constructor, arg_is_owned)
	                 || get_arg_flag(arg_needs_constructor, maybe_arg_is_owned))
		&& !is_referenced_after_call
		&& inside_function // TODO: force move when constructor invalidated
		&& !inside_loop;
	return { needs_constructor_call, is_moved };
}

void register_destructor(SourceLocation& Loc, volvoxc::FullType* ft, llvm::Value* adr, bool is_value) {
	if (!(ft->type_attr & A_destructor))
		return; // nothing to do
	auto destructor = getConstructorOrDestructor(ft, true);
	if (!destructor) {
		errs() << Loc << ": cannot find destructor for type " << *ft << "\n";
		abort();
	}
	if (is_value) { // adr is actually a value that needs to be stored
		llvm::Value* tmpstore = CreateEntryBlockAlloca(adr->getType());
		Builder->CreateStore(adr, tmpstore);
		adr = tmpstore;
	}
	FullVar tmp = {
		.val = adr,
		.destructor = destructor,
		.ft = *ft
	};
	tmp.ft.type_attr &= ~(A_globally_visible | A_mainvar);
	expr_temps.push_back(tmp);
}

std::string SourceLocation::str() const {
	llvm::SmallString<128> buf;
	llvm::raw_svector_ostream locstr(buf);
	locstr.enable_colors(true);
	locstr << *this;
	std::string res(buf);
	return res;
}

std::string volvoxc::FullType::str() const {
	llvm::SmallString<128> buf;
	llvm::raw_svector_ostream locstr(buf);
	locstr.enable_colors(true);
	locstr << *this;
	std::string res(buf);
	return res;
}
