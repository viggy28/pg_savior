// clang-format off
#include <postgres.h>
#include <fmgr.h>
// clang-format on
#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_type_d.h"
#include "commands/tablecmds.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "nodes/value.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_node.h"
#include "parser/parse_type.h"
#include "parser/parse_utilcmd.h"
#include "rewrite/rewriteHandler.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"

PG_MODULE_MAGIC;

void _PG_init(void);

static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart_hook = NULL;
static ProcessUtility_hook_type prev_ProcessUtility_hook = NULL;

static bool pg_savior_enabled = true;
static bool pg_savior_bypass = false;
static int  pg_savior_max_rows_affected = 0;
static int  pg_savior_large_table_threshold_rows = 1000000;
static int  pg_savior_index_parent_depth = 0;

/* Check top-level DML and data-modifying statements nested in WITH clauses. */
static void
pg_savior_check_query(Query *query)
{
	ListCell *lc;

	if ((query->commandType == CMD_DELETE || query->commandType == CMD_UPDATE)
		&& (query->jointree == NULL || query->jointree->quals == NULL))
		ereport(ERROR,
				(errcode(ERRCODE_RAISE_EXCEPTION),
				 errmsg("pg_savior: %s without WHERE clause is blocked",
						query->commandType == CMD_DELETE ? "DELETE" : "UPDATE"),
				 errhint("Add a WHERE clause, or set pg_savior.bypass = on for this session.")));

	foreach (lc, query->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		if (IsA(cte->ctequery, Query))
			pg_savior_check_query((Query *) cte->ctequery);
	}
}

static void
pg_savior_post_parse_analyze(ParseState *pstate, Query *query, JumbleState *jstate)
{
	if (prev_post_parse_analyze_hook)
		prev_post_parse_analyze_hook(pstate, query, jstate);

	if (!pg_savior_enabled || pg_savior_bypass)
		return;

	pg_savior_check_query(query);
}

static void
pg_savior_check_estimated_rows(CmdType operation, double estimated_rows)
{
	if (estimated_rows <= pg_savior_max_rows_affected)
		return;

	ereport(ERROR,
			(errcode(ERRCODE_RAISE_EXCEPTION),
			 errmsg("pg_savior: %s estimated to affect %.0f rows, exceeds pg_savior.max_rows_affected (%d)",
					operation == CMD_DELETE ? "DELETE" : "UPDATE",
					estimated_rows,
					pg_savior_max_rows_affected),
			 errhint("Refine the WHERE clause, raise pg_savior.max_rows_affected, or set pg_savior.bypass = on. Run ANALYZE if the estimate looks wrong.")));
}

/* Walk initialized plan states so data-modifying CTEs are checked too. */
static bool
pg_savior_check_planstate(PlanState *planstate, void *context)
{
	Plan *plan = planstate->plan;

	(void) context;

	if (IsA(plan, ModifyTable))
	{
		ModifyTable *modify = (ModifyTable *) plan;

		if (modify->operation == CMD_DELETE || modify->operation == CMD_UPDATE)
		{
			Plan *source_plan = plan->lefttree != NULL ? plan->lefttree : plan;

			pg_savior_check_estimated_rows(modify->operation,
									 source_plan->plan_rows);
		}
	}

	return planstate_tree_walker(planstate, pg_savior_check_planstate, NULL);
}

static void
pg_savior_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	bool check_estimates = pg_savior_enabled
		&& !pg_savior_bypass
		&& pg_savior_max_rows_affected > 0;

	/* Reject ordinary top-level DML before ExecutorStart acquires its locks. */
	if (check_estimates
		&& (queryDesc->operation == CMD_DELETE
			|| queryDesc->operation == CMD_UPDATE)
		&& queryDesc->plannedstmt != NULL
		&& queryDesc->plannedstmt->planTree != NULL)
	{
		Plan *plan = queryDesc->plannedstmt->planTree;
		Plan *source_plan = plan->lefttree != NULL ? plan->lefttree : plan;

		pg_savior_check_estimated_rows(queryDesc->operation,
								 source_plan->plan_rows);
	}

	if (prev_ExecutorStart_hook)
		prev_ExecutorStart_hook(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	/* Initialized plan states expose ModifyTable nodes inside CTE subplans. */
	if (check_estimates && queryDesc->planstate != NULL)
		pg_savior_check_planstate(queryDesc->planstate, NULL);
}

/*
 * Look up reltuples for a relation by RangeVar, returning -1 if the relation
 * cannot be opened (does not exist, lacks privileges). Caller must already be
 * inside a transaction (which is true in any utility-statement context).
 */
