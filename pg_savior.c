// clang-format off
#include <postgres.h>
#include <fmgr.h>
// clang-format on
#include "access/relation.h"
#include "catalog/namespace.h"
#include "executor/executor.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "parser/analyze.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

void _PG_init(void);

static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart_hook = NULL;
static ProcessUtility_hook_type prev_ProcessUtility_hook = NULL;

static bool pg_savior_enabled = true;
static bool pg_savior_bypass = false;
static int  pg_savior_max_rows_affected = 0;
static int  pg_savior_large_table_threshold_rows = 1000000;

static void
pg_savior_post_parse_analyze(ParseState *pstate, Query *query, JumbleState *jstate)
{
	if (prev_post_parse_analyze_hook)
		prev_post_parse_analyze_hook(pstate, query, jstate);

	if (!pg_savior_enabled || pg_savior_bypass)
		return;

	if (query->commandType != CMD_DELETE && query->commandType != CMD_UPDATE)
		return;

	if (query->jointree != NULL && query->jointree->quals != NULL)
		return;

	ereport(ERROR,
			(errcode(ERRCODE_RAISE_EXCEPTION),
			 errmsg("pg_savior: %s without WHERE clause is blocked",
					query->commandType == CMD_DELETE ? "DELETE" : "UPDATE"),
			 errhint("Add a WHERE clause, or set pg_savior.bypass = on for this session.")));
}

static void
pg_savior_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (pg_savior_enabled
		&& !pg_savior_bypass
		&& pg_savior_max_rows_affected > 0
		&& (queryDesc->operation == CMD_DELETE || queryDesc->operation == CMD_UPDATE)
		&& queryDesc->plannedstmt != NULL
		&& queryDesc->plannedstmt->planTree != NULL)
	{
		/*
		 * The top of the plan tree for DELETE/UPDATE is a ModifyTable node,
		 * whose plan_rows is 0 unless RETURNING is used. The estimate of
		 * source rows lives on the child scan/join.
		 */
		Plan   *plan = queryDesc->plannedstmt->planTree;
		Plan   *source_plan = plan->lefttree != NULL ? plan->lefttree : plan;
		double	estimated_rows = source_plan->plan_rows;

		if (estimated_rows > pg_savior_max_rows_affected)
			ereport(ERROR,
					(errcode(ERRCODE_RAISE_EXCEPTION),
					 errmsg("pg_savior: %s estimated to affect %.0f rows, exceeds pg_savior.max_rows_affected (%d)",
							queryDesc->operation == CMD_DELETE ? "DELETE" : "UPDATE",
							estimated_rows,
							pg_savior_max_rows_affected),
					 errhint("Refine the WHERE clause, raise pg_savior.max_rows_affected, or set pg_savior.bypass = on. Run ANALYZE if the estimate looks wrong.")));
	}

	if (prev_ExecutorStart_hook)
		prev_ExecutorStart_hook(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
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

	ereport(ERROR,
			(errcode(ERRCODE_RAISE_EXCEPTION),
			 errmsg("pg_savior: CREATE INDEX without CONCURRENTLY is blocked"),
			 errhint("Use CREATE INDEX CONCURRENTLY (it cannot run in a transaction block), "
					 "or set pg_savior.bypass = on for this session.")));
}

/*
 * Does this ColumnDef include a DEFAULT clause? The parser represents
 * column-level DEFAULTs as Constraint nodes with contype = CONSTR_DEFAULT
 * in col->constraints (not in raw_default, which is reserved for cooked
 * defaults inherited via CREATE TABLE LIKE etc.).
 */
static bool
column_has_default(ColumnDef *col)
{
	ListCell *lc;

	if (col == NULL)
		return false;

	if (col->raw_default != NULL || col->cooked_default != NULL)
		return true;

	foreach (lc, col->constraints)
	{
		Constraint *con = (Constraint *) lfirst(lc);

		if (con->contype == CONSTR_DEFAULT)
			return true;
	}
	return false;
}

static void
pg_savior_check_alter_table(AlterTableStmt *stmt)
{
	ListCell   *lc;
	double		reltuples = -2;	/* -2 = not yet looked up; -1 = lookup failed */

	foreach (lc, stmt->cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lc);
		ColumnDef  *col;

		if (cmd->subtype != AT_AddColumn)
			continue;

		col = (ColumnDef *) cmd->def;
		if (!column_has_default(col))
			continue;

		/* Lazy lookup: only inspect the relation once, only if needed */
		if (reltuples == -2)
			reltuples = pg_savior_relation_tuples(stmt->relation);

		if (reltuples < 0)	/* relation lookup failed; let standard handler error */
			return;

		if (reltuples <= pg_savior_large_table_threshold_rows)
			return;

		ereport(ERROR,
				(errcode(ERRCODE_RAISE_EXCEPTION),
				 errmsg("pg_savior: ALTER TABLE ADD COLUMN with DEFAULT on a large table (%.0f rows) is blocked",
						reltuples),
				 errhint("Adding a column with a volatile default rewrites the whole table. "
						 "Add the column without a default first, then backfill in batches; "
						 "raise pg_savior.large_table_threshold_rows; or set pg_savior.bypass = on. "
						 "Run ANALYZE if the row estimate looks wrong.")));
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

	if (pg_savior_enabled && !pg_savior_bypass)
	{
		if (IsA(parsetree, IndexStmt))
			pg_savior_check_create_index((IndexStmt *) parsetree);
		else if (IsA(parsetree, AlterTableStmt))
			pg_savior_check_alter_table((AlterTableStmt *) parsetree);
		else if (IsA(parsetree, DropStmt))
			pg_savior_check_drop((DropStmt *) parsetree);
		else if (IsA(parsetree, DropdbStmt))
			pg_savior_check_drop_database((DropdbStmt *) parsetree);
	}

	if (prev_ProcessUtility_hook)
		prev_ProcessUtility_hook(pstmt, queryString, readOnlyTree, context,
								 params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
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
