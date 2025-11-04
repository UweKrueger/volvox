/*
 * Copyright © Uwe Krüger 2021, 2022, 2023, 2024, 2025
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
	: ExprAST(ft, Loc) {
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
					auto insert = Fields.try_emplace(*field_key, std::pair<std::unique_ptr<ExprAST>,bool>{ std::move(field_val->RHS), false });
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
