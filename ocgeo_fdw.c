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
#include "storage/ipc.h"
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


#define URI_OPTION          "uri"
#define API_KEY_OPTION      "api_key"
#define MAX_REQS_SEC_OPTION "max_reqs_sec"
#define MAX_REQS_DAY_OPTION "max_reqs_day"

#define QUERY_ATT_NAME      "q"
#define MAX_RESULTS_REQUESTED 50
/*
 * Valid options for ocgeo_fdw.
 */
static struct OCGeoFdwOption valid_options[] = {
    {URI_OPTION,          ForeignServerRelationId, true},
    {API_KEY_OPTION,      UserMappingRelationId,   true},
    {MAX_REQS_SEC_OPTION, UserMappingRelationId,   false},
    {MAX_REQS_DAY_OPTION, UserMappingRelationId,   false}
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
static void ocgeoExplainForeignScan (ForeignScanState * node, ExplainState * es);

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
    fdwroutine->ExplainForeignScan = ocgeoExplainForeignScan;
    fdwroutine->ReScanForeignScan = ocgeoReScanForeignScan;

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
        DefElem *def = lfirst_node (DefElem, cell);
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

            ereport(ERROR,
                    (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                     errmsg ("invalid option \"%s\"", def->defname),
                     errhint ("Valid options in this context are: %s", buf.data)));
        }

        /* check valid values for "max_reqs_sec" or "max_reqs_day", they should be numbers */
        if (strcmp (def->defname, MAX_REQS_SEC_OPTION) == 0
                || strcmp (def->defname, MAX_REQS_DAY_OPTION) == 0) {
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
        if (catalog == valid_options[i].optcontext
                && valid_options[i].optrequired && !option_given[i]) {
            ereport(ERROR,
                    (errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
                     errmsg ("missing required option \"%s\"", valid_options[i].optname)));
        }
    }

    PG_RETURN_VOID ();
}


static void
exitHook (int code, Datum arg)
{
    curl_global_cleanup();
}

/*
 * _PG_init
 *      Library load-time initalization.
 *      Sets exitHook() callback for backend shutdown.
 */
