// clang-format off
#include <postgres.h>
#include <fmgr.h>
// clang-format on
#include "executor/executor.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "parser/analyze.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

void _PG_init(void);

static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart_hook = NULL;

static bool pg_savior_enabled = true;
static bool pg_savior_bypass = false;
static int  pg_savior_max_rows_affected = 0;

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

	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = pg_savior_post_parse_analyze;

	prev_ExecutorStart_hook = ExecutorStart_hook;
	ExecutorStart_hook = pg_savior_ExecutorStart;
}
