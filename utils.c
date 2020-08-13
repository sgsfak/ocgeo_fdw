#include "postgres.h"
#if PG_VERSION_NUM < 120000
#include "optimizer/var.h"
#else
#include "optimizer/optimizer.h"
#endif
#include "optimizer/clauses.h"
#include "optimizer/pathnode.h"
#include "optimizer/subselect.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_operator.h"
#include "mb/pg_wchar.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "miscadmin.h"
#include "parser/parsetree.h"
#include "pg_config.h"
#include "access/htup_details.h"
#include "nodes/makefuncs.h"



/* Third argument to get_attname was introduced in [8237f27] (release 11) */
#if PG_VERSION_NUM >= 110000
#define get_attname(x, y) get_attname(x, y, true)
#endif

Node       *unnestClause(Node *node);
void extractClauseFromOpExpr(Relids base_relids, OpExpr *node, List **quals);

Node       *unnestClause(Node *node);
void swapOperandsAsNeeded(Node **left, Node **right, Oid *opoid, Relids base_relids);
OpExpr     *canonicalOpExpr(OpExpr *opExpr, Relids base_relids);

Value* colnameFromVar(Var *var, PlannerInfo *root);
void extractRestrictions(Relids base_relids, Expr *node, List **quals);

/*
 * Swaps the operands if needed / possible, so that left is always a node
 * belonging to the baserel and right is either:
 *	- a Const
 *	- a Param
 *	- a Var from another relation
 */
OpExpr *
canonicalOpExpr(OpExpr *opExpr, Relids base_relids)
{
	Oid			operatorid = opExpr->opno;
	Node	   *l,
			   *r;
	OpExpr	   *result = NULL;

	/* Only treat binary operators for now. */
	if (list_length(opExpr->args) == 2)
	{
		l = unnestClause(list_nth(opExpr->args, 0));
		r = unnestClause(list_nth(opExpr->args, 1));
		swapOperandsAsNeeded(&l, &r, &operatorid, base_relids);
		if (IsA(l, Var) &&bms_is_member(((Var *) l)->varno, base_relids)
			&& ((Var *) l)->varattno >= 1)
		{
			result = (OpExpr *) make_opclause(operatorid,
											  opExpr->opresulttype,
											  opExpr->opretset,
											  (Expr *) l, (Expr *) r,
											  opExpr->opcollid,
											  opExpr->inputcollid);
		}
	}
	return result;
}


/*
 *	Build an intermediate value representation for an OpExpr,
 *	and append it to the corresponding list (quals, or params).
 *
 *	The quals list consist of list of the form:
 *
 *	- Const key: the column index in the cinfo array
 *	- Const operator: the operator representation
 *	- Var or Const value: the value.
 */
void
extractClauseFromOpExpr(Relids base_relids,
						OpExpr *op,
						List **quals)
{
	Var		   *left;
	Expr	   *right;

	/* Use a "canonical" version of the op expression, to ensure that the */
	/* left operand is a Var on our relation. */
	op = canonicalOpExpr(op, base_relids);
	if (op)
	{
		left = list_nth(op->args, 0);
		right = list_nth(op->args, 1);
		/* Do not add it if it either contains a mutable function, or makes */
		/* self references in the right hand side. */
		if (!(contain_volatile_functions((Node *) right) ||
			  bms_is_subset(base_relids, pull_varnos((Node *) right))))
		{
			*quals = lappend(*quals, 
                    list_make3(left, get_opname(op->opno), right));
		}
	}
}


/*
 *	Returns a "Value" node containing the string name of the column from a var.
 */
Value *
colnameFromVar(Var *var, PlannerInfo *root)
{
	RangeTblEntry *rte = rte = planner_rt_fetch(var->varno, root);
	char	   *attname = get_attname(rte->relid, var->varattno);

	if (attname == NULL)
		return NULL;
    return makeString(attname);
}

/*
 * Extract conditions that can be pushed down, as well as the parameters.
 *
 */
void
extractRestrictions(Relids base_relids, Expr *node, List **quals)
{
	switch (nodeTag(node))
	{
		case T_OpExpr:
			extractClauseFromOpExpr(base_relids, (OpExpr *) node, quals);
			break;
		default:
			{
				ereport(WARNING,
						(errmsg("unsupported expression for "
								"extractClauseFrom"),
						 errdetail("%s", nodeToString(node))));
			}
			break;
	}
}


void
swapOperandsAsNeeded(Node **left, Node **right, Oid *opoid,
                     Relids base_relids)
{
    HeapTuple   tp;
    Form_pg_operator op;
    Node       *l = *left,
               *r = *right;

    tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(*opoid));
    if (!HeapTupleIsValid(tp))
        elog(ERROR, "cache lookup failed for operator %u", *opoid);
    op = (Form_pg_operator) GETSTRUCT(tp);
    ReleaseSysCache(tp);
    /* Right is already a var. */
    /* If "left" is a Var from another rel, and right is a Var from the */
    /* target rel, swap them. */
    /* Same thing is left is not a var at all. */
    /* To swap them, we have to lookup the commutator operator. */
    if (IsA(r, Var))
    {
        Var        *rvar = (Var *) r;

        if (!IsA(l, Var) ||
            (!bms_is_member(((Var *) l)->varno, base_relids) &&
             bms_is_member(rvar->varno, base_relids)))
        {
            /* If the operator has no commutator operator, */
            /* bail out. */
            if (op->oprcom == 0)
            {
                return;
            }
            {
                *left = r;
                *right = l;
                *opoid = op->oprcom;
            }
        }
    }

}

/*
 * Returns the node of interest from a node.
 */
Node *
unnestClause(Node *node)
{
    switch (node->type)
    {
        case T_RelabelType:
            return (Node *) ((RelabelType *) node)->arg;
        case T_ArrayCoerceExpr:
            return (Node *) ((ArrayCoerceExpr *) node)->arg;
        default:
            return node;
    }
}
