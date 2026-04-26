// clang-format off
#include <postgres.h>
#include <fmgr.h>
// clang-format on
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "parser/analyze.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

void _PG_init(void);

static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;

static bool pg_savior_enabled = true;
static bool pg_savior_bypass = false;

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

	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = pg_savior_post_parse_analyze;
}
