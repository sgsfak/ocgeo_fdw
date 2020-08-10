/*-------------------------------------------------------------------------
 *
 * ocgeo_fdw.c
 *      PostgreSQL-related functions for OpenCageData foreign data wrapper.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"


#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string.h>
#include <stdlib.h>

#include <curl/curl.h>

#include "fmgr.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_user_mapping.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "mb/pg_wchar.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"

#if PG_VERSION_NUM < 120000
#include "nodes/relation.h"
#include "optimizer/var.h"
#endif
#if PG_VERSION_NUM >= 120000
#include "optimizer/optimizer.h"
#endif
// #include "nodes/pathnodes.h"
#include "nodes/parsenodes.h"
// #include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "storage/fd.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/jsonb.h"
#if PG_VERSION_NUM < 130000
#include "utils/jsonapi.h"
#else
#include "utils/jsonfuncs.h"
#endif

#include "ocgeo_api.h"
#include "cJSON.h"

PG_MODULE_MAGIC;


/*
 * Describes the valid options for objects that use this wrapper.
 */
struct OCGeoFdwOption
{
    const char *optname;
    Oid optcontext;     /* Oid of catalog in which option may appear */
    bool optrequired;
};

#define DEFAULT_MAX_LONG 32767
#define DEFAULT_PREFETCH 200


/*
 * Valid options for ocgeo_fdw.
 */
static struct OCGeoFdwOption valid_options[] = {
    {"uri",          ForeignServerRelationId, true},
    {"api_key",      UserMappingRelationId,   true},
    {"max_reqs_sec", UserMappingRelationId,   false},
    {"max_reqs_day", UserMappingRelationId,   false}
};

#define option_count (sizeof(valid_options)/sizeof(struct OCGeoFdwOption))


typedef struct ocgeo_fdw_options
{
    char*   uri;
    char*   api_key;
    int     max_reqs_sec;
    int     max_reqs_day;
} ocgeo_fdw_options;

extern PGDLLEXPORT void _PG_init (void);
extern PGDLLEXPORT void _PG_fini (void);

/*
 * SQL functions
 */