void _PG_init (void)
{
    elog(DEBUG2,"function %s, before curl global init",__func__);

    // register an exit hook 
    on_proc_exit (&exitHook, PointerGetDatum (NULL));

    curl_global_init(CURL_GLOBAL_ALL);
    elog(DEBUG2,"function %s, after curl global init",__func__);
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
    int64             calls;         /* # of times executed */
    int64             success_calls; /* # of times executed successfully */
    double            total_time;    /* total execution time, in msec */
    ocgeo_rate_info_t rate_info;     /* rate info, got from the most recent API call */
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

static List * ColumnList(RelOptInfo *baserel);

/*
 * Helper functions
 */
static void ocgeoGetOptions(Oid foreigntableid, ocgeoTableOptions *options);
static Counters * ocgeoGetCounters(ocgeoTableOptions *table_options);

static bool isAttrInRestrictInfo(Index relid, AttrNumber attno, RestrictInfo *restrictinfo);
static List *clausesInvolvingAttr(Index relid, AttrNumber attnum, EquivalenceClass *eq_class);
static List * findPaths(PlannerInfo *root, RelOptInfo *baserel, List *possiblePaths, int startupCost);

extern Value* colnameFromVar(Var *var, PlannerInfo *root);
extern void extractRestrictions(Relids base_relids, Expr *node, List **quals);

static char* colnameFromTupleVar(Var *var, TupleDesc desc);

Datum
ocgeo_stats(PG_FUNCTION_ARGS)
{
    FuncCallContext     *funcctx;
    int                  call_cntr;
    int                  max_calls;
    TupleDesc            tupdesc;
    AttInMetadata       *attinmeta;
    HTAB                *ht = ocgeo_hash;
    HASH_SEQ_STATUS     *hash_seq = NULL;

    /* stuff done only on the first call of the function */
    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext   oldcontext;

        /* create a function context for cross-call persistence */
        funcctx = SRF_FIRSTCALL_INIT();

        /* switch to memory context appropriate for multiple function calls */
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /* total number of tuples to be returned */
        funcctx->max_calls = ht != NULL ? hash_get_num_entries(ht) : 0;

        /* Build a tuple descriptor for our result type */
        if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("function returning record called in context "
                            "that cannot accept type record")));

        /*
         * generate attribute metadata needed later to produce tuples from raw
         * C strings
         */
        attinmeta = TupleDescGetAttInMetadata(tupdesc);
        funcctx->attinmeta = attinmeta;

        if (ht != NULL) {
            hash_seq = palloc0(sizeof(*hash_seq));
            hash_seq_init(hash_seq, ht);
            funcctx->user_fctx = hash_seq;
        }

        MemoryContextSwitchTo(oldcontext);
    }

    /* stuff done on every call of the function */
    funcctx = SRF_PERCALL_SETUP();

    call_cntr = funcctx->call_cntr;
    max_calls = funcctx->max_calls;
    attinmeta = funcctx->attinmeta;
    hash_seq  = funcctx->user_fctx;

    ocgeoHashEntry* hentry = hash_seq != NULL ? hash_seq_search(hash_seq) : NULL;
    if (hentry != NULL)    /* do when there is more left to send */
    {
        char      **values;
        HeapTuple tuple;
        Datum     result;
        int64     total_calls;
        int64     failed_calls;
        double    total_time_ms;
        double    total_time;

        total_calls = hentry->counters.calls;
        failed_calls = total_calls - hentry->counters.success_calls;
        total_time_ms = hentry->counters.total_time;
        total_time = total_time_ms / 1000.0;
        /*
         * Prepare a values array for building the returned tuple.
         * This should be an array of C strings which will
         * be processed later by the type input functions.
         */
        values = (char **) palloc(3 * sizeof(char *));
        values[0] = (char *) palloc(16 * sizeof(char));
        values[1] = (char *) palloc(16 * sizeof(char));
        values[2] = (char *) palloc(16 * sizeof(char));

        snprintf(values[0], 16, "%ld", total_calls);
        snprintf(values[1], 16, "%ld", failed_calls);
        snprintf(values[2], 16, "%.2lf", total_time);

        /* build a tuple */
        tuple = BuildTupleFromCStrings(attinmeta, values);

        /* make the tuple into a datum */
        result = HeapTupleGetDatum(tuple);

        /* clean up (this is not really necessary) */
        pfree(values[0]);
        pfree(values[1]);
        pfree(values[2]);
        pfree(values);

        SRF_RETURN_NEXT(funcctx, result);
    }
    else    /* do when there is no more left */
    {
        if (hash_seq != NULL)
            pfree(hash_seq);
        SRF_RETURN_DONE(funcctx);
    }
}

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

        if (strcmp(def->defname, URI_OPTION) == 0)
            table_options->uri = defGetString(def);

        if (strcmp(def->defname, API_KEY_OPTION) == 0)
            table_options->api_key = defGetString(def);

        if (strcmp(def->defname, MAX_REQS_SEC_OPTION) == 0)
            table_options->max_reqs_sec = atoi(defGetString(def));

        if (strcmp(def->defname, MAX_REQS_DAY_OPTION) == 0)
            table_options->max_reqs_day = atoi(defGetString(def));
    }

    /* Default values: Free plan */

    if (!table_options->max_reqs_day)
        table_options->max_reqs_day = 2500;

    if (!table_options->max_reqs_day)
        table_options->max_reqs_day = 1;
}

char* colnameFromTupleVar(Var* var, TupleDesc tupdesc)
{
    Index varattno = var->varattno;
    char* name = NameStr(TupleDescAttr(tupdesc, varattno - 1)->attname);
    return name;
}


Counters * ocgeoGetCounters(struct ocgeoTableOptions *table_options) {
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

#if PG_VERSION_NUM < 110000
#define		GET_RELID_ATTNAME(foreignTableId, columnId) get_relid_attribute_name(foreignTableId, columnId)
#else
#define		GET_RELID_ATTNAME(foreignTableId, columnId) get_attname(foreignTableId, columnId, false)
#endif

typedef struct ocgeoFdwPlanState {
    ForeignTable* table;
    ForeignServer* server;
    Oid user_id;
    AttInMetadata *attinmeta;

    /* baserestrictinfo clauses, broken down into safe and unsafe subsets. */
	List	   *remote_conds;
	List	   *local_conds;

	/* Bitmap of attr numbers we need to fetch from the remote server. */
	Bitmapset  *attrs_used;

	/* Estimated size and cost for a scan with baserestrictinfo quals. */
	double		rows;
	int			width;
	Cost		startup_cost;
	Cost		total_cost;

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

	List	   *targetColumnList = baserel->reltarget->exprs;
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
										   PVC_RECURSE_PLACEHOLDERS);

		neededColumnList = list_union(neededColumnList, clauseColumnList);
	}

	/* Walk over all column definitions, and de-duplicate column list */
	for (columnIndex = 1; columnIndex <= columnCount; columnIndex++) {
		ListCell   *neededColumnCell;
		Var		   *column = NULL;

		/* Look for this column in the needed column list */
		foreach(neededColumnCell, neededColumnList) {
			Var *neededColumn = lfirst_node(Var, neededColumnCell);
			if (neededColumn->varattno == columnIndex) {
				column = neededColumn;
				break;
			}
		}

		if (column != NULL)
			columnList = lappend(columnList, column);
	}

	return columnList;
}