static double
pg_savior_relation_tuples(RangeVar *relation)
{
	Relation	rel;
	double		reltuples;

	rel = relation_openrv_extended(relation, AccessShareLock, true);
	if (rel == NULL)
		return -1;

	reltuples = rel->rd_rel->reltuples;
	relation_close(rel, AccessShareLock);
	return reltuples;
}

static void
pg_savior_check_create_index(IndexStmt *stmt)
{
	Relation rel;
	bool	 partitioned;

	if (stmt->concurrent)
		return;

	/* An existing same-schema relation makes IF NOT EXISTS a harmless no-op. */
	if (stmt->if_not_exists && stmt->idxname != NULL)
	{
		rel = relation_openrv_extended(stmt->relation, ShareLock, true);

		if (rel != NULL)
		{
			Oid namespace = RelationGetNamespace(rel);
			Oid existing = get_relname_relid(stmt->idxname, namespace);

			if (OidIsValid(existing))
			{
				/* Lock heap then index, matching CREATE INDEX lock order. */
				LockRelationOid(existing, ShareLock);
				if (get_relname_relid(stmt->idxname, namespace) == existing)
				{
					relation_close(rel, NoLock);
					return;
				}
				UnlockRelationOid(existing, ShareLock);
			}
			relation_close(rel, ShareLock);
		}
	}

	/*
	 * PostgreSQL cannot build an index concurrently on a partitioned parent.
	 * Its documented online migration creates an invalid metadata-only parent
	 * index with ON ONLY, builds each leaf concurrently, then attaches them.
	 * Allow only that proven metadata-only parent operation.
	 */
	if (!stmt->relation->inh)
	{
		rel = relation_openrv_extended(stmt->relation, ShareLock, true);
		if (rel != NULL)
		{
			partitioned = rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE;
			if (partitioned)
			{
				/* Keep the target stable until core processes the allowed command. */
				relation_close(rel, NoLock);
				return;
			}
			relation_close(rel, ShareLock);
		}
	}
	else
	{
		/* Inspect only to provide an actionable partition-specific error. */
		rel = relation_openrv_extended(stmt->relation, AccessShareLock, true);
		if (rel != NULL)
		{
			partitioned = rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE;
			relation_close(rel, AccessShareLock);
			if (partitioned)
				ereport(ERROR,
						(errcode(ERRCODE_RAISE_EXCEPTION),
						 errmsg("pg_savior: recursive CREATE INDEX on a partitioned table is blocked"),
						 errhint("Create the parent index with CREATE INDEX ON ONLY, build each leaf "
								 "index with CREATE INDEX CONCURRENTLY, then attach them with ALTER "
								 "INDEX ... ATTACH PARTITION; or set pg_savior.bypass = on.")));
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_RAISE_EXCEPTION),
			 errmsg("pg_savior: CREATE INDEX without CONCURRENTLY is blocked"),
			 errhint("Use CREATE INDEX CONCURRENTLY (it cannot run in a transaction block), "
					 "or set pg_savior.bypass = on for this session.")));
}

/*
 * Return a column DEFAULT expression from either a raw grammar tree or a
 * transformed ColumnDef. Never modify the returned node: utility trees may be
 * marked read-only by the caller.
 */
static Node *
column_default_expression(ColumnDef *col)
{
	ListCell *lc;

	if (col == NULL)
		return NULL;
	if (col->raw_default != NULL)
		return col->raw_default;
	if (col->cooked_default != NULL)
		return col->cooked_default;

	foreach (lc, col->constraints)
	{
		Constraint *con = (Constraint *) lfirst(lc);

		if (con->contype == CONSTR_DEFAULT)
			return con->raw_expr;
	}
	return NULL;
}

/*
 * Match PostgreSQL's fast-default decision for an explicit column DEFAULT.
 * cookDefault resolves functions, operators, casts, and the target type;
 * planner preprocessing then exposes volatility hidden by SQL-function
 * inlining or default arguments. STABLE expressions are deliberately allowed.
 */
static bool
column_has_constraint_type(ColumnDef *col, ConstrType type)
{
	ListCell *lc;

	if (col == NULL)
		return false;
	foreach (lc, col->constraints)
	{
		Constraint *con = (Constraint *) lfirst(lc);

		if (con->contype == type)
			return true;
	}
	return false;
}

