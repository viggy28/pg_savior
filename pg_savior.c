// clang-format off
#include <postgres.h>
#include <fmgr.h>
// clang-format on
#include "access/relation.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "nodes/value.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_node.h"
#include "parser/parse_type.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
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
	if (stmt->concurrent)
		return;

	/* An existing same-schema relation makes IF NOT EXISTS a harmless no-op. */
	if (stmt->if_not_exists && stmt->idxname != NULL)
	{
		Relation rel = relation_openrv_extended(stmt->relation,
										ShareLock, true);

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

		case AT_AlterColumnType:
			return "ALTER COLUMN TYPE";

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

static void
pg_savior_check_alter_table(AlterTableStmt *stmt, const char *queryString)
{
	Relation	rel;
	Oid			relid;
	double		reltuples;
	List	   *added_columns = NIL;
	ListCell   *lc;

	/* Preserve ALTER TABLE IF EXISTS no-op behavior before cooking defaults. */
	rel = relation_openrv_extended(stmt->relation, AccessShareLock, true);
	if (rel == NULL)
		return;

	relid = RelationGetRelid(rel);
	reltuples = rel->rd_rel->reltuples;
	if (reltuples < 0
		|| reltuples <= pg_savior_large_table_threshold_rows)
	{
		relation_close(rel, AccessShareLock);
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

	relation_close(rel, AccessShareLock);
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