static void classifyConditions(PlannerInfo *root, RelOptInfo *baserel, 
        Oid foreignTableId, List *input_conds, List **remote_conds, List **local_conds);
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
static void
ocgeoGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{

    elog(DEBUG2,"entering function %s",__func__);

    ocgeoFdwPlanState *fplanstate;
    ocgeoTableOptions table_options;

    fplanstate = palloc0(sizeof(*fplanstate));
    baserel->fdw_private = fplanstate;

    /* Look up foreign-table catalog info. */
    fplanstate->table = GetForeignTable(foreigntableid);
    fplanstate->server = GetForeignServer(fplanstate->table->serverid);
    fplanstate->user_id = baserel->userid;

    /* Get attribute descriptions for the foreign table: */
    Relation rel = RelationIdGetRelation(fplanstate->table->relid);
    TupleDesc desc = RelationGetDescr(rel);
    fplanstate->attinmeta = TupleDescGetAttInMetadata(desc);
    RelationClose(rel);

	/*
	 * Identify which baserestrictinfo clauses can be sent to the remote
	 * server and which can't.
	 */
	classifyConditions(root, baserel, foreigntableid, baserel->baserestrictinfo,
					   &fplanstate->remote_conds, &fplanstate->local_conds);
    elog(DEBUG1, "%s: remote conds: %d, local conds: %d", __func__, list_length(fplanstate->remote_conds),
            list_length(fplanstate->local_conds));

	fplanstate->attrs_used = NULL;
	pull_varattnos((Node *) baserel->reltarget->exprs, baserel->relid, &fplanstate->attrs_used);
    ListCell* lc;
	foreach(lc, fplanstate->local_conds)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		pull_varattnos((Node *) rinfo->clause, baserel->relid,
					   &fplanstate->attrs_used);
	}
    ocgeoGetOptions(foreigntableid, &table_options);

    baserel->tuples = 1e10; /* number of tuples in relation (not considering restrictions) */
    baserel->rows = MAX_RESULTS_REQUESTED; /* estimated number of tuples in the relation after restriction
                           clauses have been applied (ie, output rows of a plan for it) */
    fplanstate->startup_cost = 25;
    fplanstate->total_cost = fplanstate->startup_cost + baserel->rows;
}

/*
 * Create possible access paths for a scan on a foreign table. This is
 * called during query planning. The parameters are the same as for
 * GetForeignRelSize, which has already been called.
 *
 * This function must generate at least one access path (ForeignPath node)
 * for a scan on the foreign table and must call add_path to add each such
 * path to baserel->pathlist. It's recommended to use
 * create_foreignscan_path to build the ForeignPath nodes. The function
 * can generate multiple access paths, e.g., a path which has valid
 * pathkeys to represent a pre-sorted result. Each access path must
 * contain cost estimates, and can contain any FDW-private information
 * that is needed to identify the specific scan method intended.
 */
