#include "postgres.h"
#include "funcapi.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/guc.h"
#include "access/xact.h"
#include "regex/regex.h"

#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>

extern void GoFuzz();
extern void staticdeathcallback();
extern void errorcallback(const char *errorname);

extern MemoryContext RegexpContext;

static int in_fuzzer;

static int paranoid_rcancelrequested(void);
static int check_heap_allocs();

size_t
WatchMemoryContextStats(MemoryContext context);
static void
MemoryContextStatsInternal(MemoryContext context, int level,
						   MemoryContextCounters *totals);


PG_MODULE_MAGIC;

SPIPlanPtr plan;

void fuzz_exit_handler(int code, Datum arg) {
	if (in_fuzzer)
		abort();
	//		staticdeathcallback();
}


static void limit_resources() {
	int i;
	struct rlimit old, new;
	struct {
		char *resource_name;
		int resource;
		rlim_t new_soft_limit;
		rlim_t new_hard_limit;
	} limits[] = {
		// { "max memory size", RLIMIT_AS, 200000000 },
		{ "core file size", RLIMIT_CORE, 0 , 0},
		// { "cpu time", RLIMIT_CPU, 1, 300},
		{ "data seg size", RLIMIT_DATA, 200000000, RLIM_INFINITY},
	};
	
	for (i=0; i<sizeof(limits)/sizeof(*limits); i++) {
		int retval;
		retval = getrlimit(limits[i].resource, &old);
		if (retval < 0) {
			perror("getrlimit");
			abort();
		}
		new.rlim_cur = limits[i].new_soft_limit;
		new.rlim_max = limits[i].new_soft_limit;
		if (new.rlim_max > old.rlim_max)
			new.rlim_max = old.rlim_max;
		fprintf(stderr, "Setting %s to %zd / %zd (was %zd / %zd)\n", 
				limits[i].resource_name, 
				new.rlim_cur, new.rlim_max, 
				old.rlim_cur, old.rlim_max);
		retval = setrlimit(limits[i].resource, &new);
		if (retval < 0) {
			perror("setrlimit");
			abort();
		}
	}
}

PG_FUNCTION_INFO_V1(test_fuzz_environment);
Datum
test_fuzz_environment(PG_FUNCTION_ARGS){
	if (!RegexpContext)
		elog(ERROR, "RegexpContext does not exist");

	elog(WARNING, "setting rlimit");
	limit_resources();

	elog(WARNING, "setting rcancelrequested func");
	pg_regex_set_rcancel(&paranoid_rcancelrequested);

	elog(WARNING, "setting statement_timeout");
	SetConfigOption("statement_timeout", "1000", PGC_SUSET, PGC_S_OVERRIDE);

	PG_RETURN_NULL();
}	

/* Postgres SQL Function to invoke fuzzer */

PG_FUNCTION_INFO_V1(fuzz);
Datum
fuzz(PG_FUNCTION_ARGS)
{
	unsigned runs = PG_GETARG_INT32(0);
	text *expr_text = PG_GETARG_TEXT_P(1);
	char *expr = text_to_cstring(expr_text);
	Oid argtypes[1] = { TEXTOID };
	int retval;

	if (runs > 400000000)
		elog(ERROR, "Unreasonable number of runs");

	limit_resources();

	pg_regex_set_rcancel(&paranoid_rcancelrequested);

	/* If Postgres handles a FATAL error it'll exit cleanly but we
	 * want to treat the last test as a failure */
	on_proc_exit(fuzz_exit_handler, 0);
	in_fuzzer = 1;

	retval = SPI_connect();
	if (retval != SPI_OK_CONNECT)
		abort();

	/* A query which takes 3s is a slow query but at least it calls
	 * CHECK_FOR_INTERRUPTS enough.
	 */
	SetConfigOption("statement_timeout", "3000", PGC_SUSET, PGC_S_OVERRIDE);

	/* Prepare once before we start the driver */
	plan = SPI_prepare(expr, 1, argtypes);
	if (!plan)
		elog(ERROR, "Failed to plan query");

	retval = SPI_getargcount(plan);
	if (retval != 1)
		elog(ERROR, "Query to fuzz must take precisely one parameter");

	/* Invoke the driver via the test_harness.cpp C++ code */

	GoFuzz(runs);

	SPI_finish();

	/* disable the proc_exit call which calls the deathcallback */
	in_fuzzer = 0;

	PG_RETURN_NULL();
}		

/* 
 * Callback from fuzzer to execute one fuzz test case as set up in
 * global "plan" variable by fuzz() 
 */