static bool
column_uses_serial_pseudotype(ColumnDef *col)
{
	char *name;

	if (col == NULL || col->typeName == NULL
		|| list_length(col->typeName->names) != 1)
		return false;

	name = strVal(linitial(col->typeName->names));
	return strcmp(name, "smallserial") == 0
		|| strcmp(name, "serial2") == 0
		|| strcmp(name, "serial") == 0
		|| strcmp(name, "serial4") == 0
		|| strcmp(name, "bigserial") == 0
		|| strcmp(name, "serial8") == 0;
}

static const char *
column_default_rewrite_op(ColumnDef *col, const char *queryString)
{
	Node	   *default_expr;
	ParseState *pstate;
	Node	   *cooked;
	Oid			typeid;
	int32		typmod;
	bool		is_volatile;

	if (col == NULL)
		return NULL;
	if (col->generated != '\0'
		|| column_has_constraint_type(col, CONSTR_GENERATED))
		return "ADD generated COLUMN";
	if (col->identity != '\0'
		|| column_has_constraint_type(col, CONSTR_IDENTITY))
		return "ADD identity COLUMN";
	if (column_uses_serial_pseudotype(col))
		return "ADD serial COLUMN";

	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;
	typenameTypeIdAndMod(pstate, col->typeName, &typeid, &typmod);

	/* Core cannot use attmissingval when domain constraints need checking. */
	if (DomainHasConstraints(typeid))
	{
		free_parsestate(pstate);
		return "ADD constrained-domain COLUMN";
	}

	default_expr = column_default_expression(col);
	if (default_expr == NULL)
	{
		free_parsestate(pstate);
		return NULL;
	}
	if (col->cooked_default != NULL)
		cooked = copyObject(col->cooked_default);
	else
		cooked = cookDefault(pstate,
							copyObject(default_expr),
							typeid,
							typmod,
							col->colname,
							col->generated);

	is_volatile = contain_volatile_functions_after_planning((Expr *) cooked);
	free_parsestate(pstate);

	return is_volatile ? "ADD COLUMN with volatile DEFAULT" : NULL;
}

/*
 * Decide whether this AlterTableCmd is the kind that triggers a rewrite or
 * validation scan of a large table. Returns a human-readable operation name.
 */
static bool
column_has_index_constraint(ColumnDef *col)
{
	ListCell *lc;

	if (col == NULL)
		return false;

	foreach (lc, col->constraints)
	{
		Constraint *con = (Constraint *) lfirst(lc);

		if (con->contype == CONSTR_PRIMARY
			|| con->contype == CONSTR_UNIQUE
			|| con->contype == CONSTR_EXCLUSION)
			return true;
	}
	return false;
}

/*
 * This is PostgreSQL's ATColumnChangeRequiresRewrite(), expressed using only
 * exported node types and helpers.  Keep it in sync with supported server
 * branches: PostgreSQL 14 through 17 have the same decision tree.
 */
static bool
pg_savior_column_change_requires_rewrite(Node *expr, AttrNumber varattno)
{
	Assert(expr != NULL);

	for (;;)
	{
		if (IsA(expr, Var) && ((Var *) expr)->varattno == varattno)
			return false;
		if (IsA(expr, RelabelType))
			expr = (Node *) ((RelabelType *) expr)->arg;
		else if (IsA(expr, CoerceToDomain))
		{
			CoerceToDomain *domain = (CoerceToDomain *) expr;

			if (DomainHasConstraints(domain->resulttype))
				return true;
			expr = (Node *) domain->arg;
		}
		else if (IsA(expr, FuncExpr))
		{
			FuncExpr *func = (FuncExpr *) expr;

			if (func->funcid != F_TIMESTAMPTZ_TIMESTAMP
				&& func->funcid != F_TIMESTAMP_TIMESTAMPTZ)
				return true;
			if (TimestampTimestampTzRequiresRewrite())
				return true;
			expr = (Node *) linitial(func->args);
		}
		else
			return true;
	}
}

/*
 * Transform a single ALTER TYPE command against rel without touching the
 * caller's utility tree, then reproduce ATPrepAlterColumnType's coercion and
 * planning steps.  Parse/coercion failures intentionally use core routines so
 * an invalid ALTER retains PostgreSQL's normal error rather than our guard.
 */