static void
ocgeoGetForeignPaths(PlannerInfo *root,
                     RelOptInfo *baserel,
                     Oid foreigntableid)
{
    elog(DEBUG2,"entering function %s",__func__);
    ocgeoFdwPlanState *planstate = baserel->fdw_private;

    /* Try to find parameterized paths */
    List* possiblePaths = NIL;
    List* paths = findPaths(root, baserel, possiblePaths, planstate->startup_cost);
    elog(DEBUG1, "%s: param paths %d", __func__, list_length(paths));


    /* if there are parameterized paths (because of joins etc) do not add
     * a default path, to force a nested loop */
    if (list_length(planstate->remote_conds) > 0 && list_length(paths) == 0) {
        /* Add a simple default path */
        paths = lappend(paths, create_foreignscan_path(root, baserel,
#if PG_VERSION_NUM >= 90600
                    NULL,  /* default pathtarget */
#endif
                    baserel->rows,
                    planstate->startup_cost,
#if PG_VERSION_NUM >= 90600
                    baserel->rows * baserel->reltarget->width,
#else
                    baserel->rows * baserel->width,
#endif
                    NIL,		/* no pathkeys */
                    NULL,
#if PG_VERSION_NUM >= 90500
                    NULL,
#endif
                    NULL));
    }

    /* Add each ForeignPath previously found */
    ListCell* lc;
	foreach(lc, paths)
	{
		ForeignPath *path = (ForeignPath *) lfirst(lc);

		/* Add the path without modification */
		add_path(baserel, (Path *) path);
	}

    elog(DEBUG2,"exiting function %s",__func__);
}



static void printRestrictInfoList(List* l, Oid relid)
{
    if (l == NIL)
        return;
    ListCell* lc;
    StringInfoData d;
    initStringInfo(&d);
    foreach(lc, l) {
        /* See https://doxygen.postgresql.org/pathnodes_8h_source.html#l01981 */
        RestrictInfo* r = lfirst_node(RestrictInfo, lc);

        if (!IsA(r->clause, OpExpr)) continue;
        OpExpr* e = IsA(r->clause, OpExpr) ? castNode(OpExpr, r->clause) : NULL;
        int n = list_length(e->args);
        if (n<1) continue;
        Var* var = linitial_node(Var, e->args);
        Const* con = lsecond_node(Const, e->args);

        appendStringInfo(&d, "(%s %s ", GET_RELID_ATTNAME(relid, var->varattno), get_opname(e->opno));
        switch(var->vartype) {
            case TEXTOID:
            case VARCHAROID:
                appendStringInfo(&d, "'%s')", TextDatumGetCString(con->constvalue));
                break;
            case INT4OID:
            case INT2OID:
                appendStringInfo(&d, "%d)", DatumGetInt32(con->constvalue));
                break;
            default:
                appendStringInfo(&d, "..{%d})", var->vartype);
                break;
        }
        if (lc != list_tail(l))
                appendStringInfo(&d, " AND ");
    }
    elog(DEBUG1, "%s: %s", __func__, d.data);
    pfree(d.data);
}

/*
 * Examine each qual clause in input_conds, and classify them into two groups,
 * which are returned as two lists:
 *      - remote_conds contains expressions that can be evaluated remotely
 *      - local_conds contains expressions that can't be evaluated remotely
 */
void
classifyConditions(PlannerInfo *root,
                   RelOptInfo *baserel,
                   Oid foreignTableId,
                   List *input_conds,
                   List **remote_conds,
                   List **local_conds)
{
    ListCell   *lc;

    *remote_conds = NIL;
    *local_conds = NIL;
    Relids relids = baserel->relids;

    /*
    if (IS_UPPER_REL(baserel))
        relids = fpinfo->outerrel->relids;
    else
        relids = baserel->relids;
    */

    foreach(lc, input_conds)
    {
        RestrictInfo *ri = lfirst_node(RestrictInfo, lc);
        Expr* clause = ri->clause;

        if (nodeTag(clause) != T_OpExpr)
            goto add_local;
        OpExpr* opExpr = castNode(OpExpr, clause);
        if (list_length(opExpr->args) != 2) 
            goto add_local;
        if (nodeTag(linitial(opExpr->args)) != T_Var && nodeTag(lsecond(opExpr->args)) != T_Const)
            goto add_local;

        Var* var = linitial_node(Var, opExpr->args);
        char* opName = get_opname(opExpr->opno);
        if (!bms_is_member(var->varno, relids) /* && var->varlevelsup == 0*/) /* attribute belongs to foreign table ? */
            goto add_local;

        /* We can deal with restrictions of the form:
         * q=<string>
         * confidence >= <int>
         */
        char* attName = GET_RELID_ATTNAME(foreignTableId, var->varattno);
        if ((strcmp(opName, "=") == 0 && strcmp(attName, QUERY_ATT_NAME)==0) ||
            (strcmp(opName, ">=") == 0 && strcmp(attName, "confidence")==0)) {
            *remote_conds = lappend(*remote_conds, ri);
            continue;
        }
add_local:
        *local_conds = lappend(*local_conds, ri);
    }
}