extern PGDLLEXPORT Datum ocgeo_fdw_handler(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum ocgeo_fdw_validator(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum ocgeo_stats(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(ocgeo_fdw_handler);
PG_FUNCTION_INFO_V1(ocgeo_fdw_validator);
PG_FUNCTION_INFO_V1 (ocgeo_stats);

/*
 * FDW callback routines
 */
static void ocgeoGetForeignRelSize (PlannerInfo * root, RelOptInfo * baserel, Oid foreigntableid);
static ForeignScan *ocgeoGetForeignPlan (PlannerInfo * root, RelOptInfo * foreignrel, Oid foreigntableid, ForeignPath * best_path, List * tlist, List * scan_clauses , Plan * outer_plan);
static void ocgeoBeginForeignScan (ForeignScanState * node, int eflags);
static TupleTableSlot *ocgeoIterateForeignScan (ForeignScanState * node);
static void ocgeoEndForeignScan (ForeignScanState * node);
static void ocgeoGetForeignPaths (PlannerInfo * root, RelOptInfo * baserel, Oid foreigntableid);
static void ocgeoReScanForeignScan (ForeignScanState * node);
// static void ocgeoExplainForeignScan (ForeignScanState * node, ExplainState * es);

/*
 * Foreign-data wrapper handler function: return a struct with pointers
 * to callback routines.
 */
PGDLLEXPORT Datum
ocgeo_fdw_handler (PG_FUNCTION_ARGS)
{
    FdwRoutine *fdwroutine = makeNode (FdwRoutine);

    fdwroutine->GetForeignRelSize = ocgeoGetForeignRelSize;
    fdwroutine->BeginForeignScan = ocgeoBeginForeignScan;
    fdwroutine->IterateForeignScan = ocgeoIterateForeignScan;
    fdwroutine->EndForeignScan = ocgeoEndForeignScan;
    fdwroutine->GetForeignPlan = ocgeoGetForeignPlan;


    fdwroutine->GetForeignPaths = ocgeoGetForeignPaths;
    // fdwroutine->ExplainForeignScan = ocgeoExplainForeignScan;
    fdwroutine->ReScanForeignScan = ocgeoReScanForeignScan;

    // fdwroutine->AnalyzeForeignTable = ocgeoAnalyzeForeignTable;
    // fdwroutine->GetForeignJoinPaths = ocgeoGetForeignJoinPaths;
    PG_RETURN_POINTER (fdwroutine);
}




/*
 * ocgeo_fdw_validator
 *      Validate the generic options given to a FOREIGN DATA WRAPPER, SERVER,
 *      USER MAPPING or FOREIGN TABLE that uses ocgeo_fdw.
 *
 *      Raise an ERROR if the option or its value are considered invalid
 *      or a required option is missing.
 */
PGDLLEXPORT Datum
ocgeo_fdw_validator (PG_FUNCTION_ARGS)
{
    List *options_list = untransformRelOptions (PG_GETARG_DATUM (0));
    Oid catalog = PG_GETARG_OID (1);
    ListCell *cell;
    bool option_given[option_count] = { false };
    int i;

  /*
   * Check that only options supported by ocgeo_fdw, and allowed for the
   * current object type, are given.
   */

    foreach (cell, options_list) {
        DefElem *def = (DefElem *) lfirst (cell);
        bool opt_found = false;

    /* search for the option in the list of valid options */
        for (i = 0; i < option_count; ++i) {
            if (catalog == valid_options[i].optcontext && strcmp (valid_options[i].optname, def->defname) == 0) {
                opt_found = true;
                option_given[i] = true;
                break;
            }
        }


    /* option not found, generate error message */
        if (!opt_found) {
      /* generate list of options */
            StringInfoData buf;
            initStringInfo (&buf);
            for (i = 0; i < option_count; ++i) {
                if (catalog == valid_options[i].optcontext)
                    appendStringInfo (&buf, "%s%s", (buf.len > 0) ? ", " : "", valid_options[i].optname);
            }

            ereport (ERROR, (errcode (ERRCODE_FDW_INVALID_OPTION_NAME), errmsg ("invalid option \"%s\"", def->defname), errhint ("Valid options in this context are: %s", buf.data)));
        }

        /* check valid values for "max_reqs_sec" or "max_reqs_day", they should be numbers */
        if (strcmp (def->defname, "max_reqs_sec") == 0 || strcmp (def->defname, "max_reqs_day") == 0) {
            char *val = ((Value *) (def->arg))->val.str;
            char *endptr;
            unsigned long numVal = strtol (val, &endptr, 0);
            if (val[0] == '\0' || *endptr != '\0' || numVal < 0)
                ereport (ERROR,
                    (errcode (ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
                        errmsg ("invalid value for option \"%s\"", def->defname), 
                        errhint ("Valid values in this context are positive integers.")));
        }
    }

  /* check that all required options have been given */
    for (i = 0; i < option_count; ++i) {
        if (catalog == valid_options[i].optcontext && valid_options[i].optrequired && !option_given[i]) {
            ereport (ERROR, (errcode (ERRCODE_FDW_OPTION_NAME_NOT_FOUND), errmsg ("missing required option \"%s\"", valid_options[i].optname)));
        }
    }

    PG_RETURN_VOID ();
}


static void
exitHook (int code, Datum arg)
{
  /* Cleanup? */
}

/*
 * _PG_init
 *      Library load-time initalization.
 *      Sets exitHook() callback for backend shutdown.
 */
void _PG_init (void)
{
    elog(DEBUG1,"function %s, before curl global init",__func__);

    // register an exit hook 
    // on_proc_exit (&exitHook, PointerGetDatum (NULL));

    curl_global_init(CURL_GLOBAL_ALL);
    elog(DEBUG1,"function %s, after curl global init",__func__);
}

void _PG_fini(void)
{
    curl_global_cleanup();
}


/*
 * Hashtable key that defines the identity of a hashtable entry.  We only
 * keep the user oid
 */
typedef Oid ocgeoHashKey;

// See https://github.com/postgres/postgres/blob/REL_12_3/contrib/pg_stat_statements/pg_stat_statements.c
/*
 * The actual stats counters kept within pgssEntry.
 */
typedef struct Counters
{
    int64       calls;          /* # of times executed */
    double      total_time;     /* total execution time, in msec */
    double      min_time;       /* minimum execution time in msec */
    double      max_time;       /* maximum execution time in msec */
    double      mean_time;      /* mean execution time in msec */
    double      sum_var_time;   /* sum of variances in execution time in msec */
    int64       rows;           /* total # of retrieved or affected rows */
} Counters;

/*
 * Statistics per user
 *
 */
typedef struct ocgeoHashEntry
{
    ocgeoHashKey key;           /* hash key of entry - MUST BE FIRST */
    Counters    counters;       /* the statistics for this query */
    slock_t     mutex;          /* protects the counters */
} ocgeoHashEntry;


typedef struct ocgeoTableOptions
{
    Oid server_id;
    Oid user_id;
    char* uri;
    char* api_key;
    int max_reqs_sec;
    int max_reqs_day;

} ocgeoTableOptions;

/*
 * the hash table keeping the Counters per user Oid
 */
static HTAB *ocgeo_hash = NULL;

/*
 * Helper functions
 */
static void ocgeoGetOptions(Oid foreigntableid, ocgeoTableOptions *options);
static void ocgeoGetQual(Node *node, TupleDesc tupdesc, char **key,
    char **value, bool *pushdown);
static Counters * GetCounters(ocgeoTableOptions *table_options);

static void
ocgeoGetOptions(Oid foreigntableid, ocgeoTableOptions *table_options)
{
    ForeignTable *table;
    ForeignServer *server;
    UserMapping *mapping;
    List       *options;
    ListCell   *lc;

    table = GetForeignTable(foreigntableid);
    server = GetForeignServer(table->serverid);
    mapping = GetUserMapping(GetUserId(), table->serverid);

    table_options->user_id = mapping->userid;
    table_options->server_id = server->serverid;

    options = NIL;
    options = list_concat(options, table->options);
    options = list_concat(options, server->options);
    options = list_concat(options, mapping->options);

    /* Loop through the options, and get the uri, apikey, etc. */
    foreach(lc, options)
    {
        DefElem    *def = (DefElem *) lfirst(lc);

        if (strcmp(def->defname, "uri") == 0)
            table_options->uri = defGetString(def);

        if (strcmp(def->defname, "api_key") == 0)
            table_options->api_key = defGetString(def);

        if (strcmp(def->defname, "max_reqs_sec") == 0)
            table_options->max_reqs_sec = atoi(defGetString(def));

        if (strcmp(def->defname, "max_reqs_day") == 0)
            table_options->max_reqs_day = atoi(defGetString(def));
    }

    /* Default values: Free plan */

    if (!table_options->max_reqs_day)
        table_options->max_reqs_day = 2500;

    if (!table_options->max_reqs_day)
        table_options->max_reqs_day = 1;
}

#define PROCID_TEXTEQ 67
static void
ocgeoGetQual(Node *node, TupleDesc tupdesc, char **key, char **value, bool *pushdown)
{
    *key = NULL;
    *value = NULL;
    *pushdown = false;

    if (!node || !IsA(node, OpExpr))
        return;

    OpExpr     *op = (OpExpr *) node;
    Node       *left,
               *right;
    Index       varattno;

    if (list_length(op->args) != 2)
        return;

    left = list_nth(op->args, 0);
    right = list_nth(op->args, 1);

    if (!((IsA(left, Var) && IsA(right, Const)) || (IsA(left, Const) && IsA(right, Var))))
        return;
    if (IsA(left, Const)) {
        Node* t = left;
        left = right;
        right = t;
    }


    varattno = ((Var *) left)->varattno;

    StringInfoData buf;
    initStringInfo(&buf);

    /* And get the column and value... */
    *key = NameStr(TupleDescAttr(tupdesc, varattno - 1)->attname);
    *value = TextDatumGetCString(((Const *) right)->constvalue);

    /*
     * We can push down this qual if: - The operatory is TEXTEQ - The
     * qual is on the `query` column
     */
    if (op->opfuncid == PROCID_TEXTEQ && strcmp(*key, "query") == 0)
        *pushdown = true;
}


Counters * GetCounters(struct ocgeoTableOptions *table_options) {
    bool                 found;
    ocgeoHashEntry*   entry;
    ocgeoHashKey      key;

    /* First time through, initialize connection cache hashtable */
    if (ocgeo_hash == NULL)
    {
        HASHCTL ctl;

        MemSet(&ctl, 0, sizeof(ctl));
        ctl.keysize = sizeof(ocgeoHashKey);
        ctl.entrysize = sizeof(ocgeoHashEntry);
        ctl.hash = tag_hash;
        /* allocate ocgeo_hash in the cache context */
        ctl.hcxt = CacheMemoryContext;
        ocgeo_hash = hash_create("ocgeo_fdw connections", 8,
            &ctl,
            HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
    }

    /* Create hash key for the entry */
    key = table_options->user_id;

    /*
     * Find or create cached entry for requested connection.
     */
    entry = hash_search(ocgeo_hash, &key, HASH_ENTER, &found);
    if (!found) {
        /* initialize new hashtable entry (key is already filled in) */
        entry->counters = (Counters) {0};
    }


    return &entry->counters;
}

#if 0

static void
ocgeoGetForeignRelSize(PlannerInfo *root,
    RelOptInfo *baserel,
    Oid foreigntableid)
{
      /*
       * Obtain relation size estimates for a foreign table. This is called at
       * the beginning of planning for a query that scans a foreign table. root
       * is the planner's global information about the query; baserel is the
       * planner's information about this table; and foreigntableid is the
       * pg_class OID of the foreign table. (foreigntableid could be obtained
       * from the planner data structures, but it's passed explicitly to save
       * effort.)
       *
       * This function should update baserel->rows to be the expected number of
       * rows returned by the table scan, after accounting for the filtering
       * done by the restriction quals. The initial value of baserel->rows is
       * just a constant default estimate, which should be replaced if at all
       * possible. The function may also choose to update baserel->width if it
       * can compute a better estimate of the average result row width.
       */

    ocgeoTableOptions *fpinfo;
    ListCell   *lc;


    elog(DEBUG1, "entering function %s", __func__);

    /*
     * We use PgFdwRelationInfo to pass various information to subsequent
     * functions.
     */
    fpinfo = (ocgeoFdwPlanState *) palloc0(sizeof(ocgeoFdwPlanState));
    baserel->fdw_private = fpinfo;

    /* Look up foreign-table catalog info. */
    fpinfo->table = GetForeignTable(foreigntableid);
    fpinfo->server = GetForeignServer(fpinfo->table->serverid);

    /*
     * Extract user-settable option values.  Note that per-table setting of
     * use_remote_estimate overrides per-server setting.
     */
    fpinfo->use_remote_estimate = DEFAULT_FDW_USE_REMOTE_ESTIMATE;
    fpinfo->fdw_startup_cost = DEFAULT_FDW_STARTUP_COST;
    fpinfo->fdw_tuple_cost = DEFAULT_FDW_TUPLE_COST;
    fpinfo->shippable_extensions = NIL;

    foreach(lc, fpinfo->server->options)
    {
        DefElem    *def = (DefElem *) lfirst(lc);

        if (strcmp(def->defname, "use_remote_estimate") == 0)
            fpinfo->use_remote_estimate = defGetBoolean(def);
        else if (strcmp(def->defname, "fdw_startup_cost") == 0)
            fpinfo->fdw_startup_cost = strtod(defGetString(def), NULL);
        else if (strcmp(def->defname, "fdw_tuple_cost") == 0)
            fpinfo->fdw_tuple_cost = strtod(defGetString(def), NULL);
    }
    foreach(lc, fpinfo->table->options)
    {
        DefElem    *def = (DefElem *) lfirst(lc);

        if (strcmp(def->defname, "use_remote_estimate") == 0)
            fpinfo->use_remote_estimate = defGetBoolean(def);
    }

    /*
     * Identify which baserestrictinfo clauses can be sent to the remote
     * server and which can't.
     */
    classifyConditions(root, baserel, baserel->baserestrictinfo,
        &fpinfo->remote_conds, &fpinfo->local_conds);

    /*
     * Identify which attributes will need to be retrieved from the remote
     * server.  These include all attrs needed for joins or final output, plus
     * all attrs used in the local_conds.  (Note: if we end up using a
     * parameterized scan, it's possible that some of the join clauses will be
     * sent to the remote and thus we wouldn't really need to retrieve the
     * columns used in them.  Doesn't seem worth detecting that case though.)
     */
    fpinfo->attrs_used = NULL;
    pull_varattnos((Node *) baserel->reltargetlist, baserel->relid,
        &fpinfo->attrs_used);
    foreach(lc, fpinfo->local_conds)
    {
        RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

        pull_varattnos((Node *) rinfo->clause, baserel->relid,
            &fpinfo->attrs_used);
    }

    /*
     * Compute the selectivity and cost of the local_conds, so we don't have
     * to do it over again for each path.  The best we can do for these
     * conditions is to estimate selectivity on the basis of local statistics.
     */
    fpinfo->local_conds_sel = clauselist_selectivity(root,
        fpinfo->local_conds,
        baserel->relid,
        JOIN_INNER,
        NULL);

    cost_qual_eval(&fpinfo->local_conds_cost, fpinfo->local_conds, root);

        /*
         * If the foreign table has never been ANALYZEd, it will have relpages
         * and reltuples equal to zero, which most likely has nothing to do
         * with reality.  We can't do a whole lot about that if we're not
         * allowed to consult the remote server, but we can use a hack similar
         * to plancat.c's treatment of empty relations: use a minimum size
         * estimate of 10 pages, and divide by the column-datatype-based width
         * estimate to get the corresponding number of tuples.
         */
    if (baserel->pages == 0 && baserel->tuples == 0)
    {
        baserel->pages = 10;
        baserel->tuples =
        (10 * BLCKSZ) / (baserel->width +
            MAXALIGN(SizeofHeapTupleHeader));

    }

        /* Estimate baserel size as best we can with local statistics. */
    set_baserel_size_estimates(root, baserel);

        /* Fill in basically-bogus cost estimates for use later. */
    estimate_path_cost_size(root, baserel, NIL, NIL,
        &fpinfo->rows, &fpinfo->width,
        &fpinfo->startup_cost,
        &fpinfo->total_cost);

}
#endif


typedef struct ocgeoFdwPlanState {
    ForeignTable* table;
    ForeignServer* server;
    Oid user_id;

} ocgeoFdwPlanState;


/*
 * ColumnList
 *		Takes in the planner's information about this foreign table.  The
 *		function then finds all columns needed for query execution, including
 *		those used in projections, joins, and filter clauses, de-duplicates
 *		these columns, and returns them in a new list.
 */
List *
ColumnList(RelOptInfo *baserel)
{
	List	   *columnList = NIL;
	List	   *neededColumnList;
	AttrNumber	columnIndex;
	AttrNumber	columnCount = baserel->max_attr;

#if PG_VERSION_NUM >= 90600
	List	   *targetColumnList = baserel->reltarget->exprs;
#else
	List	   *targetColumnList = baserel->reltargetlist;
#endif
	List	   *restrictInfoList = baserel->baserestrictinfo;
	ListCell   *restrictInfoCell;

	/* First add the columns used in joins and projections */
	neededColumnList = list_copy(targetColumnList);

	/* Then walk over all restriction clauses, and pull up any used columns */
	foreach(restrictInfoCell, restrictInfoList)
	{
		RestrictInfo *restrictInfo = (RestrictInfo *) lfirst(restrictInfoCell);
		Node	   *restrictClause = (Node *) restrictInfo->clause;
		List	   *clauseColumnList = NIL;

		/* Recursively pull up any columns used in the restriction clause */
		clauseColumnList = pull_var_clause(restrictClause,
#if PG_VERSION_NUM < 90600
										   PVC_RECURSE_AGGREGATES,
#endif
										   PVC_RECURSE_PLACEHOLDERS);

		neededColumnList = list_union(neededColumnList, clauseColumnList);
	}

	/* Walk over all column definitions, and de-duplicate column list */
	for (columnIndex = 1; columnIndex <= columnCount; columnIndex++)
	{
		ListCell   *neededColumnCell;
		Var		   *column = NULL;

		/* Look for this column in the needed column list */
		foreach(neededColumnCell, neededColumnList)
		{
			Var		   *neededColumn = (Var *) lfirst(neededColumnCell);

			if (neededColumn->varattno == columnIndex)
			{
				column = neededColumn;
				break;
			}
		}

		if (column != NULL)
			columnList = lappend(columnList, column);
	}

	return columnList;
}

static void
ocgeoGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{

    elog(DEBUG1,"entering function %s",__func__);

    ocgeoFdwPlanState *fplanstate;
    ocgeoTableOptions table_options;

    fplanstate = palloc0(sizeof(*fplanstate));
    baserel->fdw_private = fplanstate;

    ocgeoGetOptions(foreigntableid, &table_options);

    /* Look up foreign-table catalog info. */
    fplanstate->table = GetForeignTable(foreigntableid);
    fplanstate->server = GetForeignServer(fplanstate->table->serverid);
    fplanstate->user_id = baserel->userid;
    baserel->tuples = 1e10; /* number of tuples in relation (not considering restrictions) */
    baserel->rows = 10; /* estimated number of tuples in the relation after restriction
                           clauses have been applied (ie, output rows of a plan for it) */
}

static void
ocgeoGetForeignPaths(PlannerInfo *root,
                     RelOptInfo *baserel,
                     Oid foreigntableid)
{
    elog(DEBUG1,"entering function %s",__func__);
    // ocgeoFdwPlanState *fdw_private = baserel->fdw_private;
    Cost startup_cost, total_cost;

    startup_cost = 25;
    total_cost = startup_cost + baserel->rows;

    /* Create a ForeignPath node and add it as only possible path */
    add_path(baserel, (Path *)
             create_foreignscan_path(root, baserel,
                                     NULL,      /* default pathtarget */
                                     baserel->rows,
                                     startup_cost,
                                     total_cost,
                                     NIL,       /* no pathkeys */
                                     NULL,      /* no outer rel either */
                                     NULL,        /* no extra plan */
                                     NIL));     /* no fdw_private data */
}

static ForeignScan *
ocgeoGetForeignPlan(PlannerInfo *root,
                    RelOptInfo *baserel,
                    Oid foreigntableid,
                    ForeignPath *best_path,
                    List *tlist,
                    List *scan_clauses,
                    Plan *outer_plan)
{
    Index       scan_relid = baserel->relid;
    
    elog(DEBUG1,"entering function %s",__func__);

    List* colList = ColumnList(baserel);

    /*
     * We have no native ability to evaluate restriction clauses, so we just
     * put all the scan_clauses into the plan node's qual list for the
     * executor to check.  So all we have to do here is strip RestrictInfo
     * nodes from the clauses and ignore pseudoconstants (which will be
     * handled elsewhere).
     */
    scan_clauses = extract_actual_clauses(scan_clauses, false);

    /* Create the ForeignScan node */
    return make_foreignscan(tlist,
                            scan_clauses,
                            scan_relid,
                            NIL,    /* no expressions to evaluate */
                            colList, /* private state: the column list */
                            NIL,    /* no custom tlist */
                            NIL,    /* no remote quals */
                            outer_plan);
}

/*
 * ColumnMapping reprents a hash table entry that maps a column name to column
 * related information.  We construct these hash table entries to speed up the
 * conversion from JSON documents to PostgreSQL tuples; and each hash entry
 * maps the column name to the column's tuple index and its type-related
 * information.
 */
typedef struct ColumnMapping
{
	char		columnName[NAMEDATALEN];
	uint32		columnIndex;
	Oid			columnTypeId;
	int32		columnTypeMod;
	Oid			columnArrayTypeId;
} ColumnMapping;

/*
 * ColumnMappingHash
 *		Creates a hash table that maps column names to column index and types.
 *
 * This table helps us quickly translate OpenCageData JSON data fields to the
 * corresponding PostgreSQL columns.
 */
static HTAB *
ColumnMappingHash(Oid foreignTableId, List *columnList)
{
	ListCell   *columnCell;
	const long	hashTableSize = 2048;
	HTAB	   *columnMappingHash;

	/* Create hash table */
	HASHCTL		hashInfo;

	memset(&hashInfo, 0, sizeof(hashInfo));
	hashInfo.keysize = NAMEDATALEN;
	hashInfo.entrysize = sizeof(ColumnMapping);
	hashInfo.hash = string_hash;
	hashInfo.hcxt = CurrentMemoryContext;

	columnMappingHash = hash_create("Column Mapping Hash", hashTableSize,
									&hashInfo,
									(HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT));
	Assert(columnMappingHash != NULL);

	foreach(columnCell, columnList)
	{
		Var		   *column = (Var *) lfirst(columnCell);
		AttrNumber	columnId = column->varattno;
		ColumnMapping *columnMapping;
		char	   *columnName = NULL;
		bool		handleFound = false;

#if PG_VERSION_NUM < 110000
		columnName = get_relid_attribute_name(foreignTableId, columnId);
#else
		columnName = get_attname(foreignTableId, columnId, false);
#endif

		columnMapping = (ColumnMapping *) hash_search(columnMappingHash,
													  columnName,
													  HASH_ENTER,
													  &handleFound);
		Assert(columnMapping != NULL);

		columnMapping->columnIndex = columnId - 1;
		columnMapping->columnTypeId = column->vartype;
		columnMapping->columnTypeMod = column->vartypmod;
		columnMapping->columnArrayTypeId = get_element_type(column->vartype);
	}

	return columnMappingHash;
}


typedef struct ocgeoForeignScanState {
    AttInMetadata *attinmeta;
    char  *qual_key;
    char  *qual_value;
    List* columnList;
    HTAB* columnMappingHash;
    MemoryContext mctxt;

    struct ocgeo_api* api;
    ocgeo_response_t response;
    ocgeo_result_t* cursor;

} ocgeoForeignScanState;

/*
 * ocgeoBeginForeignScan :     Initiate access to the API
 */
static void
ocgeoBeginForeignScan(ForeignScanState *node, int eflags)
{
    ocgeoTableOptions table_options;
    char       *qual_key = NULL;
    char       *qual_value = NULL;
    bool        pushdown = false;
    ocgeoForeignScanState *sstate;
    Oid foreignTableId = RelationGetRelid(node->ss.ss_currentRelation);

    elog(DEBUG1,"entering function %s",__func__);


    /* Fetch options  */
    ocgeoGetOptions(foreignTableId, &table_options);

    /* See if we've got a qual we can push down */
    if (node->ss.ps.plan->qual)
    {
        ListCell   *lc;

        foreach(lc, node->ss.ps.plan->qual)
        {
            /* Only the first qual can be pushed down */
            Expr  *state = lfirst(lc);

            ocgeoGetQual((Node *) state,
                         node->ss.ss_currentRelation->rd_att,
                         &qual_key, &qual_value, &pushdown);
            if (pushdown)
                break;
        }
    }

    List* columnList = (List*) ((ForeignScan*) node->ss.ps.plan)->fdw_private;

	HTAB* columnMappingHash = ColumnMappingHash(foreignTableId, columnList);

    /* Stash away the state info we have already */
    sstate = palloc0(sizeof(*sstate));
    node->fdw_state = sstate;

    /* Store the additional state info */
    sstate->attinmeta =
        TupleDescGetAttInMetadata(node->ss.ss_currentRelation->rd_att);
    elog(DEBUG1,"function %s, before curl init",__func__);
    sstate->api = NULL;
    // sstate->curl = curl_easy_init(); // XXX: check for errors?
    elog(DEBUG1,"function %s, after curl init",__func__);
    sstate->qual_key = qual_key;
    sstate->qual_value = pushdown ? qual_value : NULL;

    sstate->columnList = columnList;
    sstate->columnMappingHash = columnMappingHash;

    /* OK, we connected. If this is an EXPLAIN, bail out now */
    if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
        return;

    /*
     * We're going to use the current scan-lived context to
     * store the pstrduped cusrsor id.
     */
    sstate->mctxt = CurrentMemoryContext;

    elog(DEBUG1,"function %s, before ocgeo init: api: %s , server : %s",__func__,table_options.api_key, table_options.uri);
    sstate->api = ocgeo_init(table_options.api_key, table_options.uri);
    elog(DEBUG1,"function %s, after ocgeo init",__func__);
    sstate->cursor = NULL;

    /* Make a forward request, only if some restriction is given (pushed-down) */
    if (sstate->qual_value != NULL) {
        ocgeo_forward(sstate->api, sstate->qual_value, NULL, &sstate->response);
        if (ocgeo_response_ok(&sstate->response)) {
            sstate->cursor = sstate->response.results;
        }
    }
    
    elog(DEBUG1,"In %s API returned status: %d, results: %d",__func__, sstate->response.status.code, sstate->response.total_results);
}


#if PG_VERSION_NUM < 110000
#define		GET_RELID_ATTNAME(foreignTableId, columnId) get_relid_attribute_name(foreignTableId, columnId)
#else
#define		GET_RELID_ATTNAME(foreignTableId, columnId) get_attname(foreignTableId, columnId, false)
#endif

TupleTableSlot* ocgeoIterateForeignScan(ForeignScanState * node)
{
    ocgeoForeignScanState* sstate = node->fdw_state;
    ocgeo_result_t* current_result  = sstate->cursor;

    Oid foreignTableId = node->ss.ss_currentRelation->rd_node.relNode;
    TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
    EState         *estate = node->ss.ps.state;
    MemoryContext   oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

    TupleDesc	tupleDescriptor = slot->tts_tupleDescriptor;
	Datum	   *columnValues = slot->tts_values;
	bool	   *columnNulls = slot->tts_isnull;
	int32		columnCount = tupleDescriptor->natts;


    memset(columnValues, 0, sizeof(Datum) * columnCount);
    memset(columnNulls, true, sizeof(bool) * columnCount);

    elog(DEBUG1,"entering function %s result: %s attr count=%d, %s",__func__, current_result ? current_result->city : "",
            columnCount, columnCount>0 ? tupleDescriptor->attrs[0].attname.data : "");
    
    /*
	 * We execute the protocol to load a virtual tuple into a slot. We first
	 * call ExecClearTuple, then fill in values / isnull arrays, and last call
	 * ExecStoreVirtualTuple.  If we are done iterating over the API results,
	 * we just return an empty slot as required.
	 */
    ExecClearTuple(slot);

    /* no results or results finished */
    if (current_result == NULL) {
        return slot;
    }
    sstate->cursor = current_result->next; /* move the cursor to the next result for next iteration */
    
    /*
    make_tuple_from_result_row(current_result,
                               tupleDescriptor, festate->retrieved_attrs,
                               slot->tts_values, slot->tts_isnull);
    */
    ListCell* col;
    foreach(col, sstate->columnList) {

		Var		   *column = (Var *) lfirst(col);
		AttrNumber	columnId = column->varattno;
		ColumnMapping *columnMapping;
		Datum		columnValue;
		char	   *columnName = NULL;
		bool		handleFound = false;

		columnName = GET_RELID_ATTNAME(foreignTableId, columnId);
		columnMapping = (ColumnMapping *) hash_search(sstate->columnMappingHash,
													  columnName,
													  HASH_ENTER,
													  &handleFound);
		Assert(columnMapping != NULL);

        // elog(DEBUG1,"%s: Column: %s, index=%d, type=%d",__func__, columnName, columnMapping->columnIndex, columnMapping->columnTypeId);

        bool ok = false;
        if (strcmp(sstate->qual_key, columnName) == 0) {
            text* result = cstring_to_text(sstate->qual_value);
            columnValue = PointerGetDatum(result);
            ok = true;
        }
        else {
            StringInfoData field;
            initStringInfo(&field);
            if (strcmp(columnName, "confidence") == 0 || strcmp(columnName, "formatted") == 0) 
                appendStringInfoString(&field, columnName);
            else
                /* By default we look into the components field for fields 
                 * matching the goven columnName 
                 */
                appendStringInfo(&field, "components.%s", columnName);

            switch(columnMapping->columnTypeId) {
            case TEXTOID:
            case VARCHAROID: {
                const char* v = ocgeo_response_get_str(current_result, field.data, &ok);
                if (ok) {
                    text* result = cstring_to_text(v);
                    columnValue = PointerGetDatum(result);
                }
                break;
            }
            case INT4OID:
            case INT8OID: {
                int v = ocgeo_response_get_int(current_result, field.data, &ok);
                if (ok)
                    columnValue = Int32GetDatum(v);
                break;
            }
            /* Geometric info */
            case POINTOID: {
                double lat = ocgeo_response_get_dbl(current_result, "geometry.lat", &ok);
                if (!ok) break;
                double lon = ocgeo_response_get_dbl(current_result, "geometry.lng", &ok);
                if (!ok) break;

                StringInfoData fieldVal;
                initStringInfo(&fieldVal);
                appendStringInfo(&fieldVal, "(%.6lf,%.6lf)", lat, lon);
                columnValue = DirectFunctionCall1(point_in,
                                                  PointerGetDatum(fieldVal.data));
                pfree(fieldVal.data);
                ok = true;
                break;
            }
            case BOXOID: {
                double lat1 = ocgeo_response_get_dbl(current_result, "bounds.northeast.lat", &ok);
                if (!ok) break;
                double lon1 = ocgeo_response_get_dbl(current_result, "bounds.northeast.lng", &ok);
                if (!ok) break;
                double lat2 = ocgeo_response_get_dbl(current_result, "bounds.southwest.lat", &ok);
                if (!ok) break;
                double lon2 = ocgeo_response_get_dbl(current_result, "bounds.southwest.lng", &ok);
                if (!ok) break;

                StringInfoData fieldVal;
                initStringInfo(&fieldVal);
                appendStringInfo(&fieldVal, "((%.6lf,%.6lf),(%.6lf,%.6lf))", lat1, lon1, lat2, lon2);
                columnValue = DirectFunctionCall1(box_in, PointerGetDatum(fieldVal.data));
                pfree(fieldVal.data);
                ok = true;
                break;
            }
            /* JSONB data type... */
            case JSONBOID: {
                char* js = cJSON_PrintUnformatted(current_result->internal);
                columnValue = DirectFunctionCall1(jsonb_in, PointerGetDatum(js));
                free(js);
                ok = true;
                break;
            }
            }
            pfree(field.data);
        }
        if (ok) {
            slot->tts_values[columnMapping->columnIndex] = columnValue;
            slot->tts_isnull[columnMapping->columnIndex] = false;
        }

    }
    ExecStoreVirtualTuple(slot);
    elog(DEBUG1,"exiting function %s slot: %s",__func__, slot ? "OK" : "NULL");
    return slot;
}


static void
ocgeoReScanForeignScan(ForeignScanState *node)
{
    elog(DEBUG1,"entering function %s",__func__);
}


static void
ocgeoEndForeignScan(ForeignScanState *node)
{
    elog(DEBUG1,"entering function %s",__func__);
    ocgeoForeignScanState* sstate = node->fdw_state;
    ocgeo_close(sstate->api);
}