static bool
pg_savior_alter_type_requires_rewrite(Relation rel, AlterTableStmt *stmt,
									  AlterTableCmd *cmd,
									  const char *queryString)
{
	ColumnDef *rawdef = (ColumnDef *) cmd->def;
	ColumnDef *def = rawdef;
	AlterTableCmd *transformed_cmd = NULL;
	HeapTuple tuple;
	Form_pg_attribute attr;
	AttrNumber attnum;
	Oid targettype;
	int32 targettypmod;
	Oid targetcollid;
	Node *transform;
	ParseState *pstate;
	AclResult aclresult;
	bool is_expr;
	bool attrgenerated;

	/* transformAlterTableStmt is allowed to scribble, so give it a private tree. */
	if (rawdef->raw_default != NULL)
	{
		AlterTableStmt *one = copyObject(stmt);
		List *before = NIL;
		List *after = NIL;
		ListCell *lc;

		one->cmds = list_make1(copyObject(cmd));
		one = transformAlterTableStmt(RelationGetRelid(rel), one, queryString,
									  &before, &after);
		foreach (lc, one->cmds)
		{
			AlterTableCmd *candidate = (AlterTableCmd *) lfirst(lc);

			if (candidate->subtype == AT_AlterColumnType)
			{
				transformed_cmd = candidate;
				break;
			}
		}
		Assert(transformed_cmd != NULL);
		def = (ColumnDef *) transformed_cmd->def;
	}

	if (rel->rd_rel->reloftype)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot alter column type of typed table")));

	tuple = SearchSysCacheAttName(RelationGetRelid(rel), cmd->name);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						cmd->name, RelationGetRelationName(rel))));
	attr = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attr->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"", cmd->name)));
	if (attr->attgenerated && def->cooked_default != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
				 errmsg("cannot specify USING when altering type of generated column"),
				 errdetail("Column \"%s\" is a generated column.", cmd->name)));
	if (attr->attinhcount > 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot alter inherited column \"%s\"", cmd->name)));
	if (has_partition_attrs(rel,
						bms_make_singleton(attnum - FirstLowInvalidHeapAttributeNumber),
						&is_expr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot alter column \"%s\" because it is part of the partition key of relation \"%s\"",
						cmd->name, RelationGetRelationName(rel))));

	typenameTypeIdAndMod(NULL, def->typeName, &targettype, &targettypmod);
#if PG_VERSION_NUM >= 160000
	aclresult = object_aclcheck(TypeRelationId, targettype, GetUserId(), ACL_USAGE);
#else
	aclresult = pg_type_aclcheck(targettype, GetUserId(), ACL_USAGE);
#endif
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, targettype);
	targetcollid = GetColumnDefCollation(NULL, def, targettype);
	CheckAttributeType(cmd->name, targettype, targetcollid,
				   list_make1_oid(rel->rd_rel->reltype), 0);

	attrgenerated = attr->attgenerated != '\0';
	transform = def->cooked_default != NULL
		? copyObject(def->cooked_default) : NULL;
	if (transform == NULL)
		transform = (Node *) makeVar(1, attnum, attr->atttypid,
									 attr->atttypmod, attr->attcollation, 0);
	ReleaseSysCache(tuple);

	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;
	transform = coerce_to_target_type(pstate, transform, exprType(transform),
									 targettype, targettypmod,
									 COERCION_ASSIGNMENT,
									 COERCE_IMPLICIT_CAST, -1);
	if (transform == NULL)
	{
		if (def->cooked_default != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("result of USING clause for column \"%s\" cannot be cast automatically to type %s",
							cmd->name, format_type_be(targettype)),
					 errhint("You might need to add an explicit cast.")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("column \"%s\" cannot be cast automatically to type %s",
							cmd->name, format_type_be(targettype)),
					 !attrgenerated
					 ? errhint("You might need to specify \"USING %s::%s\".",
							   quote_identifier(cmd->name),
							   format_type_with_typemod(targettype, targettypmod))
					 : 0));
	}

	assign_expr_collations(pstate, transform);
	transform = (Node *) expression_planner((Expr *) transform);
	free_parsestate(pstate);

	return pg_savior_column_change_requires_rewrite(transform, attnum);
}