/*
 * Create a ForeignScan plan node from the selected foreign access path.
 * This is called at the end of query planning. The parameters are as for
 * GetForeignRelSize, plus the selected ForeignPath (previously produced
 * by GetForeignPaths), the target list to be emitted by the plan node,
 * and the restriction clauses to be enforced by the plan node.
 *
 * This function must create and return a ForeignScan plan node; it's
 * recommended to use make_foreignscan to build the ForeignScan node.
 *
 */
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
    List	   *colList;

    elog(DEBUG2,"entering function %s, %d restrictions",__func__, list_length(baserel->baserestrictinfo));

    printRestrictInfoList(baserel->baserestrictinfo, foreigntableid);
    colList = ColumnList(baserel);

    /*
     * We push down applicable restriction clauses to the API (notably the "query"
     * restriction and the "min_confidence"), but for simplicity
     * we currently put all the restrictionClauses into the plan node's qual
     * list for the executor to re-check. So all we have to do here is strip
     * RestrictInfo nodes from the clauses and ignore pseudoconstants (which
     * will be handled elsewhere).
     */
    scan_clauses = extract_actual_clauses(scan_clauses, false);
    elog(DEBUG1,"%s, %d column list, %d scan clauses",__func__, list_length(colList), list_length(scan_clauses));

    /* Create the ForeignScan node */
    return make_foreignscan(tlist,
                            scan_clauses,
                            scan_relid,
                            scan_clauses,    /* expressions to evaluate */
                            colList, /* private state: the column list */
                            NIL,    /* no custom tlist */
                            NIL,
                            outer_plan);
    elog(DEBUG2,"exiting function %s",__func__);
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

		columnName = GET_RELID_ATTNAME (foreignTableId, columnId);
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
    Oid tableid; /* the foreign table relation id */
    char  *qual_key; /* this should be always QUERY_ATT_NAME */
    char  *qual_value;
    int min_confidence;
    List* columnList;
    HTAB* columnMappingHash;

    struct ocgeo_api* api;
    ocgeo_response_t response;
    ocgeo_result_t* cursor;

} ocgeoForeignScanState;

/*
 * ocgeoBeginForeignScan :     Initiate access to the API
 * Begin executing a foreign scan. This is called during executor startup.
 * It should perform any initialization needed before the scan can start,
 * but not start executing the actual scan (that should be done upon the
 * first call to IterateForeignScan). The ForeignScanState node has
 * already been created, but its fdw_state field is still NULL.
 * Information about the table to scan is accessible through the
 * ForeignScanState node (in particular, from the underlying ForeignScan
 * plan node, which contains any FDW-private information provided by
 * GetForeignPlan). eflags contains flag bits describing the executor's
 * operating mode for this plan node.
 *
 * Note that when (eflags & EXEC_FLAG_EXPLAIN_ONLY) is true, this function
 * should not perform any externally-visible actions; it should only do
 * the minimum required to make the node state valid for
 * ExplainForeignScan and EndForeignScan.
 *
 */
