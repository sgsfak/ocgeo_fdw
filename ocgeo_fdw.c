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

#include "ocgeo_api.h"

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

static void * my_pcalloc(size_t nmemb, size_t size);

/* Postgres C lacks a calloc version for memory allocation. The following
 * has been adopted from the OpenBSD code, which addresses integer overflow
 * errors, see http://bxr.su/OpenBSD/lib/libc/stdlib/malloc.c#1722
 */
void *
my_pcalloc(size_t nmemb, size_t size)
{
    void *r = NULL;

/*
 * This is sqrt(SIZE_MAX+1), as s1*s2 <= SIZE_MAX
 * if both s1 < MUL_NO_OVERFLOW and s2 < MUL_NO_OVERFLOW
 */
#   define MUL_NO_OVERFLOW (1UL << (sizeof(size_t) * 4))

    if ((nmemb >= MUL_NO_OVERFLOW || size >= MUL_NO_OVERFLOW) &&
        nmemb > 0 && SIZE_MAX / nmemb < size) {
        elog(ERROR, "invalid memory alloc request for %zu elements of size %zu each", nmemb, size);
        return NULL;
    }

    size *= nmemb;
    r = palloc0(size * nmemb);
    return r;
}

/*
 * _PG_init
 *      Library load-time initalization.
 *      Sets exitHook() callback for backend shutdown.
 */
void _PG_init (void)
{
    // register an exit hook 
    // on_proc_exit (&exitHook, PointerGetDatum (NULL));

    /* We set the CURL memory handlers to be the ones provided by Postgres
       plus the "pcalloc" one we wrote */

    elog(DEBUG1,"function %s, before curl global init",__func__);
    //curl_global_init_mem(CURL_GLOBAL_ALL, palloc, pfree, repalloc, pstrdup, my_pcalloc);
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
static Counters * GetCounters(struct ocgeoTableOptions *table_options);

static void
ocgeoGetOptions(Oid foreigntableid, ocgeoTableOptions *table_options)
{
    ForeignTable *table;
    ForeignServer *server;
    UserMapping *mapping;
    List       *options;
    ListCell   *lc;

#ifdef DEBUG
    elog(NOTICE, "ocgeoGetOptions");
#endif

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

    if (!node)
        return;

    if (IsA(node, OpExpr))
    {
        OpExpr     *op = (OpExpr *) node;
        Node       *left,
        *right;
        Index       varattno;

        if (list_length(op->args) != 2)
            return;

        left = list_nth(op->args, 0);

        if (!IsA(left, Var))
            return;

        varattno = ((Var *) left)->varattno;

        right = list_nth(op->args, 1);

        if (IsA(right, Const))
        {
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

            return;
        }
    }

    return;
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
                            NIL,    /* no private state either */
                            NIL,    /* no custom tlist */
                            NIL,    /* no remote quals */
                            outer_plan);
}


typedef struct ocgeoForeignScanState {
    AttInMetadata *attinmeta;
    CURL  *curl;
    char  *qual_key;
    char  *qual_value;
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

    elog(DEBUG1,"entering function %s",__func__);


    /* Fetch options  */
    ocgeoGetOptions(RelationGetRelid(node->ss.ss_currentRelation),
                    &table_options);

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

    /* OK, we connected. If this is an EXPLAIN, bail out now */
    if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
        return;

    /*
     * We're going to use the current scan-lived context to
     * store the pstrduped cusrsor id.
     */
    sstate->mctxt = CurrentMemoryContext;

    elog(DEBUG1,"function %s, before ocgeo init: api: %s , server : %s",__func__,table_options.api_key, table_options.uri);
    sstate->api = ocgeo_init(table_options.api_key, table_options.uri, palloc, pfree);
    elog(DEBUG1,"function %s, after ocgeo init",__func__);
    sstate->cursor = NULL;

    /* Make a forward request */
    ocgeo_forward(sstate->api, sstate->qual_value, NULL, &sstate->response);
    if (ocgeo_response_ok(&sstate->response)) {
        sstate->cursor = sstate->response.results;
    }
    
    elog(DEBUG1,"In %s API returned status: %d, results: %d",__func__, sstate->response.status.code, sstate->response.total_results);
}

TupleTableSlot* ocgeoIterateForeignScan(ForeignScanState * node)
{
    ocgeoForeignScanState* sstate = node->fdw_state;
    TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
    ocgeo_result_t* current_result  = sstate->cursor;

    MemoryContext    oldcontext;
    char      **values;
    HeapTuple   tuple;

    elog(DEBUG1,"entering function %s result: %s",__func__, current_result ? current_result->city : "");
    
    ExecClearTuple(slot);

    /* no results or results finished */
    if (current_result == NULL) {
        return slot;
    }
    sstate->cursor = current_result->next;


/* Build the tuple */
    values = (char **) palloc(sizeof(char *) * 2);
    values[0] = pstrdup(current_result->city);
    values[1] = pstrdup(current_result->city);

    tuple = BuildTupleFromCStrings(
        TupleDescGetAttInMetadata(node->ss.ss_currentRelation->rd_att),
        values);
    ExecStoreTuple(tuple, slot, InvalidBuffer, false);
    

/*
    // oldcontext = MemoryContextSwitchTo(node->ss.ps.ps_ExprContext->ecxt_per_query_memory);

    values = palloc(sizeof(*values) * 2);
    values[0] = pstrdup("reply");
    values[1] = pstrdup("MANMOS");

    tuple = BuildTupleFromCStrings(sstate->attinmeta, values);
    // MemoryContextSwitchTo(oldcontext);
    // ExecStoreTuple(tuple, slot, InvalidBuffer, false);
    ExecStoreTuple(tuple, slot, InvalidBuffer, true);*/

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