static const char *
pg_savior_alter_table_op(AlterTableCmd *cmd, const char *queryString)
{
	switch (cmd->subtype)
	{
		case AT_AddColumn:
		{
			const char *default_op = column_default_rewrite_op(
				(ColumnDef *) cmd->def, queryString);

			if (default_op != NULL)
				return default_op;
			if (column_has_index_constraint((ColumnDef *) cmd->def))
				return "ADD COLUMN with index-backed CONSTRAINT";
			return NULL;
		}

		case AT_AddConstraint:
		{
			Constraint *con = (Constraint *) cmd->def;

			if (con == NULL)
				return NULL;
			if (con->indexname == NULL
				&& (con->contype == CONSTR_PRIMARY
					|| con->contype == CONSTR_UNIQUE
					|| con->contype == CONSTR_EXCLUSION))
				return "ADD index-backed CONSTRAINT";
			if (con->indexname != NULL && con->contype == CONSTR_PRIMARY)
				return "ADD PRIMARY KEY USING INDEX";
			if (!con->skip_validation
				&& (con->contype == CONSTR_CHECK
					|| con->contype == CONSTR_FOREIGN))
				return "ADD validated CONSTRAINT";
			return NULL;
		}

		default:
			return NULL;
	}
}

static bool
string_list_contains(List *items, const char *value)
{
	ListCell *lc;

	foreach (lc, items)
	{
		if (strcmp((const char *) lfirst(lc), value) == 0)
			return true;
	}
	return false;
}

static bool
alter_statement_drops_column(AlterTableStmt *stmt, const char *name)
{
	ListCell *lc;

	foreach (lc, stmt->cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lc);

		if (cmd->subtype == AT_DropColumn
			&& cmd->name != NULL
			&& strcmp(cmd->name, name) == 0)
			return true;
	}
	return false;
}

/*
 * Core rejects a second ALTER TYPE after the first command changes the
 * column's type or typmod, before phase 3 can rewrite the heap.  Let core
 * report that error instead of replacing it with a pg_savior policy error.
 */
static bool
alter_type_has_core_duplicate_error(Relation rel, AlterTableStmt *stmt)
{
	List	   *changed_columns = NIL;
	ListCell   *lc;

	foreach (lc, stmt->cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lc);
		ColumnDef  *def;
		AttrNumber	attnum;
		Form_pg_attribute attr;
		Oid			targettype;
		int32		targettypmod;

		if (cmd->subtype != AT_AlterColumnType)
			continue;
		if (string_list_contains(changed_columns, cmd->name))
			return true;

		attnum = get_attnum(RelationGetRelid(rel), cmd->name);
		if (attnum == InvalidAttrNumber || attnum <= 0)
			continue;
		def = (ColumnDef *) cmd->def;
		typenameTypeIdAndMod(NULL, def->typeName, &targettype, &targettypmod);
		attr = TupleDescAttr(RelationGetDescr(rel), attnum - 1);
		if (targettype != attr->atttypid || targettypmod != attr->atttypmod)
			changed_columns = lappend(changed_columns, cmd->name);
	}

	return false;
}

/* GetAttrDefaultOid() is not exported on every supported server branch. */
static Oid
pg_savior_attr_default_oid(Oid relid, AttrNumber attnum)
{
	Relation	attrdefRel;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	tuple;
	Oid			result = InvalidOid;

	attrdefRel = table_open(AttrDefaultRelationId, AccessShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_attrdef_adrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[1],
				Anum_pg_attrdef_adnum,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(attnum));
	scan = systable_beginscan(attrdefRel, AttrDefaultIndexId, true,
							 NULL, 2, key);
	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
		result = ((Form_pg_attrdef) GETSTRUCT(tuple))->oid;
	systable_endscan(scan);
	table_close(attrdefRel, AccessShareLock);
	return result;
}

/*
 * Return true when core will reject a dependency before phase 3.  Indexes,
 * constraints, the column's own default, and extended statistics are rebuilt;
 * other direct dependencies (views, functions, triggers, policies, generated
 * columns, publication expressions, and unexpected relation kinds) are fatal.
 */
static bool
alter_type_has_blocking_dependency(Oid relid, AttrNumber attnum)
{
	Relation	depRel;
	SysScanDesc scan;
	ScanKeyData key[3];
	HeapTuple	depTup;
	Oid			own_default = pg_savior_attr_default_oid(relid, attnum);
	bool		blocking = false;

	depRel = table_open(DependRelationId, AccessShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[2],
				Anum_pg_depend_refobjsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum((int32) attnum));
	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							 NULL, 3, key);

	while (HeapTupleIsValid(depTup = systable_getnext(scan)))
	{
		Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(depTup);

		if (dep->classid == RelationRelationId)
		{
			char relkind = get_rel_relkind(dep->objid);

			if (relkind == RELKIND_INDEX
				|| relkind == RELKIND_PARTITIONED_INDEX
				|| relkind == RELKIND_SEQUENCE)
				continue;
		}
		else if (dep->classid == ConstraintRelationId
				 || dep->classid == StatisticExtRelationId)
			continue;
		else if (dep->classid == AttrDefaultRelationId
				 && dep->objid == own_default)
			continue;

		blocking = true;
		break;
	}

	systable_endscan(scan);
	table_close(depRel, AccessShareLock);
	return blocking;
}