static void
ocgeoBeginForeignScan(ForeignScanState *node, int eflags)
{
    ocgeoTableOptions table_options;
    char       *qual_key   = NULL;
    char       *qual_value = NULL;
    bool        pushdown   = false;
    int min_confidence     = 0;
    ocgeoForeignScanState *sstate;
    Oid foreignTableId = RelationGetRelid(node->ss.ss_currentRelation);
    elog(DEBUG2,"entering function %s",__func__);


    /* Fetch options  */
    ocgeoGetOptions(foreignTableId, &table_options);

    ForeignScan* foreignScan = (ForeignScan *) node->ss.ps.plan;
    List* columnList = (List*) foreignScan->fdw_private;

	HTAB* columnMappingHash = ColumnMappingHash(foreignTableId, columnList);

    /* Stash away the state info we have already */
    sstate = palloc0(sizeof(*sstate));
    node->fdw_state = sstate;

    /* Store the additional state info */
    sstate->attinmeta = TupleDescGetAttInMetadata(node->ss.ss_currentRelation->rd_att);
    sstate->tableid = foreignTableId;
    sstate->api = NULL;
    sstate->columnList = columnList;
    sstate->columnMappingHash = columnMappingHash;
    sstate->cursor = NULL;
    sstate->api = NULL;

    List* qual_list = NULL;
    ListCell* lc;
    foreach(lc, foreignScan->fdw_exprs) {
        extractRestrictions(bms_make_singleton(foreignScan->scan.scanrelid),
                            lfirst_node(Expr, lc), &qual_list);
    }
    foreach(lc, qual_list) {
        ExprContext *econtext = node->ss.ps.ps_ExprContext;
        List* clause = lfirst_node(List, lc);
        Assert(list_length(clause)==3);

        Var* var = linitial_node(Var, clause);
        char* op = (char*) lsecond(clause);
        Expr* expr = lthird_node(Expr, clause);

        char* attName = colnameFromTupleVar(var, sstate->attinmeta->tupdesc);
        Datum value;
        bool isNull = false;
        Oid valueType;
        ExprState  * expr_state = NULL;
        switch(nodeTag(expr)) {
            case T_Param:
                {
                    expr_state = ExecInitExpr(expr, (PlanState *) node);

#if PG_VERSION_NUM >= 100000
                    value = ExecEvalExpr(expr_state, econtext, &isNull);
#else
                    value = ExecEvalExpr(expr_state, econtext, &isNull, NULL);
#endif
                    valueType = ((Param*) expr)->paramtype;
                    break;
                }
            case T_Const:
                {
                    isNull = ((Const*) expr)->constisnull;
                    value = ((Const*) expr)-> constvalue;
                    valueType = ((Const*) expr)->consttype;
                    break;
                }
            default:
                break;
        }
        if (isNull || value==0)
            continue;
        if (strcmp(attName, QUERY_ATT_NAME)==0 && strcmp(op, "=")==0) {
            qual_key = pstrdup(QUERY_ATT_NAME);
            qual_value = TextDatumGetCString(value);
            pushdown = true;
        }
        else if (strcmp(attName, "confidence")==0 && strcmp(op, ">=")==0) {
            min_confidence = DatumGetInt32(value);
        }
    }

    sstate->qual_key = qual_key;
    sstate->qual_value = pushdown ? qual_value : NULL;
    sstate->min_confidence = min_confidence;

    elog(DEBUG1,"function %s qual: %s='%s' and confidence>=%d",__func__, qual_key, qual_value, min_confidence);


    /*
     * Do nothing more in EXPLAIN (no ANALYZE) case.
     */
    if (eflags & EXEC_FLAG_EXPLAIN_ONLY) {
        elog(DEBUG2,"exiting function %s",__func__);
        return;
    }


    sstate->api = ocgeo_init(table_options.api_key, table_options.uri);

    /* Make a forward request, only if some restriction is given (pushed-down) */
    if (sstate->qual_value != NULL) {
        Counters* stats       = ocgeoGetCounters(&table_options);
        ocgeo_params_t params = ocgeo_default_params();
        params.limit          = MAX_RESULTS_REQUESTED;
        struct timeval start_time, end_time;
        double tmsec;

        if (sstate->min_confidence)
            params.min_confidence = sstate->min_confidence;

        gettimeofday(&start_time, NULL);
        bool ok = ocgeo_forward(sstate->api, sstate->qual_value, &params, &sstate->response);
        gettimeofday(&end_time, NULL);
        
        /* Update statistics */
        tmsec = (end_time.tv_sec - start_time.tv_sec)*1000 + (end_time.tv_usec - start_time.tv_usec)/1000.0L;
        stats->calls++;
        stats->total_time += tmsec;
        if (ok && ocgeo_response_ok(&sstate->response)) {
            stats->success_calls++;
            sstate->cursor   = sstate->response.results;
            stats->rate_info = sstate->response.rateInfo;
        }
        elog(DEBUG1,"API %s returned status: %d, results: %d, time: %.2lf msec",
                sstate->response.url, sstate->response.status.code, sstate->response.total_results, tmsec);
    }
    elog(DEBUG2,"exiting function %s",__func__);
}

