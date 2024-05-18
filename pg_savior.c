// clang-format off
#include <postgres.h>
#include <fmgr.h>
// clang-format on
#include "executor/spi.h"
#include "executor/tstoreReceiver.h"
#include "funcapi.h"
#include "nodes/nodes.h"
#include "utils/builtins.h"
#include <miscadmin.h>

static ExecutorRun_hook_type ExecutorRun_old_hook = NULL;

// insert the query into the saved_queries table
static void insert_meta(QueryDesc *qd) {
  SPIPlanPtr plan;
  Datum values[1];
  char nulls[1] = {' '};

  values[0] = CStringGetTextDatum(qd->sourceText);

  if (SPI_connect() != SPI_OK_CONNECT)
    elog(ERROR, "SPI_connect failed");

  plan = SPI_prepare("INSERT INTO saved_queries (query) VALUES ($1)", 1,
                     (Oid[1]){TEXTOID});
  if (plan == NULL)
    elog(ERROR, "SPI_prepare failed: %s", SPI_result_code_string(SPI_result));

  SPI_execute_plan(plan, values, nulls, false, 0);
  SPI_finish();
}

bool walkPlanTree(Plan *plan);

static bool ensureWhereClause(QueryDesc *queryDesc) {
  Plan *plan = queryDesc->plannedstmt->planTree;
  if (queryDesc->params != NULL) {
    // non parameterized DELETE statement detected
    // char *s = nodeToString(plan);
    // elog(INFO, "pg_savior: planTree: %s", s);
    return true;
  }

  return walkPlanTree(plan);
}

// walk the plan tree to find if there is a WHERE clause
bool walkPlanTree(Plan *plan) {
  bool hasWhereClause = false;
  if (plan == NULL) {
    return false;
  }
  if (plan->qual != NULL) {
    if (plan->qual->type == T_List) {
      ListCell *cell;
      foreach (cell, plan->qual) {
        Node *node = lfirst(cell);
        // We are checking operation expression
        if (node->type == T_OpExpr) {
          return true;
        }
      }
    }
  }
  // subquery and CTE's creates hash join
  if (plan->type == T_HashJoin) {
    HashJoin *hashJoin = (HashJoin *)plan;
    if (hashJoin->hashclauses != NULL) {
      return true;
    }
  }

  // queries which uses index scan
  if (plan->type == T_IndexScan) {
    IndexScan *indexScan = (IndexScan *)plan;
    if (indexScan->indexqual != NULL) {
      return true;
    }
  }

  if (plan->lefttree != NULL) {
    hasWhereClause = walkPlanTree(plan->lefttree);
    if (hasWhereClause) {
      return true;
    }
  }
  if (plan->righttree != NULL) {
    hasWhereClause = walkPlanTree(plan->righttree);
    if (hasWhereClause) {
      return true;
    }
  }
  return hasWhereClause;
}

static void ExecutorRun_hook_savior(QueryDesc *queryDesc,
                                    ScanDirection direction, uint64 count,
                                    bool execute_once) {
  if (ExecutorRun_old_hook)
    ExecutorRun_old_hook(queryDesc, direction, count, execute_once);
  else
    switch (queryDesc->operation) {
    case CMD_DELETE:
      elog(INFO, "pg_savior: DELETE statement detected");
      if (ensureWhereClause(queryDesc)) {
        standard_ExecutorRun(queryDesc, direction, count, execute_once);
      } else {
        elog(INFO, "pg_savior: WHERE clause is mandatory for a DELETE statement");
        insert_meta(queryDesc);
      }
      break;
    case CMD_UPDATE:
      if (ensureWhereClause(queryDesc)) {
        standard_ExecutorRun(queryDesc, direction, count, execute_once);
      } else {
        elog(INFO, "pg_savior: WHERE clause is mandatory for an UPDATE statement");
        insert_meta(queryDesc);
      }
      break;
    default:
      standard_ExecutorRun(queryDesc, direction, count, execute_once);
    }
}

void _PG_init(void);

void _PG_init() {
  ExecutorRun_old_hook = ExecutorRun_hook;
  ExecutorRun_hook = ExecutorRun_hook_savior;
}

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(show_saved);

// this function is used to show the saved queries
// it's just a wrapper around select * from saved_queries
Datum show_saved(PG_FUNCTION_ARGS) {
  int ret;
  TupleDesc tupdesc;
  Tuplestorestate *tupstore;
  MemoryContext oldcontext;
  ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;

  SPI_connect();

  ret = SPI_execute("SELECT * FROM saved_queries", true, 0);

  if (ret != SPI_OK_SELECT)
    elog(ERROR, "SPI_execute failed: error code %d", ret);

  /* Switch to long-lived context to build tuplestore in */
  oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);

  /* Build a tuple descriptor for our result type */
  if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
    elog(ERROR, "return type must be a row type");

  tupstore = tuplestore_begin_heap(true, false, work_mem);

  /* Put tuples into tuplestore */
  for (int i = 0; i < SPI_processed; i++) {
    HeapTuple tuple;
    tuple = SPI_copytuple(SPI_tuptable->vals[i]);
    tuplestore_puttuple(tupstore, tuple);
  }

  /* Clean up and return the tuplestore */
  SPI_finish();
  tuplestore_donestoring(tupstore);

  MemoryContextSwitchTo(oldcontext);

  rsinfo->returnMode = SFRM_Materialize;
  rsinfo->setResult = tupstore;
  rsinfo->setDesc = tupdesc;

  PG_RETURN_NULL();
}