/*
 * Detect validations that make core fail before a possible heap rewrite.  In
 * those cases pg_savior stands aside so PostgreSQL keeps its specific error.
 */
static bool
alter_type_core_will_reject(Relation rel, AlterTableCmd *cmd)
{
	AttrNumber	attnum;
	Form_pg_attribute attr;
	ColumnDef  *def = (ColumnDef *) cmd->def;
	Oid			targettype;
	int32		targettypmod;
	Node	   *defaultexpr;

	attnum = get_attnum(RelationGetRelid(rel), cmd->name);
	if (attnum == InvalidAttrNumber || attnum <= 0)
		return false;
	attr = TupleDescAttr(RelationGetDescr(rel), attnum - 1);
	typenameTypeIdAndMod(NULL, def->typeName, &targettype, &targettypmod);

	if (attr->attidentity != '\0'
		&& targettype != INT2OID
		&& targettype != INT4OID
		&& targettype != INT8OID)
		return true;

	if (attr->atthasdef)
	{
		defaultexpr = build_column_default(rel, attnum);
		if (defaultexpr != NULL)
		{
			defaultexpr = strip_implicit_coercions(defaultexpr);
			defaultexpr = coerce_to_target_type(NULL,
										 defaultexpr, exprType(defaultexpr),
										 targettype, targettypmod,
										 COERCION_ASSIGNMENT,
										 COERCE_IMPLICIT_CAST, -1);
			if (defaultexpr == NULL)
				return true;
		}
	}

	return alter_type_has_blocking_dependency(RelationGetRelid(rel), attnum);
}