/*
 * Fetch one row from the foreign source, returning it in a tuple table
 * slot (the node's ScanTupleSlot should be used for this purpose). Return
 * NULL if no more rows are available. The tuple table slot infrastructure
 * allows either a physical or virtual tuple to be returned; in most cases
 * the latter choice is preferable from a performance standpoint. Note
 * that this is called in a short-lived memory context that will be reset
 * between invocations. Create a memory context in BeginForeignScan if you
 * need longer-lived storage, or use the es_query_cxt of the node's
 * EState.
 *
 * The rows returned must match the column signature of the foreign table
 * being scanned. If you choose to optimize away fetching columns that are
 * not needed, you should insert nulls in those column positions.
 *
 * Note that PostgreSQL's executor doesn't care whether the rows returned
 * violate any NOT NULL constraints that were defined on the foreign table
 * columns — but the planner does care, and may optimize queries
 * incorrectly if NULL values are present in a column declared not to
 * contain them. If a NULL value is encountered when the user has declared
 * that none should be present, it may be appropriate to raise an error
 * (just as you would need to do in the case of a data type mismatch).
 */
TupleTableSlot* ocgeoIterateForeignScan(ForeignScanState * node)
{
    ocgeoForeignScanState* sstate = node->fdw_state;
    ocgeo_result_t* current_result  = sstate->cursor;

    Oid foreignTableId = node->ss.ss_currentRelation->rd_node.relNode;
    TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
    elog(DEBUG2, "entering %s Relid: %d", __func__, foreignTableId);

    /* EState         *estate = node->ss.ps.state; */
    /* MemoryContext   oldcontext = MemoryContextSwitchTo(estate->es_query_cxt); */

    TupleDesc	tupleDescriptor = slot->tts_tupleDescriptor;
	Datum	   *columnValues = slot->tts_values;
	bool	   *columnNulls = slot->tts_isnull;
	int32		columnCount = tupleDescriptor->natts;

    /* initialize all values for this row to null */
    memset(columnValues, 0, sizeof(Datum) * columnCount);
    memset(columnNulls, true, sizeof(bool) * columnCount);

    /*
	 * We execute the protocol to load a virtual tuple into a slot. We first
	 * call ExecClearTuple, then fill in values / isnull arrays, and last call
	 * ExecStoreVirtualTuple.  If we are done iterating over the API results,
	 * we just return an empty slot as required.
	 */
    ExecClearTuple(slot);

    /* no results or results finished */
    if (current_result == NULL) {
        elog(DEBUG2,"exiting function %s",__func__);
        return slot;
    }
    sstate->cursor = current_result->next; /* move the cursor to the next result for next iteration */
    
    /*
    make_tuple_from_result_row(current_result,
                               tupleDescriptor, festate->retrieved_attrs,
                               slot->tts_values, slot->tts_isnull);
    */
    ListCell* col;
    List* columnList = sstate->columnList;
    foreach(col, columnList) {

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

        /* elog(DEBUG1,"%s: Column: %s, index=%d, type=%d",__func__, columnName, 
               columnMapping->columnIndex, columnMapping->columnTypeId); */

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
                                   columnValue = DirectFunctionCall1(point_in, PointerGetDatum(fieldVal.data));
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
    elog(DEBUG2,"exiting function %s",__func__);
    return slot;
}


static void
ocgeoReScanForeignScan(ForeignScanState *node)
{
    elog(DEBUG2,"entering function %s",__func__);
    ocgeoEndForeignScan(node);
    ocgeoBeginForeignScan(node, 0);
    elog(DEBUG2,"exiting function %s",__func__);
}


static void
ocgeoEndForeignScan(ForeignScanState *node)
{
    elog(DEBUG2,"entering function %s",__func__);
    ocgeoForeignScanState* sstate = node->fdw_state;
    ocgeo_close(sstate->api);
    elog(DEBUG2,"exiting function %s",__func__);
}


void ocgeoExplainForeignScan (ForeignScanState * node, ExplainState * es)
{
    ocgeoForeignScanState *sstate = node->fdw_state;

    elog(DEBUG2, "Entering function %s", __func__);
    StringInfo q = makeStringInfo();
    appendStringInfo(q, "q='%s'", sstate->qual_value);
    if (sstate->min_confidence)
        appendStringInfo(q, "&min_confidence='%d'", sstate->min_confidence);

    ExplainPropertyText("OpenCageData API query", q->data, es);
    elog(DEBUG2,"exiting function %s",__func__);
}


/*
 *	Test wheter an attribute identified by its relid and attno
 *	is present in a list of restrictinfo
 */
bool
isAttrInRestrictInfo(Index relid, AttrNumber attno, RestrictInfo *restrictinfo)
{
	List	   *vars = pull_var_clause((Node *) restrictinfo->clause,
#if PG_VERSION_NUM >= 90600
										PVC_RECURSE_AGGREGATES|
										PVC_RECURSE_PLACEHOLDERS);
#else
										PVC_RECURSE_AGGREGATES,
										PVC_RECURSE_PLACEHOLDERS);
#endif
	ListCell   *lc;

	foreach(lc, vars)
	{
		Var		   *var = (Var *) lfirst(lc);

		if (var->varno == relid && var->varattno == attno)
		{
			return true;
		}

	}
	return false;
}

