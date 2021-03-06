/*
* Copyright 2018-2019 Redis Labs Ltd. and Contributors
*
* This file is available under the Apache License, Version 2.0,
* modified with the Commons Clause restriction.
*/

#ifndef _CLAUSE_SET_H
#define _CLAUSE_SET_H

#include "../ast_common.h"
#include "../../rmutil/vector.h"
#include "../../util/triemap/triemap.h"
#include "../ast_arithmetic_expression.h"

typedef struct {
	AST_Variable *entity;	/* Destination entity to update. */
	AST_ArithmeticExpressionNode *exp;	/* Arithmetic expression, evaluated value used for update. */
} AST_SetElement;

typedef struct {
	Vector *set_elements; /* Vector of AST_SetElement pointers, each describes an entity update. */
} AST_SetNode;

/* Set clause individual elements. */
AST_SetNode* New_AST_SetNode(Vector *elements);
AST_SetElement* New_AST_SetElement(AST_Variable *updated_entity, AST_ArithmeticExpressionNode *exp);
void SetClause_ReferredNodes(const AST_SetNode *set_node, TrieMap *referred_nodes);
void Free_AST_SetNode(AST_SetNode *setNode);

#endif