static void
pg_savior_check_alter_table(AlterTableStmt *stmt, const char *queryString)
{
	Relation	rel;
	Oid			relid;
	LOCKMODE	lockmode;
	LOCKMODE	close_lockmode;
	double		reltuples;
	List	   *added_columns = NIL;
	List	   *descendants = NIL;
	ListCell   *lc;
	ListCell   *cmd_lc;
	bool		has_alter_type = false;
	double		large_physical_rows = -1;

	foreach (lc, stmt->cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lc);

		if (cmd->subtype == AT_AlterColumnType)
			has_alter_type = true;
	}

	/* ALTER TYPE needs core's lock level; preserve lighter existing guards. */
	if (has_alter_type)
	{
		lockmode = AlterTableGetLockLevel(stmt->cmds);
		relid = AlterTableLookupRelation(stmt, lockmode);
		if (!OidIsValid(relid))
			return;
		rel = relation_open(relid, NoLock);
		close_lockmode = NoLock;
	}
	else
	{
		lockmode = AccessShareLock;
		rel = relation_openrv_extended(stmt->relation, lockmode, true);
		if (rel == NULL)
			return;
		relid = RelationGetRelid(rel);
		close_lockmode = lockmode;
	}
	reltuples = rel->rd_rel->reltuples;

	/* Let core report statements that cannot reach a heap rewrite. */
	if (has_alter_type
		&& ((!stmt->relation->inh
			 && find_inheritance_children(relid, NoLock) != NIL)
			|| alter_type_has_core_duplicate_error(rel, stmt)))
	{
		relation_close(rel, close_lockmode);
		return;
	}

	/* Core rejects USING on relation kinds for which it cannot transform rows. */
	if (has_alter_type
		&& rel->rd_rel->relkind != RELKIND_RELATION
		&& rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
	{
		foreach (lc, stmt->cmds)
		{
			AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lc);
			ColumnDef  *def = cmd->subtype == AT_AlterColumnType
				? (ColumnDef *) cmd->def : NULL;

			if (def != NULL
				&& (def->raw_default != NULL || def->cooked_default != NULL))
			{
				relation_close(rel, close_lockmode);
				return;
			}
		}
	}

	/*
	 * ALTER TYPE recurses through inheritance and partitions.  Inspect every
	 * physical target so a storage-less or small parent cannot conceal a large
	 * child.  Child locks are released immediately: core locks identity
	 * sequences before descendants, so retaining child locks would invert its
	 * lock order.  The root's AccessExclusiveLock stabilizes membership.
	 */
	if (has_alter_type)
	{
		if (stmt->relation->inh)
			descendants = find_all_inheritors(relid, NoLock, NULL);
		else
			descendants = list_make1_oid(relid);

		foreach (lc, descendants)
		{
			Oid targetid = lfirst_oid(lc);
			Relation target;
			bool close_target;

			if (targetid == relid)
			{
				target = rel;
				close_target = false;
			}
			else
			{
				target = relation_open(targetid, AccessShareLock);
				close_target = true;
			}

			foreach (cmd_lc, stmt->cmds)
			{
				AlterTableCmd *cmd = (AlterTableCmd *) lfirst(cmd_lc);

				if (cmd->subtype == AT_AlterColumnType
					&& alter_type_core_will_reject(target, cmd))
				{
					if (close_target)
						relation_close(target, AccessShareLock);
					relation_close(rel, close_lockmode);
					return;
				}
			}

			if (target->rd_rel->relkind == RELKIND_RELATION
				&& target->rd_rel->reltuples >= 0
				&& target->rd_rel->reltuples > pg_savior_large_table_threshold_rows)
				large_physical_rows = target->rd_rel->reltuples;

			if (close_target)
				relation_close(target, AccessShareLock);
		}

		if (large_physical_rows >= 0)
		{
			foreach (lc, stmt->cmds)
			{
				AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lc);

				if (cmd->subtype == AT_AlterColumnType
					&& pg_savior_alter_type_requires_rewrite(rel, stmt, cmd,
													queryString))
					ereport(ERROR,
							(errcode(ERRCODE_RAISE_EXCEPTION),
							 errmsg("pg_savior: ALTER TABLE ALTER COLUMN TYPE on a large table (%.0f rows) is blocked",
									large_physical_rows),
							 errhint("This operation can rewrite or scan the whole table while holding a strong lock. "
									 "Plan a safer migration; raise pg_savior.large_table_threshold_rows; "
									 "or set pg_savior.bypass = on. Run ANALYZE if the row estimate looks wrong.")));
			}
		}
	}

	/* Existing non-ALTER-TYPE guards continue to use the root's estimate. */
	if (reltuples < 0
		|| reltuples <= pg_savior_large_table_threshold_rows)
	{
		relation_close(rel, close_lockmode);
		return;
	}

	foreach (lc, stmt->cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lc);
		ColumnDef  *col = cmd->subtype == AT_AddColumn
			? (ColumnDef *) cmd->def : NULL;
		const char *op;

		if (col != NULL && cmd->missing_ok
			&& !alter_statement_drops_column(stmt, col->colname)
			&& (get_attnum(relid, col->colname) != InvalidAttrNumber
				|| string_list_contains(added_columns, col->colname)))
			continue;

		op = pg_savior_alter_table_op(cmd, queryString);
		if (col != NULL)
			added_columns = lappend(added_columns, col->colname);
		if (op == NULL)
			continue;

		ereport(ERROR,
				(errcode(ERRCODE_RAISE_EXCEPTION),
				 errmsg("pg_savior: ALTER TABLE %s on a large table (%.0f rows) is blocked",
						op, reltuples),
				 errhint("This operation can rewrite or scan the whole table while holding a strong lock. "
						 "Plan a safer migration; raise pg_savior.large_table_threshold_rows; "
						 "or set pg_savior.bypass = on. Run ANALYZE if the row estimate looks wrong.")));
	}

	relation_close(rel, close_lockmode);
}

static void
pg_savior_check_truncate(TruncateStmt *stmt)
{
	ListCell *lc;

	foreach (lc, stmt->relations)
	{
		RangeVar   *rv = (RangeVar *) lfirst(lc);
		double		reltuples = pg_savior_relation_tuples(rv);

		if (reltuples < 0)
			continue;

		if (reltuples <= pg_savior_large_table_threshold_rows)
			continue;

		ereport(ERROR,
				(errcode(ERRCODE_RAISE_EXCEPTION),
				 errmsg("pg_savior: TRUNCATE on a large table \"%s\" (%.0f rows) is blocked",
						rv->relname, reltuples),
				 errhint("Verify the target, raise pg_savior.large_table_threshold_rows, "
						 "or set pg_savior.bypass = on. Run ANALYZE if the row estimate looks wrong.")));
	}
}