List *
clausesInvolvingAttr(Index relid, AttrNumber attnum, EquivalenceClass *ec)
{
	List	   *clauses = NULL;

	/*
	 * If there is only one member, then the equivalence class is either for
	 * an outer join, or a desired sort order. So we better leave it
	 * untouched.
	 */
	if (ec->ec_members->length > 1)
	{
		ListCell   *ri_lc;

		foreach(ri_lc, ec->ec_sources)
		{
			RestrictInfo *ri = (RestrictInfo *) lfirst(ri_lc);

			if (isAttrInRestrictInfo(relid, attnum, ri))
			{
				clauses = lappend(clauses, ri);
			}
		}
	}
	return clauses;
}

List *
findPaths(PlannerInfo *root, RelOptInfo *baserel, List *possiblePaths, int startupCost)
{
	List	   *result = NIL;
    int i;
    ocgeoFdwPlanState* state = baserel->fdw_private;
    AttInMetadata* attinmeta = state->attinmeta;
    AttrNumber	attnum = InvalidAttrNumber;
    for (i = 0; i < attinmeta->tupdesc->natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(attinmeta->tupdesc,i);

        if (attr->attisdropped)
            continue;
        char* attname = NameStr(attr->attname);
        if (strcmp(attname, QUERY_ATT_NAME) == 0) {
            attnum = i + 1;
            break;
        }
    }
    if (attnum == InvalidAttrNumber)
        return result;

    int			nbrows = 10; // XXX
    Bitmapset  *outer_relids = NULL;

    /* Armed with this knowledge, look for a join condition */
    /* matching the path list. */
    /* Every key must be present in either, a join clause or an */
    /* equivalence_class. */
    ListCell   *lc;
    List	   *clauses = NIL;

    /* Look in the equivalence classes. */
    foreach(lc, root->eq_classes) {
        EquivalenceClass *ec = (EquivalenceClass *) lfirst(lc);
        List	   *ec_clauses = clausesInvolvingAttr(baserel->relid,
                attnum,
                ec);

        clauses = list_concat(clauses, ec_clauses);
        if (ec_clauses != NIL)
        {
            outer_relids = bms_union(outer_relids, ec->ec_relids);
        }
    }
    /* Do the same thing for the outer joins */
    foreach(lc, list_union(root->left_join_clauses, root->right_join_clauses)) {
        RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

        if (isAttrInRestrictInfo(baserel->relid, attnum, ri)) {
            clauses = lappend(clauses, ri);
            outer_relids = bms_union(outer_relids,
                    ri->outer_relids);

        }
    }
    if (clauses == NIL)
        return result;

    /* Every key has a corresponding restriction, we can build */
    /* the parameterized path and add it to the plan. */
    Bitmapset  *req_outer = bms_difference(outer_relids, bms_make_singleton(baserel->relid));
    ParamPathInfo *ppi;
    ForeignPath *foreignPath;

    if (!bms_is_empty(req_outer)) {
        ppi = makeNode(ParamPathInfo);
        ppi->ppi_req_outer = req_outer;
        ppi->ppi_rows = nbrows;
        ppi->ppi_clauses = list_concat(ppi->ppi_clauses, clauses);
        /* Add a simple parameterized path */
        foreignPath = create_foreignscan_path(
                root, baserel,
#if PG_VERSION_NUM >= 90600
                NULL,  /* default pathtarget */
#endif
                nbrows,
                startupCost,
#if PG_VERSION_NUM >= 90600
                nbrows * baserel->reltarget->width,
#else
                nbrows * baserel->width,
#endif
                NIL, /* no pathkeys */
                NULL,
#if PG_VERSION_NUM >= 90500
                NULL,
#endif
                NULL);

        foreignPath->path.param_info = ppi;
        result = lappend(result, foreignPath);
    }
    return result;
}