void FuzzOne(const char *Data, size_t Size) {
	text *arg = cstring_to_text_with_len(Data, Size);

	static unsigned long n_execs, n_success, n_fail, n_null;
	static int last_error, last_error_count;
	MemoryContext oldcontext = CurrentMemoryContext;
 	ResourceOwner oldowner = CurrentResourceOwner;

	CHECK_FOR_INTERRUPTS();

	n_execs++;

	/* Not sure why we're being passed NULL */
	if (!Data) {
		n_null++;
		return;
	}

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(CurrentMemoryContext);

 	PG_TRY();
 	{
		Datum values[1] = { PointerGetDatum(arg) };
		int retval;

		retval = SPI_execute_plan(plan, values,
								  NULL /* nulls */,
								  true, /* read-only */
								  0 /* max rows */);
		SPI_freetuptable(SPI_tuptable);

		if (retval == SPI_OK_SELECT)
			n_success++;
		else if (retval >= 0)
			fprintf(stderr, "SPI reports non-select run retval=%d\n", retval);
		else
			abort();

		last_error_count = 0;
		last_error = 0;

		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
		SPI_restore_connection();
 	}
 	PG_CATCH();
 	{
		/* Save error info */
		MemoryContextSwitchTo(oldcontext);
		ErrorData  *edata = CopyErrorData();
		FlushErrorState();

		/* Abort the inner transaction */
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;

		SPI_restore_connection();

		n_fail++;

		/* INTERNAL_ERROR is definitely a bug. The others debatable
		 * but in particular we're interested in infinite recursion
		 * caught by check_for_stack_depth() which shows up as
		 * STATEMENT_TOO_COMPLEX which is in the
		 * PROGRAM_LIMIT_EXCEEDED category
		 */
		
		int errcategory = ERRCODE_TO_CATEGORY(edata->sqlerrcode);
		if (errcategory == ERRCODE_PROGRAM_LIMIT_EXCEEDED ||
			errcategory == ERRCODE_INSUFFICIENT_RESOURCES ||
			errcategory == ERRCODE_OPERATOR_INTERVENTION || /* statement_timeout */
			errcategory == ERRCODE_INTERNAL_ERROR ||
			(edata->sqlerrcode == ERRCODE_INVALID_REGULAR_EXPRESSION &&
			 (strstr(edata->message, "regular expression failed") ||
			  strstr(edata->message, "out of memory") ||
			  strstr(edata->message, "cannot happen") ||
			  strstr(edata->message, "too complex") ||
			  strstr(edata->message, "too many colors") ||
			  strstr(edata->message, "operation cancelled"))))
			{
				if (last_error != edata->sqlerrcode) {
					last_error = edata->sqlerrcode;
					last_error_count = 0;
				} else if (last_error_count++ > 10) {
					abort();
				}

				if (in_fuzzer) {
					char errorname[80];
					sprintf(errorname, "error-%s", unpack_sql_state(edata->sqlerrcode));
					fprintf(stderr, "Calling errocallback for %s (%s)\n", errorname, edata->message);
					errorcallback(errorname);
				}

				/* we were in a subtransaction so yay we can continue */
				FreeErrorData(edata);

				// XXX
				MemoryContextStats(RegexpContext);
			}
		else
			{
				last_error = 0;
				last_error_count = 0;

				FreeErrorData(edata);
			}
	}
	PG_END_TRY();

	pfree(arg);

	/* Every power of two executions print progress */
	if ((n_execs & (n_execs-1)) == 0) {
		static int  old_n_execs;
		fprintf(stderr, "FuzzOne n=%lu  success=%lu  fail=%lu  null=%lu\n", n_execs, n_success, n_fail, n_null);
		size_t totaldiff = WatchMemoryContextStats(TopMemoryContext);
		unsigned long ndiff = n_execs - old_n_execs;
		if (ndiff > 0 && totaldiff > 0)
			fprintf(stderr, "Memory used: %lu bytes in %lu calls (%lu bytes/call)\n", totaldiff, ndiff, totaldiff / ndiff);
		if ((totaldiff > 0 && n_execs > 200) || (totaldiff > 10000 && n_execs > 5)) {
			MemoryContextStats(TopMemoryContext);
		}		
		old_n_execs = n_execs;
	}
}



static int
paranoid_rcancelrequested(void)
{

	check_stack_depth();

	int lackmem = 0;
	static unsigned i;
	if (i++ % 20000 == 0)
		lackmem = check_heap_allocs();

	return lackmem || (InterruptPending && (QueryCancelPending || ProcDiePending));
}

static int check_heap_allocs() {
	MemoryContextCounters grand_totals;
	size_t memory_used;
	memset(&grand_totals, 0, sizeof(grand_totals));
	MemoryContextStatsInternal(RegexpContext, 0, &grand_totals);
	memory_used = grand_totals.totalspace - grand_totals.freespace;
	if (memory_used > (size_t)work_mem * 1024) {
		fprintf(stderr, "Too much memory used calling errorcallback (total=%zd MB > work_mem=%zd MB)\n",
				memory_used / 1024 / 1024,
				(size_t)work_mem / 1024
				);
		if (in_fuzzer)
			errorcallback("regexmem");
		return 1;
	} else {
		return 0;
	}
}

size_t
WatchMemoryContextStats(MemoryContext context)
{
	int totaldiff;
	static MemoryContextCounters old_totals;
	MemoryContextCounters grand_totals;

	memset(&grand_totals, 0, sizeof(grand_totals));
	MemoryContextStatsInternal(context, 0, &grand_totals);
	totaldiff = grand_totals.totalspace - old_totals.totalspace;

	if (totaldiff > 0)
		
		fprintf(stderr,
				"Memory Use Summary: %zu bytes in %zd blocks; %zu free (%zd chunks); %zu used\n",
				grand_totals.totalspace, grand_totals.nblocks,
				grand_totals.freespace, grand_totals.freechunks,
				grand_totals.totalspace - grand_totals.freespace);
		
	old_totals = grand_totals;

	return totaldiff;
}

/*
 * MemoryContextStatsInternal
 *		One recursion level for MemoryContextStats
 *
 * Copied from mcxt.c with the printouts and max_children removed
 *
 */
static void
MemoryContextStatsInternal(MemoryContext context, int level,
						   MemoryContextCounters *totals)
{
	MemoryContext child;
	int			ichild;

	AssertArg(MemoryContextIsValid(context));

	/* Examine the context itself */
	(*context->methods->stats) (context, level, false, totals);

	/* Examine children */
	for (child = context->firstchild, ichild = 0;
		 child != NULL;
		 child = child->nextchild, ichild++)
	{
		MemoryContextStatsInternal(child, level + 1,
								   totals);
	}
}