static void
pg_savior_check_drop(DropStmt *stmt)
{
	ListCell *lc;

	/* Only guard DROP TABLE; other relkinds (view, sequence, ...) pass through */
	if (stmt->removeType != OBJECT_TABLE)
		return;

	foreach (lc, stmt->objects)
	{
		List	   *name_list = (List *) lfirst(lc);
		RangeVar   *rv = makeRangeVarFromNameList(name_list);
		double		reltuples = pg_savior_relation_tuples(rv);

		/* Relation does not exist (or no privilege): let standard handler decide */
		if (reltuples < 0)
			continue;

		if (reltuples <= pg_savior_large_table_threshold_rows)
			continue;

		ereport(ERROR,
				(errcode(ERRCODE_RAISE_EXCEPTION),
				 errmsg("pg_savior: DROP TABLE on a large table \"%s\" (%.0f rows) is blocked",
						rv->relname, reltuples),
				 errhint("Verify the target, raise pg_savior.large_table_threshold_rows, "
						 "or set pg_savior.bypass = on. Run ANALYZE if the row estimate looks wrong.")));
	}
}

static void
pg_savior_check_drop_database(DropdbStmt *stmt)
{
	ereport(ERROR,
			(errcode(ERRCODE_RAISE_EXCEPTION),
			 errmsg("pg_savior: DROP DATABASE \"%s\" is blocked", stmt->dbname),
			 errhint("Set pg_savior.bypass = on for this session if you really mean it.")));
}

static void
pg_savior_ProcessUtility(PlannedStmt *pstmt,
						 const char *queryString,
						 bool readOnlyTree,
						 ProcessUtilityContext context,
						 ParamListInfo params,
						 QueryEnvironment *queryEnv,
						 DestReceiver *dest,
						 QueryCompletion *qc)
{
	Node *parsetree = pstmt->utilityStmt;
	bool index_parent = IsA(parsetree, CreateStmt)
		|| IsA(parsetree, AlterTableStmt);

	if (pg_savior_enabled && !pg_savior_bypass)
	{
		/*
		 * CREATE TABLE constraints and LIKE INCLUDING INDEXES generate IndexStmt
		 * subcommands. ALTER TABLE can do the same after its size check passes.
		 * Track those outer statements so explicit CREATE INDEX nested in CREATE
		 * SCHEMA cannot masquerade as an internal generated index.
		 */
		if (IsA(parsetree, IndexStmt))
		{
			IndexStmt *stmt = (IndexStmt *) parsetree;
			bool generated = pg_savior_index_parent_depth > 0
				&& context == PROCESS_UTILITY_SUBCOMMAND
				&& (stmt->isconstraint || stmt->transformed);

			if (!generated)
				pg_savior_check_create_index(stmt);
		}
		else if (IsA(parsetree, AlterTableStmt))
			pg_savior_check_alter_table((AlterTableStmt *) parsetree,
									 queryString);
		else if (IsA(parsetree, DropStmt))
			pg_savior_check_drop((DropStmt *) parsetree);
		else if (IsA(parsetree, DropdbStmt))
			pg_savior_check_drop_database((DropdbStmt *) parsetree);
		else if (IsA(parsetree, TruncateStmt))
			pg_savior_check_truncate((TruncateStmt *) parsetree);
	}

	if (index_parent)
		pg_savior_index_parent_depth++;

	PG_TRY();
	{
		if (prev_ProcessUtility_hook)
			prev_ProcessUtility_hook(pstmt, queryString, readOnlyTree, context,
									 params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);
	}
	PG_FINALLY();
	{
		if (index_parent)
			pg_savior_index_parent_depth--;
	}
	PG_END_TRY();
}

void
_PG_init(void)
{
	DefineCustomBoolVariable("pg_savior.enabled",
							 "Enable pg_savior protection.",
							 NULL,
							 &pg_savior_enabled,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("pg_savior.bypass",
							 "Bypass pg_savior protection for this session.",
							 NULL,
							 &pg_savior_bypass,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("pg_savior.max_rows_affected",
							"Block DELETE/UPDATE whose planner row estimate exceeds this. 0 disables the check.",
							NULL,
							&pg_savior_max_rows_affected,
							0,
							0,
							INT_MAX,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	DefineCustomIntVariable("pg_savior.large_table_threshold_rows",
							"Tables with more rows than this are considered \"large\" for the DDL guards.",
							NULL,
							&pg_savior_large_table_threshold_rows,
							1000000,
							0,
							INT_MAX,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = pg_savior_post_parse_analyze;

	prev_ExecutorStart_hook = ExecutorStart_hook;
	ExecutorStart_hook = pg_savior_ExecutorStart;

	prev_ProcessUtility_hook = ProcessUtility_hook;
	ProcessUtility_hook = pg_savior_ProcessUtility;
}
