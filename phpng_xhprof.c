/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2015 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author:  Yaoguai (newtopdtdio@163.com)                               |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef linux
/* To enable CPU_ZERO and CPU_SET, etc.     */
# define _GNU_SOURCE
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_phpng_xhprof.h"
#include "zend_extensions.h"
#include <sys/time.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef __FreeBSD__
# if __FreeBSD_version >= 700110
#   include <sys/resource.h>
#   include <sys/cpuset.h>
#   define cpu_set_t cpuset_t
#   define SET_AFFINITY(pid, size, mask) \
           cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, size, mask)
#   define GET_AFFINITY(pid, size, mask) \
           cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, size, mask)
# else
#   error "This version of FreeBSD does not support cpusets"
# endif /* __FreeBSD_version */
#elif __APPLE__
/*
 * Patch for compiling in Mac OS X Leopard
 * @author Svilen Spasov <s.spasov@gmail.com>
 */
#    include <mach/mach_init.h>
#    include <mach/thread_policy.h>
#    define cpu_set_t thread_affinity_policy_data_t
#    define CPU_SET(cpu_id, new_mask) \
        (*(new_mask)).affinity_tag = (cpu_id + 1)
#    define CPU_ZERO(new_mask)                 \
        (*(new_mask)).affinity_tag = THREAD_AFFINITY_TAG_NULL
#   define SET_AFFINITY(pid, size, mask)       \
        thread_policy_set(mach_thread_self(), THREAD_AFFINITY_POLICY, mask, \
                          THREAD_AFFINITY_POLICY_COUNT)
#else
/* For sched_getaffinity, sched_setaffinity */
# include <sched.h>
# define SET_AFFINITY(pid, size, mask) sched_setaffinity(0, size, mask)
# define GET_AFFINITY(pid, size, mask) sched_getaffinity(0, size, mask)
#endif /* __FreeBSD__ */



/**
 * **********************
 * GLOBAL MACRO CONSTANTS
 * **********************
 */

/* XHProf version                           */
#define XHPROF_VERSION       "0.9.5"

/* Fictitious function name to represent top of the call tree. The paranthesis
 * in the name is to ensure we don't conflict with user function names.  */
#define ROOT_SYMBOL                "main()"

/* Size of a temp scratch buffer            */
#define SCRATCH_BUF_LEN            512

/* Various XHPROF modes. If you are adding a new mode, register the appropriate
 * callbacks in hp_begin() */
#define XHPROF_MODE_HIERARCHICAL            1
#define XHPROF_MODE_SAMPLED            620002      /* Rockfort's zip code */

/* Hierarchical profiling flags.
 *
 * Note: Function call counts and wall (elapsed) time are always profiled.
 * The following optional flags can be used to control other aspects of
 * profiling.
 */
#define XHPROF_FLAGS_NO_BUILTINS   0x0001         /* do not profile builtins */
#define XHPROF_FLAGS_CPU           0x0002      /* gather CPU times for funcs */
#define XHPROF_FLAGS_MEMORY        0x0004   /* gather memory usage for funcs */

/* Constants for XHPROF_MODE_SAMPLED        */
#define XHPROF_SAMPLING_INTERVAL       100000      /* In microsecs        */

/* Constant for ignoring functions, transparent to hierarchical profile */
#define XHPROF_MAX_IGNORED_FUNCTIONS  256
#define XHPROF_IGNORED_FUNCTION_FILTER_SIZE                           \
               ((XHPROF_MAX_IGNORED_FUNCTIONS + 7)/8)

#if !defined(uint64)
typedef unsigned long long uint64;
#endif
#if !defined(uint32)
typedef unsigned int uint32;
#endif
#if !defined(uint8)
typedef unsigned char uint8;
#endif


/**
 * *****************************
 * GLOBAL DATATYPES AND TYPEDEFS
 * *****************************
 */

/* XHProf maintains a stack of entries being profiled. The memory for the entry
 * is passed by the layer that invokes BEGIN_PROFILING(), e.g. the hp_execute()
 * function. Often, this is just C-stack memory.
 *
 * This structure is a convenient place to track start time of a particular
 * profile operation, recursion depth, and the name of the function being
 * profiled. */
typedef struct hp_entry_t
{
	char                   *name_hprof;                       /* function name */
	int                     rlvl_hprof;        /* recursion level for function */
	uint64                  tsc_start;         /* start value for TSC counter  */
	long int                mu_start_hprof;                    /* memory usage */
	long int                pmu_start_hprof;              /* peak memory usage */
	struct rusage           ru_start_hprof;             /* user/sys time start */
	struct hp_entry_t      *prev_hprof;    /* ptr to prev entry being profiled */
	uint8                   hash_code;     /* hash_code for the function name  */
} hp_entry_t;

/* Various types for XHPROF callbacks       */
typedef void (*hp_init_cb)();
typedef void (*hp_exit_cb)();
typedef void (*hp_begin_function_cb)(hp_entry_t **entries, hp_entry_t *current);
typedef void (*hp_end_function_cb)(hp_entry_t **entries);

/* Struct to hold the various callbacks for a single xhprof mode */
typedef struct hp_mode_cb
{
	hp_init_cb init_cb;
	hp_exit_cb exit_cb;
	hp_begin_function_cb begin_fn_cb;
	hp_end_function_cb end_fn_cb;
} hp_mode_cb;

/* Xhprof's global state.
 *
 * This structure is instantiated once.  Initialize defaults for attributes in
 * hp_init_profiler_state() Cleanup/free attributes in
 * hp_clean_profiler_state() */
ZEND_BEGIN_MODULE_GLOBALS(xhprof)
	/*       ----------   Global attributes:  -----------       */

	/* Indicates if xhprof is currently enabled */
	int enabled;

	/* Indicates if xhprof was ever enabled during this request */
	int ever_enabled;

	/* Holds all the xhprof statistics */
	zval stats_count;

	/* Indicates the current xhprof mode or level */
	int profiler_level;

	/* Top of the profile stack */
	hp_entry_t *entries;

	/* freelist of hp_entry_t chunks for reuse... */
	hp_entry_t *entry_free_list;

	/* Callbacks for various xhprof modes */
	hp_mode_cb mode_cb;

	/*       ----------   Mode specific attributes:  -----------       */

	/* Global to track the time of the last sample in time and ticks */
	struct timeval last_sample_time;
	uint64 last_sample_tsc;
	/* XHPROF_SAMPLING_INTERVAL in ticks */
	uint64 sampling_interval_tsc;

	/* This array is used to store cpu frequencies for all available logical
	 * cpus.  For now, we assume the cpu frequencies will not change for power
	 * saving or other reasons. If we need to worry about that in the future, we
	 * can use a periodical timer to re-calculate this arrary every once in a
	 * while (for example, every 1 or 5 seconds). */
	double *cpu_frequencies;

	/* The number of logical CPUs this machine has. */
	uint32 cpu_num;

	/* The saved cpu affinity. */
	cpu_set_t prev_mask;

	/* The cpu id current process is bound to. (default 0) */
	uint32 cur_cpu_id;

	/* XHProf flags */
	uint32 xhprof_flags;

	/* counter table indexed by hash value of function names. */
	uint8 func_hash_counters[256];

	/* Table of ignored function names and their filter */
	char **ignored_function_names;
	uint8 ignored_function_filter[XHPROF_IGNORED_FUNCTION_FILTER_SIZE];

ZEND_END_MODULE_GLOBALS(xhprof);

#define XHG(v) ZEND_MODULE_GLOBALS_ACCESSOR(xhprof, v)

ZEND_DECLARE_MODULE_GLOBALS(xhprof);

/**
 * ***********************
 * GLOBAL STATIC VARIABLES
 * ***********************
 */

/* Pointer to the original execute function */
ZEND_API void execute_ex_replace(zend_execute_data *execute_data);
ZEND_DLEXPORT void hp_execute_ex (zend_execute_data *execute_data);
static void (*_zend_execute_ex) (zend_execute_data *execute_data);

/* Pointer to the original compile function */
static zend_op_array * (*_zend_compile_file)(zend_file_handle *file_handle, int type);

/* Pointer to the original compile string function (used by eval) */
static zend_op_array * (*_zend_compile_string) (zval *source_string, char *filename);

/* Bloom filter for function names to be ignored */
#define INDEX_2_BYTE(index)  (index >> 3)
#define INDEX_2_BIT(index)   (1 << (index & 0x7));


/**
 * ****************************
 * STATIC FUNCTION DECLARATIONS
 * ****************************
 */
static void hp_register_constants(INIT_FUNC_ARGS);

static void hp_begin(long level, long xhprof_flags);
static void hp_stop();
static void hp_end();

static inline uint64 cycle_timer();
static double get_cpu_frequency();
static void clear_frequencies();

static void hp_free_the_free_list();
static hp_entry_t *hp_fast_alloc_hprof_entry();
static void hp_fast_free_hprof_entry(hp_entry_t *p);
static inline uint8 hp_inline_hash(char * str);
static void get_all_cpu_frequencies();
static long get_us_interval(struct timeval *start, struct timeval *end);
static void incr_us_interval(struct timeval *start, uint64 incr);

static void hp_get_ignored_functions_from_arg(zval *args);
static void hp_ignored_functions_filter_clear();
static void hp_ignored_functions_filter_init();

static inline zval *hp_zval_at_key(char *key, zval *values);
static inline char **hp_strings_in_zval(zval *values);
static inline void hp_array_del(char **name_array);

/* {{{ arginfo */
ZEND_BEGIN_ARG_INFO_EX(arginfo_xhprof_enable, 0, 0, 0)
	ZEND_ARG_INFO(0, flags)
	ZEND_ARG_INFO(0, options)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_xhprof_disable, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_xhprof_sample_enable, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_xhprof_sample_disable, 0)
ZEND_END_ARG_INFO()
/* }}} */

/**
 * *********************
 * FUNCTION PROTOTYPES
 * *********************
 */
int restore_cpu_affinity(cpu_set_t * prev_mask);
int bind_to_cpu(uint32 cpu_id);

/**
 * *********************
 * PHP EXTENSION GLOBALS
 * *********************
 */
/* List of functions implemented/exposed by xhprof */
zend_function_entry xhprof_functions[] =
{
	PHP_FE(xhprof_enable, arginfo_xhprof_enable)
	PHP_FE(xhprof_disable, arginfo_xhprof_disable)
	PHP_FE(xhprof_sample_enable, arginfo_xhprof_sample_enable)
	PHP_FE(xhprof_sample_disable, arginfo_xhprof_sample_disable)
	{ NULL, NULL, NULL }
};

/* Callback functions for the xhprof extension */
zend_module_entry xhprof_module_entry =
{
#if ZEND_MODULE_API_NO >= 20010901
	STANDARD_MODULE_HEADER,
#endif
	"xhprof", /* Name of the extension */
	xhprof_functions, /* List of functions exposed */
	PHP_MINIT(xhprof), /* Module init callback */
	PHP_MSHUTDOWN(xhprof), /* Module shutdown callback */
	PHP_RINIT(xhprof), /* Request init callback */
	PHP_RSHUTDOWN(xhprof), /* Request shutdown callback */
	PHP_MINFO(xhprof), /* Module info callback */
#if ZEND_MODULE_API_NO >= 20010901
	XHPROF_VERSION,
#endif
	STANDARD_MODULE_PROPERTIES
};

/* Init module */
#if defined(COMPILE_DL_PHPNG_XHPROF)
	ZEND_GET_MODULE(xhprof)
#endif

#ifdef ZTS
	ZEND_TSRMLS_CACHE_DEFINE();
#endif

PHP_INI_BEGIN()

/* output directory:
 * Currently this is not used by the extension itself.
 * But some implementations of iXHProfRuns interface might
 * choose to save/restore XHProf profiler runs in the
 * directory specified by this ini setting.
 */
PHP_INI_ENTRY("xhprof.output_dir", "", PHP_INI_ALL, NULL)

PHP_INI_END()

/**
 * **********************************
 * PHP EXTENSION FUNCTION DEFINITIONS
 * **********************************
 */

/**
 * Start XHProf profiling in hierarchical mode.
 *
 * @param  long $flags  flags for hierarchical mode
 * @return void
 * @author kannan
 */
PHP_FUNCTION(xhprof_enable)
{
	long xhprof_flags = 0; /* XHProf flags */
	zval *optional_array = NULL; /* optional array arg: for future use */

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|lz", &xhprof_flags,
			&optional_array) == FAILURE)
	{
		return;
	}

	hp_get_ignored_functions_from_arg(optional_array);

	hp_begin(XHPROF_MODE_HIERARCHICAL, xhprof_flags);
}

/**
 * Stops XHProf from profiling in hierarchical mode anymore and returns the
 * profile info.
 *
 * @param  void
 * @return array  hash-array of XHProf's profile info
 * @author kannan, hzhao
 */
PHP_FUNCTION(xhprof_disable)
{
	if (XHG(enabled))
	{
		hp_stop();
		ZVAL_COPY(return_value, &XHG(stats_count));
		if (Z_TYPE(XHG(stats_count)) != IS_UNDEF) {
			zval_ptr_dtor(&XHG(stats_count));
		}
		ZVAL_UNDEF(&XHG(stats_count));
	}
	/* else null is returned */
}

/**
 * Start XHProf profiling in sampling mode.
 *
 * @return void
 * @author cjiang
 */
PHP_FUNCTION(xhprof_sample_enable)
{
	long xhprof_flags = 0; /* XHProf flags */
	hp_get_ignored_functions_from_arg(NULL);
	hp_begin(XHPROF_MODE_SAMPLED, xhprof_flags);
}

/**
 * Stops XHProf from profiling in sampling mode anymore and returns the profile
 * info.
 *
 * @param  void
 * @return array  hash-array of XHProf's profile info
 * @author cjiang
 */
PHP_FUNCTION(xhprof_sample_disable)
{
	if (XHG(enabled))
	{
		hp_stop();
		ZVAL_COPY(return_value, &XHG(stats_count));
		zval_ptr_dtor(&XHG(stats_count));
		ZVAL_UNDEF(&XHG(stats_count));
	}
	/* else null is returned */
}

static inline void php_xhprof_init_globals(zend_xhprof_globals *xhg) {
	memset(xhg, 0, sizeof(zend_xhprof_globals));
}

/**
 * Module init callback.
 *
 * @author cjiang
 */
PHP_MINIT_FUNCTION(xhprof)
{

	int i;

	ZEND_INIT_MODULE_GLOBALS(xhprof, php_xhprof_init_globals, NULL);

	REGISTER_INI_ENTRIES();

	/*replace original zend_execute_ex*/
	zend_execute_ex = execute_ex_replace;

	hp_register_constants(INIT_FUNC_ARGS_PASSTHRU);

#if defined(DEBUG)
	/* To make it random number generator repeatable to ease testing. */
	srand(0);
#endif
	return SUCCESS;
}

/**
 * Module shutdown callback.
 */
PHP_MSHUTDOWN_FUNCTION(xhprof)
{
	UNREGISTER_INI_ENTRIES();

	return SUCCESS;
}

/**
 * Request init callback. Nothing to do yet!
 */
PHP_RINIT_FUNCTION(xhprof)
{
#if defined(ZTS) && defined(COMPILE_DL_PHPNG_XHPROF)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif

	/* Get the number of available logical CPUs. */
	XHG(cpu_num) = sysconf(_SC_NPROCESSORS_CONF);

	/* Get the cpu affinity mask. */
#ifndef __APPLE__
	if (GET_AFFINITY(0, sizeof(cpu_set_t), &XHG(prev_mask)) < 0)
	{
		perror("getaffinity");
		return FAILURE;
	}
#else
	CPU_ZERO(&XHG(prev_mask));
#endif

	hp_ignored_functions_filter_clear();

	return SUCCESS;
}

/**
 * Request shutdown callback. Stop profiling and return.
 */
PHP_RSHUTDOWN_FUNCTION(xhprof)
{
	hp_end();

	/* Make sure cpu_frequencies is free'ed. */
	clear_frequencies();

	/* free any remaining items in the free list */
	hp_free_the_free_list();

	return SUCCESS;
}

/**
 * Module info callback. Returns the xhprof version.
 */
PHP_MINFO_FUNCTION(xhprof)
{
	char buf[SCRATCH_BUF_LEN];
	char tmp[SCRATCH_BUF_LEN];
	int i;
	int len;

	php_info_print_table_start();
	php_info_print_table_header(2, "xhprof", XHPROF_VERSION);
	len = snprintf(buf, SCRATCH_BUF_LEN, "%d", XHG(cpu_num));
	buf[len] = 0;
	php_info_print_table_header(2, "CPU num", buf);

	if (XHG(cpu_frequencies))
	{
		/* Print available cpu frequencies here. */
		php_info_print_table_header(2, "CPU logical id", " Clock Rate (MHz) ");
		for (i = 0; i < XHG(cpu_num); ++i)
		{
			len = snprintf(buf, SCRATCH_BUF_LEN, " CPU %d ", i);
			buf[len] = 0;
			len = snprintf(tmp, SCRATCH_BUF_LEN, "%f",
					XHG(cpu_frequencies[i]));
			tmp[len] = 0;
			php_info_print_table_row(2, buf, tmp);
		}
	}

	php_info_print_table_end();
}


/**
 * ***************************************************
 * COMMON HELPER FUNCTION DEFINITIONS AND LOCAL MACROS
 * ***************************************************
 */

static void hp_register_constants(INIT_FUNC_ARGS)
{
	REGISTER_LONG_CONSTANT("XHPROF_FLAGS_NO_BUILTINS", XHPROF_FLAGS_NO_BUILTINS,
                           CONST_CS | CONST_PERSISTENT);

	REGISTER_LONG_CONSTANT("XHPROF_FLAGS_CPU", XHPROF_FLAGS_CPU,
                           CONST_CS | CONST_PERSISTENT);

	REGISTER_LONG_CONSTANT("XHPROF_FLAGS_MEMORY", XHPROF_FLAGS_MEMORY,
                           CONST_CS | CONST_PERSISTENT);
}

/**
 * A hash function to calculate a 8-bit hash code for a function name.
 * This is based on a small modification to 'zend_inline_hash_func' by summing
 * up all bytes of the ulong returned by 'zend_inline_hash_func'.
 *
 * @param str, char *, string to be calculated hash code for.
 *
 * @author cjiang
 */
static inline uint8 hp_inline_hash(char * str)
{
	ulong h = 5381;
	uint i = 0;
	uint8 res = 0;

	while (*str)
	{
		h += (h << 5);
		h ^= (ulong) *str++;
	}

	for (i = 0; i < sizeof(ulong); i++)
	{
		res += ((uint8 *) &h)[i];
	}
	return res;
}

/**
 * @phpng
 * Parse the list of ignored functions from the zval argument.
 *
 * @author mpal
 */
static void hp_get_ignored_functions_from_arg(zval *args)
{
	if (XHG(ignored_function_names))
	{
		hp_array_del(XHG(ignored_function_names));
	}
	if (args != NULL)
	{
		zval *zresult = NULL;

		zresult = hp_zval_at_key("ignored_functions", args);
		XHG(ignored_function_names) = hp_strings_in_zval(zresult);
	}
	else
	{
		XHG(ignored_function_names) = NULL;
	}
}

static inline zend_class_entry* hp_get_called_scope(const zend_execute_data *e) {
#if PHP_VERSION_ID < 70100
	return e->called_scope;
#else
	return zend_get_called_scope((zend_execute_data*) e);
#endif
}

/**
 * @phpng
 * Clear filter for functions which may be ignored during profiling.
 *
 * @author mpal
 */
static void hp_ignored_functions_filter_clear()
{
	memset(XHG(ignored_function_filter), 0, XHPROF_IGNORED_FUNCTION_FILTER_SIZE);
}

/*
 * @phpng
 * Initialize filter for ignored functions using bit vector.
 *
 * @author mpal
 */
static void hp_ignored_functions_filter_init()
{
	if (XHG(ignored_function_names) != NULL)
	{
		int i = 0;
		for (; XHG(ignored_function_names[i]) != NULL; i++)
		{
			char *str = XHG(ignored_function_names[i]);
			uint8 hash = hp_inline_hash(str);
			int idx = INDEX_2_BYTE(hash);
			XHG(ignored_function_filter[idx]) |= INDEX_2_BIT(hash);
		}
	}
}

/**
 * @phpng
 * Check if function collides in filter of functions to be ignored.
 *
 * @author mpal
 */
int hp_ignored_functions_filter_collision(uint8 hash)
{
	uint8 mask = INDEX_2_BIT(hash);
	return XHG(ignored_function_filter[INDEX_2_BYTE(hash)]) & mask;
}

/**
 * Initialize profiler state
 *
 * @author kannan, veeve
 */
void hp_init_profiler_state(int level)
{
	/* Setup globals */
	if (!XHG(ever_enabled))
	{
		XHG(ever_enabled) = 1;
		XHG(entries) = NULL;
	}
	XHG(profiler_level) = (int) level;
	if(Z_TYPE(XHG(stats_count)) == IS_UNDEF){
		array_init(&XHG(stats_count));
	}
	/* NOTE(cjiang): some fields such as cpu_frequencies take relatively longer
	 * to initialize, (5 milisecond per logical cpu right now), therefore we
	 * calculate them lazily. */
	if (XHG(cpu_frequencies) == NULL)
	{
		get_all_cpu_frequencies();
		restore_cpu_affinity(&XHG(prev_mask));
	}

	/* bind to a random cpu so that we can use rdtsc instruction. */
	bind_to_cpu((int) (rand() % XHG(cpu_num)));

	/* Call current mode's init cb */
	XHG(mode_cb).init_cb();

	/* Set up filter of functions which may be ignored during profiling */
	hp_ignored_functions_filter_init();
}

/**
 * Cleanup profiler state
 *
 * @author kannan, veeve
 */
void hp_clean_profiler_state()
{
	/* Call current mode's exit cb */
	XHG(mode_cb).exit_cb();

	/* Clear globals */
	if (Z_TYPE(XHG(stats_count)) != IS_UNDEF) {
		zval_ptr_dtor(&XHG(stats_count));
		ZVAL_UNDEF(&XHG(stats_count));
	}

	XHG(entries) = NULL;
	XHG(profiler_level) = 1;
	XHG(ever_enabled) = 0;

	/* Delete the array storing ignored function names */
	hp_array_del(XHG(ignored_function_names));
	XHG(ignored_function_names) = NULL;
}

/*
 * Start profiling - called just before calling the actual function
 * NOTE:  PLEASE MAKE SURE TSRMLS_CC IS AVAILABLE IN THE CONTEXT
 *        OF THE FUNCTION WHERE THIS MACRO IS CALLED.
 *        TSRMLS_CC CAN BE MADE AVAILABLE VIA TSRMLS_DC IN THE
 *        CALLING FUNCTION OR BY CALLING TSRMLS_FETCH()
 *        TSRMLS_FETCH() IS RELATIVELY EXPENSIVE.
 */
#define BEGIN_PROFILING(entries, symbol, profile_curr)                        \
	do {                                                                      \
		/* Use a hash code to filter most of the string comparisons. */       \
		uint8 hash_code  = hp_inline_hash(symbol);                            \
		profile_curr = !hp_ignore_entry(hash_code, symbol);                   \
		if (profile_curr) {                                                   \
			hp_entry_t *cur_entry = hp_fast_alloc_hprof_entry();              \
			(cur_entry)->hash_code = hash_code;                               \
			(cur_entry)->name_hprof = symbol;                                 \
			(cur_entry)->prev_hprof = (*(entries));                           \
			/* Call the universal callback */                                 \
			hp_mode_common_beginfn((entries), (cur_entry));                   \
			/* Call the mode's beginfn callback */                            \
			XHG(mode_cb).begin_fn_cb((entries), (cur_entry));           \
			/* Update entries linked list */                                  \
			(*(entries)) = (cur_entry);                                       \
		}                                                                     \
	} while (0)

/*
 * Stop profiling - called just after calling the actual function
 * NOTE:  PLEASE MAKE SURE TSRMLS_CC IS AVAILABLE IN THE CONTEXT
 *        OF THE FUNCTION WHERE THIS MACRO IS CALLED.
 *        TSRMLS_CC CAN BE MADE AVAILABLE VIA TSRMLS_DC IN THE
 *        CALLING FUNCTION OR BY CALLING TSRMLS_FETCH()
 *        TSRMLS_FETCH() IS RELATIVELY EXPENSIVE.
 */
#define END_PROFILING(entries, profile_curr)                                  \
	do {                                                                      \
		if (profile_curr) {                                                   \
			hp_entry_t *cur_entry;                                            \
			/* Call the mode's endfn callback. */                             \
			/* NOTE(cjiang): we want to call this 'end_fn_cb' before */       \
			/* 'hp_mode_common_endfn' to avoid including the time in */       \
			/* 'hp_mode_common_endfn' in the profiling results.      */       \
			XHG(mode_cb).end_fn_cb((entries));                          \
			cur_entry = (*(entries));                                         \
			/* Call the universal callback */                                 \
			hp_mode_common_endfn((entries), (cur_entry));                     \
			/* Free top entry and update entries linked list */               \
			(*(entries)) = (*(entries))->prev_hprof;                          \
			hp_fast_free_hprof_entry(cur_entry);                              \
		}                                                                     \
	} while (0)


/**
 * Returns formatted function name
 *
 * @param  entry        hp_entry
 * @param  result_buf   ptr to result buf
 * @param  result_len   max size of result buf
 * @return total size of the function name returned in result_buf
 * @author veeve
 */
size_t hp_get_entry_name(hp_entry_t *entry, char *result_buf, size_t result_len)
{

	/* Validate result_len */
	if (result_len <= 1)
	{
		/* Insufficient result_bug. Bail! */
		return 0;
	}

	/* Add '@recurse_level' if required */
	/* NOTE:  Dont use snprintf's return val as it is compiler dependent */
	if (entry->rlvl_hprof)
	{
		snprintf(result_buf, result_len, "%s@%d", entry->name_hprof, entry->rlvl_hprof);
	}
	else
	{
		snprintf(result_buf, result_len, "%s", entry->name_hprof);
	}

	/* Force null-termination at MAX */
	result_buf[result_len - 1] = 0;

	return strlen(result_buf);
}

/**
 * Check if this entry should be ignored, first with a conservative Bloomish
 * filter then with an exact check against the function names.
 *
 * @author mpal
 */
int hp_ignore_entry_work(uint8 hash_code, char *curr_func)
{
	int ignore = 0;
	if (hp_ignored_functions_filter_collision(hash_code))
	{
		int i = 0;
		for (; XHG(ignored_function_names[i]) != NULL; i++)
		{
			if (!strcmp(curr_func, XHG(ignored_function_names[i])))
			{
				ignore++;
				break;
			}
		}
	}

	return ignore;
}

inline int hp_ignore_entry(uint8 hash_code, char *curr_func)
{
	/* First check if ignoring functions is enabled */
	return XHG(ignored_function_names) != NULL && hp_ignore_entry_work(hash_code, curr_func);
}

/**
 * Build a caller qualified name for a callee.
 *
 * For example, if A() is caller for B(), then it returns "A==>B".
 * Recursive invokations are denoted with @<n> where n is the recursion
 * depth.
 *
 * For example, "foo==>foo@1", and "foo@2==>foo@3" are examples of direct
 * recursion. And  "bar==>foo@1" is an example of an indirect recursive
 * call to foo (implying the foo() is on the call stack some levels
 * above).
 *
 * @author kannan, veeve
 */
size_t hp_get_function_stack(hp_entry_t *entry, int level, char *result_buf, size_t result_len)
{
	size_t len = 0;

	/* End recursion if we dont need deeper levels or we dont have any deeper
	 * levels */
	if (!entry->prev_hprof || (level <= 1))
	{
		return hp_get_entry_name(entry, result_buf, result_len);
	}

	/* Take care of all ancestors first */
	len = hp_get_function_stack(entry->prev_hprof, level - 1, result_buf, result_len);

	/* Append the delimiter */
# define    HP_STACK_DELIM        "==>"
# define    HP_STACK_DELIM_LEN    (sizeof(HP_STACK_DELIM) - 1)

	if (result_len < (len + HP_STACK_DELIM_LEN))
	{
		/* Insufficient result_buf. Bail out! */
		return len;
	}

	/* Add delimiter only if entry had ancestors */
	if (len)
	{
		strncat(result_buf + len, HP_STACK_DELIM, result_len - len);
		len += HP_STACK_DELIM_LEN;
	}

# undef     HP_STACK_DELIM_LEN
# undef     HP_STACK_DELIM

	/* Append the current function name */
	return len + hp_get_entry_name(entry, result_buf + len, result_len - len);
}

/**
 * Takes an input of the form /a/b/c/d/foo.php and returns
 * a pointer to one-level directory and basefile name
 * (d/foo.php) in the same string.
 */
static const char *hp_get_base_filename(const char *filename)
{
	const char *ptr;
	int found = 0;

	if (!filename)
		return "";

	/* reverse search for "/" and return a ptr to the next char */
	for (ptr = filename + strlen(filename) - 1; ptr >= filename; ptr--)
	{
		if (*ptr == '/')
		{
			found++;
		}
		if (found == 2)
		{
			return ptr + 1;
		}
	}

	/* no "/" char found, so return the whole string */
	return filename;
}

/**
 * Get the name of the current function. The name is qualified with
 * the class name if the function is in a class.
 *
 * @author kannan, hzhao
 */
static char *hp_opcode_get_function_name(const zend_execute_data const *execute_data)
{
	const char *cls  = NULL;
	const char *func = NULL;
	char * ret = NULL;
	int len;
	zend_function *curr_func;
	zend_internal_function internal_function;
	switch(execute_data->opline->opcode)
	{
	case ZEND_STRLEN:
		if(XHG(xhprof_flags) & XHPROF_FLAGS_NO_BUILTINS){
			return NULL;
		}
		return estrdup("strlen");
	case ZEND_DO_ICALL:
	//case ZEND_DO_FCALL:
	//case ZEND_DO_UCALL:
	//case ZEND_DO_FCALL_BY_NAME:
		{
			switch(execute_data->call->func->type)
			{
			case ZEND_INTERNAL_FUNCTION:
				if(XHG(xhprof_flags) & XHPROF_FLAGS_NO_BUILTINS){
					return NULL;
				}
				internal_function = execute_data->call->func->internal_function;
				cls  = internal_function.scope ? internal_function.scope->name->val : NULL;
				func = internal_function.function_name->val;
				len  = internal_function.function_name->len + 1;
				break;
			case ZEND_USER_FUNCTION:
				curr_func = execute_data->call->func;
				cls  = curr_func->common.scope ? curr_func->common.scope->name->val : (
						hp_get_called_scope(execute_data) ? hp_get_called_scope(execute_data)->name->val : NULL);
				func = curr_func->common.function_name->val;
				len  = curr_func->common.function_name->len + 1;
				break;
			}
			if (cls)
			{
				len = strlen(cls) + strlen(func) + 10;
				ret = (char*) emalloc(len);
				snprintf(ret, len, "%s::%s", cls, func);
			}
			else
			{
				ret = (char*) emalloc(len);
				snprintf(ret, len, "%s", func);
			}
			return ret;
		}
		break;
	}
	return NULL;
}

static char *hp_get_function_name(zend_execute_data * execute_data)
{
	zend_execute_data *data;
	char *ret = NULL;
	int len;
	const char * cls;
	const char * func;
	zend_function *curr_func;
	uint32_t curr_op;
	int add_filename;
	const zend_op *opline;

	data = EG(current_execute_data);

	if (data)
	{
		/* shared meta data for function on the call stack */
		curr_func = data->func;
		/* extract function name from the meta info */
		if (curr_func->common.function_name)
		{
			/* previously, the order of the tests in the "if" below was
			 * flipped, leading to incorrect function names in profiler
			 * reports. When a method in a super-type is invoked the
			 * profiler should qualify the function name with the super-type
			 * class name (not the class name based on the run-time type
			 * of the object.
			 */
			func = curr_func->common.function_name->val;
			len  = curr_func->common.function_name->len + 1;
			cls = curr_func->common.scope ?
					curr_func->common.scope->name->val :
					(hp_get_called_scope(data) ?
							hp_get_called_scope(data)->name->val : NULL);
			if (cls)
			{
				len = strlen(cls) + strlen(func) + 10;
				ret = (char*) emalloc(len);
				snprintf(ret, len, "%s::%s", cls, func);
			}
			else
			{
				ret = (char*) emalloc(len);
				snprintf(ret, len, "%s", func);
			}
		}
		else
		{
			if (data->prev_execute_data)
			{
				opline  = data->prev_execute_data->opline;
			}
			else
			{
				opline  = data->opline;
			}
			switch (opline->extended_value)
			{
			case ZEND_EVAL:
				func = "eval";
				break;
			case ZEND_INCLUDE:
				func = "include";
				add_filename = 1;
				break;
			case ZEND_REQUIRE:
				func = "require";
				add_filename = 1;
				break;
			case ZEND_INCLUDE_ONCE:
				func = "include_once";
				add_filename = 1;
				break;
			case ZEND_REQUIRE_ONCE:
				func = "require_once";
				add_filename = 1;
				break;
			default:
				func = NULL;
				break;
			}

			/* For some operations, we'll add the filename as part of the function
			 * name to make the reports more useful. So rather than just "include"
			 * you'll see something like "run_init::foo.php" in your reports.
			 */
			if (func)
			{
				if (add_filename)
				{
					const char *filename;
					filename = hp_get_base_filename((curr_func->op_array).filename->val);
					len      = strlen("run_init") + strlen(filename) + 3;
					ret      = (char *)emalloc(len);
					snprintf(ret, len, "run_init::%s", filename);
				}
				else
				{
					ret = estrdup(func);
				}
			}
		}
	}
	return ret;
}

/**
 * Get the name of the current function. The name is qualified with
 * the class name if the function is in a class.
 *
 * @author kannan, hzhao
 */


/**
 * Free any items in the free list.
 */
static void hp_free_the_free_list()
{
	hp_entry_t *p = XHG(entry_free_list);
	hp_entry_t *cur;

	while (p)
	{
		cur = p;
		p = p->prev_hprof;
		free(cur);
	}
}

/**
 * Fast allocate a hp_entry_t structure. Picks one from the
 * free list if available, else does an actual allocate.
 *
 * Doesn't bother initializing allocated memory.
 *
 * @author kannan
 */
static hp_entry_t *hp_fast_alloc_hprof_entry()
{
	hp_entry_t *p;

	p = XHG(entry_free_list);

	if (p)
	{
		XHG(entry_free_list) = p->prev_hprof;
		return p;
	}
	else
	{
		return (hp_entry_t *) malloc(sizeof(hp_entry_t));
	}
}

/**
 * Fast free a hp_entry_t structure. Simply returns back
 * the hp_entry_t to a free list and doesn't actually
 * perform the free.
 *
 * @author kannan
 */
static void hp_fast_free_hprof_entry(hp_entry_t *p)
{

	/* we use/overload the prev_hprof field in the structure to link entries in
	 * the free list. */
	p->prev_hprof = XHG(entry_free_list);
	XHG(entry_free_list) = p;
}

/**
 * @phpng
 * Increment the count of the given stat with the given count
 * If the stat was not set before, inits the stat to the given count
 *
 * @param  zval *counts   Zend hash table pointer
 * @param  char *name     Name of the stat
 * @param  long  count    Value of the stat to incr by
 * @return void
 * @author kannan
 */
void hp_inc_count(zval *counts, char *name, long count)
{
	HashTable *ht;
	zval * data;

	if (!counts || Z_TYPE_P(counts) != IS_ARRAY){
		return;
	}
	ht = Z_ARRVAL_P(counts);
	if (!ht){
		return;
	}

	if ((data = zend_hash_str_find(ht, name, strlen(name))) != NULL)
	{
		ZVAL_LONG(data, Z_LVAL_P(data)+count)
	}
	else
	{
		add_assoc_long(counts, name, count);
	}

}

/**
 * Looksup the hash table for the given symbol
 * Initializes a new array() if symbol is not present
 *
 * @author kannan, veeve
 */
int hp_hash_lookup(zval ** val, char *symbol)
{
	HashTable *ht;
	zval * tmp;

	/* Bail if something is goofy */
	if (Z_TYPE_P(&XHG(stats_count)) == IS_UNDEF || !(ht = HASH_OF(&XHG(stats_count))))
	{
		return -1;
	}

	/* Lookup our hash table */
	if ((tmp = zend_hash_str_find(ht, symbol, strlen(symbol))) != NULL)
	{
		/* Symbol already exists */
		*val = tmp;
	}
	else
	{
		/* Add symbol to hash table */
		array_init(*val);
		add_assoc_zval(&XHG(stats_count), symbol, *val);
	}
	return 0;
}

/**
 * Truncates the given timeval to the nearest slot begin, where
 * the slot size is determined by intr
 *
 * @param  tv       Input timeval to be truncated in place
 * @param  intr     Time interval in microsecs - slot width
 * @return void
 * @author veeve
 */
void hp_trunc_time(struct timeval *tv, uint64 intr)
{
	uint64 time_in_micro;

	/* Convert to microsecs and trunc that first */
	time_in_micro = (tv->tv_sec * 1000000) + tv->tv_usec;
	time_in_micro /= intr;
	time_in_micro *= intr;

	/* Update tv */
	tv->tv_sec = (time_in_micro / 1000000);
	tv->tv_usec = (time_in_micro % 1000000);
}

/**
 * Sample the stack. Add it to the stats_count global.
 *
 * @param  tv            current time
 * @param  entries       func stack as linked list of hp_entry_t
 * @return void
 * @author veeve
 */
void hp_sample_stack(hp_entry_t **entries)
{
	char key[SCRATCH_BUF_LEN];
	char symbol[SCRATCH_BUF_LEN * 1000];

	/* Build key */
	snprintf(key, sizeof(key), "%d.%06d", XHG(last_sample_time).tv_sec,
			 XHG(last_sample_time).tv_usec);

	/* Init stats in the global stats_count hashtable */
	hp_get_function_stack(*entries, INT_MAX, symbol, sizeof(symbol));

	add_assoc_string(&XHG(stats_count), key, symbol);
	return;
}

/**
 * Checks to see if it is time to sample the stack.
 * Calls hp_sample_stack() if its time.
 *
 * @param  entries        func stack as linked list of hp_entry_t
 * @param  last_sample    time the last sample was taken
 * @param  sampling_intr  sampling interval in microsecs
 * @return void
 * @author veeve
 */
void hp_sample_check(hp_entry_t **entries)
{
	/* Validate input */
	if (!entries || !(*entries))
	{
		return;
	}

	/* See if its time to sample.  While loop is to handle a single function
	 * taking a long time and passing several sampling intervals. */
	while ((cycle_timer() - XHG(last_sample_tsc)) > XHG(sampling_interval_tsc))
	{

		/* bump last_sample_tsc */
		XHG(last_sample_tsc) += XHG(sampling_interval_tsc);

		/* bump last_sample_time - HAS TO BE UPDATED BEFORE calling hp_sample_stack */
		incr_us_interval(&XHG(last_sample_time),
				XHPROF_SAMPLING_INTERVAL);

		/* sample the stack */
		hp_sample_stack(entries);
	}

	return;
}


/**
 * ***********************
 * High precision timer related functions.
 * ***********************
 */

/**
 * Get time stamp counter (TSC) value via 'rdtsc' instruction.
 *
 * @return 64 bit unsigned integer
 * @author cjiang
 */
inline uint64 cycle_timer()
{
	uint32 __a, __d;
	uint64 val;
	asm volatile("rdtsc" : "=a" (__a), "=d" (__d));
	(val) = ((uint64) __a) | (((uint64) __d) << 32);
	return val;
}

/**
 * Bind the current process to a specified CPU. This function is to ensure that
 * the OS won't schedule the process to different processors, which would make
 * values read by rdtsc unreliable.
 *
 * @param uint32 cpu_id, the id of the logical cpu to be bound to.
 * @return int, 0 on success, and -1 on failure.
 *
 * @author cjiang
 */
int bind_to_cpu(uint32 cpu_id)
{
	cpu_set_t new_mask;

	CPU_ZERO(&new_mask);
	CPU_SET(cpu_id, &new_mask);

	if (SET_AFFINITY(0, sizeof(cpu_set_t), &new_mask) < 0)
	{
		perror("setaffinity");
		return -1;
	}

	/* record the cpu_id the process is bound to. */
	XHG(cur_cpu_id) = cpu_id;

	return 0;
}

/**
 * Get time delta in microseconds.
 */
static long get_us_interval(struct timeval *start, struct timeval *end)
{
	return (((end->tv_sec - start->tv_sec) * 1000000) + (end->tv_usec - start->tv_usec));
}

/**
 * Incr time with the given microseconds.
 */
static void incr_us_interval(struct timeval *start, uint64 incr)
{
	incr += (start->tv_sec * 1000000 + start->tv_usec);
	start->tv_sec = incr / 1000000;
	start->tv_usec = incr % 1000000;
	return;
}

/**
 * Convert from TSC counter values to equivalent microseconds.
 *
 * @param uint64 count, TSC count value
 * @param double cpu_frequency, the CPU clock rate (MHz)
 * @return 64 bit unsigned integer
 *
 * @author cjiang
 */
inline double get_us_from_tsc(uint64 count, double cpu_frequency)
{
	return count / cpu_frequency;
}

/**
 * Convert microseconds to equivalent TSC counter ticks
 *
 * @param uint64 microseconds
 * @param double cpu_frequency, the CPU clock rate (MHz)
 * @return 64 bit unsigned integer
 *
 * @author veeve
 */
inline uint64 get_tsc_from_us(uint64 usecs, double cpu_frequency)
{
	return (uint64) (usecs * cpu_frequency);
}

/**
 * This is a microbenchmark to get cpu frequency the process is running on. The
 * returned value is used to convert TSC counter values to microseconds.
 *
 * @return double.
 * @author cjiang
 */
static double get_cpu_frequency()
{
	struct timeval start;
	struct timeval end;

	if (gettimeofday(&start, 0))
	{
		perror("gettimeofday");
		return 0.0;
	}
	uint64 tsc_start = cycle_timer();
	/* Sleep for 5 miliseconds. Comparaing with gettimeofday's  few microseconds
	 * execution time, this should be enough. */
	usleep(5000);
	if (gettimeofday(&end, 0))
	{
		perror("gettimeofday");
		return 0.0;
	}
	uint64 tsc_end = cycle_timer();
	return (tsc_end - tsc_start) * 1.0 / (get_us_interval(&start, &end));
}

/**
 * Calculate frequencies for all available cpus.
 *
 * @author cjiang
 */
static void get_all_cpu_frequencies()
{
	int id;
	double frequency;

	XHG(cpu_frequencies) = malloc(sizeof(double) * XHG(cpu_num));
	if (XHG(cpu_frequencies) == NULL)
	{
		return;
	}

	/* Iterate over all cpus found on the machine. */
	for (id = 0; id < XHG(cpu_num); ++id)
	{
		/* Only get the previous cpu affinity mask for the first call. */
		if (bind_to_cpu(id))
		{
			clear_frequencies();
			return;
		}

		/* Make sure the current process gets scheduled to the target cpu. This
		 * might not be necessary though. */
		usleep(0);

		frequency = get_cpu_frequency();
		if (frequency == 0.0)
		{
			clear_frequencies();
			return;
		}
		XHG(cpu_frequencies[id]) = frequency;
	}
}

/**
 * Restore cpu affinity mask to a specified value. It returns 0 on success and
 * -1 on failure.
 *
 * @param cpu_set_t * prev_mask, previous cpu affinity mask to be restored to.
 * @return int, 0 on success, and -1 on failure.
 *
 * @author cjiang
 */
int restore_cpu_affinity(cpu_set_t * prev_mask)
{
	if (SET_AFFINITY(0, sizeof(cpu_set_t), prev_mask) < 0)
	{
		perror("restore setaffinity");
		return -1;
	}
	/* default value ofor cur_cpu_id is 0. */
	XHG(cur_cpu_id) = 0;
	return 0;
}

/**
 * Reclaim the memory allocated for cpu_frequencies.
 *
 * @author cjiang
 */
static void clear_frequencies()
{
	if (XHG(cpu_frequencies))
	{
		free(XHG(cpu_frequencies));
		XHG(cpu_frequencies) = NULL;
	}
	restore_cpu_affinity(&XHG(prev_mask));
}


/**
 * ***************************
 * XHPROF DUMMY CALLBACKS
 * ***************************
 */
void hp_mode_dummy_init_cb()
{
}

void hp_mode_dummy_exit_cb()
{
}

void hp_mode_dummy_beginfn_cb(hp_entry_t **entries, hp_entry_t *current)
{
}

void hp_mode_dummy_endfn_cb(hp_entry_t **entries)
{
}


/**
 * ****************************
 * XHPROF COMMON CALLBACKS
 * ****************************
 */
/**
 * XHPROF universal begin function.
 * This function is called for all modes before the
 * mode's specific begin_function callback is called.
 *
 * @param  hp_entry_t **entries  linked list (stack)
 *                                  of hprof entries
 * @param  hp_entry_t  *current  hprof entry for the current fn
 * @return void
 * @author kannan, veeve
 */
void hp_mode_common_beginfn(hp_entry_t **entries, hp_entry_t *current)
{
	hp_entry_t *p;
	/* This symbol's recursive level */
	int recurse_level = 0;

	if (XHG(func_hash_counters)[current->hash_code] > 0)
	{
		/* Find this symbols recurse level */
		for (p = (*entries); p; p = p->prev_hprof)
		{
			if (!strcmp(current->name_hprof, p->name_hprof))
			{
				recurse_level = (p->rlvl_hprof) + 1;
				break;
			}
		}
	}
	XHG(func_hash_counters)[current->hash_code]++;

	/* Init current function's recurse level */
	current->rlvl_hprof = recurse_level;
}

/**
 * XHPROF universal end function.  This function is called for all modes after
 * the mode's specific end_function callback is called.
 *
 * @param  hp_entry_t **entries  linked list (stack) of hprof entries
 * @return void
 * @author kannan, veeve
 */
void hp_mode_common_endfn(hp_entry_t **entries, hp_entry_t *current)
{
	XHG(func_hash_counters)[current->hash_code]--;
}


/**
 * *********************************
 * XHPROF INIT MODULE CALLBACKS
 * *********************************
 */
/**
 * XHPROF_MODE_SAMPLED's init callback
 *
 * @author veeve
 */
void hp_mode_sampled_init_cb()
{
	struct timeval now;
	uint64 truncated_us;
	uint64 truncated_tsc;
	double cpu_freq = XHG(cpu_frequencies)[XHG(cur_cpu_id)];

	/* Init the last_sample in tsc */
	XHG(last_sample_tsc) = cycle_timer();

	/* Find the microseconds that need to be truncated */
	gettimeofday(&XHG(last_sample_time), 0);
	now = XHG(last_sample_time);
	hp_trunc_time(&XHG(last_sample_time), XHPROF_SAMPLING_INTERVAL);

	/* Subtract truncated time from last_sample_tsc */
	truncated_us = get_us_interval(&XHG(last_sample_time), &now);
	truncated_tsc = get_tsc_from_us(truncated_us, cpu_freq);
	if (XHG(last_sample_tsc) > truncated_tsc)
	{
		/* just to be safe while subtracting unsigned ints */
		XHG(last_sample_tsc) -= truncated_tsc;
	}

	/* Convert sampling interval to ticks */
	XHG(sampling_interval_tsc) = get_tsc_from_us(XHPROF_SAMPLING_INTERVAL, cpu_freq);
}


/**
 * ************************************
 * XHPROF BEGIN FUNCTION CALLBACKS
 * ************************************
 */

/**
 * XHPROF_MODE_HIERARCHICAL's begin function callback
 *
 * @author kannan
 */
void hp_mode_hier_beginfn_cb(hp_entry_t **entries, hp_entry_t *current)
{
	/* Get start tsc counter */
	current->tsc_start = cycle_timer();

	/* Get CPU usage */
	if (XHG(xhprof_flags) & XHPROF_FLAGS_CPU)
	{
		getrusage(RUSAGE_SELF, &(current->ru_start_hprof));
	}

	/* Get memory usage */
	if (XHG(xhprof_flags) & XHPROF_FLAGS_MEMORY)
	{
		current->mu_start_hprof = zend_memory_usage(0);
		current->pmu_start_hprof = zend_memory_peak_usage(0);
	}
}


/**
 * XHPROF_MODE_SAMPLED's begin function callback
 *
 * @author veeve
 */
void hp_mode_sampled_beginfn_cb(hp_entry_t **entries, hp_entry_t *current)
{
	/* See if its time to take a sample */
	hp_sample_check(entries);
}


/**
 * **********************************
 * XHPROF END FUNCTION CALLBACKS
 * **********************************
 */

/**
 * XHPROF shared end function callback
 *
 * @author kannan
 */
int hp_mode_shared_endfn_cb(zval * val, hp_entry_t *top, char *symbol)
{
	uint64 tsc_end;

	/* Get end tsc counter */
	tsc_end = cycle_timer();

	/* Get the stat array */
	if (hp_hash_lookup(&val, symbol) != 0)
	{
		return -1;
	}

	/* Bump stats in the counts hashtable */
	hp_inc_count(val, "ct", 1);

	hp_inc_count(val, "wt", get_us_from_tsc(tsc_end - top->tsc_start,
				 XHG(cpu_frequencies)[XHG(cur_cpu_id)]));
	return 0;
}

/**
 * XHPROF_MODE_HIERARCHICAL's end function callback
 *
 * @author kannan
 */
void hp_mode_hier_endfn_cb(hp_entry_t **entries)
{
	hp_entry_t *top = (*entries);
	zval counts;
	struct rusage ru_end;
	char symbol[SCRATCH_BUF_LEN];
	long int mu_end;
	long int pmu_end;

	/* Get the stat array */
	hp_get_function_stack(top, 2, symbol, sizeof(symbol));
	if (hp_mode_shared_endfn_cb(&counts, top, symbol) != 0)
	{
		return;
	}

	if (XHG(xhprof_flags) & XHPROF_FLAGS_CPU)
	{
		/* Get CPU usage */
		getrusage(RUSAGE_SELF, &ru_end);

		/* Bump CPU stats in the counts hashtable */
		hp_inc_count(&counts, "cpu", (get_us_interval(&(top->ru_start_hprof.ru_utime),
					 &(ru_end.ru_utime)) + get_us_interval(&(top->ru_start_hprof.ru_stime),
					 &(ru_end.ru_stime))));
	}

	if (XHG(xhprof_flags) & XHPROF_FLAGS_MEMORY)
	{
		/* Get Memory usage */
		mu_end = zend_memory_usage(0);
		pmu_end = zend_memory_peak_usage(0);

		/* Bump Memory stats in the counts hashtable */
		hp_inc_count(&counts, "mu", mu_end - top->mu_start_hprof);
		hp_inc_count(&counts, "pmu", pmu_end - top->pmu_start_hprof);
	}
}

/**
 * XHPROF_MODE_SAMPLED's end function callback
 *
 * @author veeve
 */
void hp_mode_sampled_endfn_cb(hp_entry_t **entries)
{
	/* See if its time to take a sample */
	hp_sample_check(entries);
}


/**
 * ***************************
 * PHP EXECUTE/COMPILE PROXIES
 * ***************************
 */

ZEND_API void execute_ex_replace(zend_execute_data *execute_data)
{
	char * funcName = NULL;
	int hp_profile_flag = 1;
	while(1){
		int ret;
		if(XHG(enabled)){
			if(NULL != (funcName = hp_opcode_get_function_name(execute_data))){
				BEGIN_PROFILING(&XHG(entries), funcName, hp_profile_flag);
			}
		}
		ret = zend_vm_call_opcode_handler(execute_data);
		if(funcName)
		{
			if(XHG(enabled))
			{
				// main() can't be out here
				if (XHG(entries) && XHG(entries)->prev_hprof) {
					END_PROFILING(&XHG(entries), hp_profile_flag);
				}
			}
			efree(funcName);
		}
		if (ret != 0) {
			if (ret < 0) {
				return;
			} else {
				execute_data = EG(current_execute_data);
			}
		}
	}
	zend_error_noreturn(E_CORE_ERROR, "Arrived at end of main loop which shouldn't happen");
}

/**
 * XHProf enable replaced the zend_execute function with this
 * new execute function. We can do whatever profiling we need to
 * before and after calling the actual zend_execute().
 *
 * @author hzhao, kannan
 */
ZEND_DLEXPORT void hp_execute_ex (zend_execute_data *execute_data) {

  if(!XHG(enabled)){
	  _zend_execute_ex(execute_data);
	  return;
  }

  char          *func = NULL;
  int hp_profile_flag = 1;

  func = hp_get_function_name(execute_data);
  if (!func) {
    _zend_execute_ex(execute_data);
    return;
  }

  BEGIN_PROFILING(&XHG(entries), func, hp_profile_flag);
  _zend_execute_ex(execute_data);
  if (XHG(entries)) {
    END_PROFILING(&XHG(entries), hp_profile_flag);
  }
  efree(func);
}

/**
 * Proxy for zend_compile_file(). Used to profile PHP compilation time.
 *
 * @author kannan, hzhao
 */

ZEND_DLEXPORT zend_op_array* hp_compile_file(zend_file_handle *file_handle, int type)
{
	const char *filename;
	char *func;
	int len;
	zend_op_array *ret;
	int hp_profile_flag = 1;

	filename = hp_get_base_filename(file_handle->filename);
	len = sizeof("load") - 1 + strlen(filename) + 3;
	func = (char *) emalloc(len);
	snprintf(func, len, "load::%s", filename);

	BEGIN_PROFILING(&XHG(entries), func, hp_profile_flag);
	ret = _zend_compile_file(file_handle, type);
	if (XHG(entries))
	{
		END_PROFILING(&XHG(entries), hp_profile_flag);
	}

	efree(func);
	return ret;
}

/**
 * Proxy for zend_compile_string(). Used to profile PHP eval compilation time.
 */
ZEND_DLEXPORT zend_op_array* hp_compile_string(zval *source_string, char *filename)
{

	char *func;
	int len;
	zend_op_array *ret;
	int hp_profile_flag = 1;

	len = sizeof("eval") -1 + strlen(filename) + 3;
	func = (char *) emalloc(len);
	snprintf(func, len, "eval::%s", filename);

	BEGIN_PROFILING(&XHG(entries), func, hp_profile_flag);
	ret = _zend_compile_string(source_string, filename);
	if (XHG(entries))
	{
		END_PROFILING(&XHG(entries), hp_profile_flag);
	}

	efree(func);
	return ret;
}

/**
 * **************************
 * MAIN XHPROF CALLBACKS
 * **************************
 */

/**
 * This function gets called once when xhprof gets enabled.
 * It replaces all the functions like zend_execute,
 * etc that needs to be instrumented with their corresponding proxies.
 */
static void hp_begin(long level, long xhprof_flags)
{
	if (!XHG(enabled))
	{
		int hp_profile_flag = 1;

		XHG(enabled) = 1;
		XHG(xhprof_flags) = (uint32) xhprof_flags;

		 /* Replace zend_compile with our proxy */
		_zend_compile_file = zend_compile_file;
		zend_compile_file  = hp_compile_file;

		/* Replace zend_compile_string with our proxy */
		_zend_compile_string = zend_compile_string;
		zend_compile_string  = hp_compile_string;

		/*init the execute pointer*/
		_zend_execute_ex = zend_execute_ex;
		zend_execute_ex  = hp_execute_ex;

		/* Initialize with the dummy mode first Having these dummy callbacks saves
		 * us from checking if any of the callbacks are NULL everywhere. */
		XHG(mode_cb).init_cb = hp_mode_dummy_init_cb;
		XHG(mode_cb).exit_cb = hp_mode_dummy_exit_cb;
		XHG(mode_cb).begin_fn_cb = hp_mode_dummy_beginfn_cb;
		XHG(mode_cb).end_fn_cb = hp_mode_dummy_endfn_cb;

		/* Register the appropriate callback functions Override just a subset of
		 * all the callbacks is OK. */
		switch (level)
		{
		case XHPROF_MODE_HIERARCHICAL:
			XHG(mode_cb).begin_fn_cb = hp_mode_hier_beginfn_cb;
			XHG(mode_cb).end_fn_cb = hp_mode_hier_endfn_cb;
			break;
		case XHPROF_MODE_SAMPLED:
			XHG(mode_cb).init_cb = hp_mode_sampled_init_cb;
			XHG(mode_cb).begin_fn_cb = hp_mode_sampled_beginfn_cb;
			XHG(mode_cb).end_fn_cb = hp_mode_sampled_endfn_cb;
			break;
		}

		/* one time initializations */
		hp_init_profiler_state(level);

		/* start profiling from fictitious main() */
		BEGIN_PROFILING(&XHG(entries), ROOT_SYMBOL, hp_profile_flag);
	}
}

/**
 * Called at request shutdown time. Cleans the profiler's global state.
 */
static void hp_end()
{
	/* Bail if not ever enabled */
	if (!XHG(ever_enabled))
	{
		return;
	}

	/* Stop profiler if enabled */
	if (XHG(enabled))
	{
		hp_stop();
	}

	/* Clean up state */
	hp_clean_profiler_state();
}

/**
 * Called from xhprof_disable(). Removes all the proxies setup by
 * hp_begin() and restores the original values.
 */
static void hp_stop()
{
	int hp_profile_flag = 1;

	/* End any unfinished calls */
	while (XHG(entries))
	{
		END_PROFILING(&XHG(entries), hp_profile_flag);
	}

	/* Remove proxies, restore the originals */
	zend_compile_file     = _zend_compile_file;
	zend_compile_string   = _zend_compile_string;
	zend_execute_ex       = _zend_execute_ex;

	/* Resore cpu affinity. */
	restore_cpu_affinity(&XHG(prev_mask));

	/* Stop profiling */
	XHG(enabled) = 0;
}


/**
 * *****************************
 * XHPROF ZVAL UTILITY FUNCTIONS
 * *****************************
 */

/**
 * @phpng
 *  Look in the PHP assoc array to find a key and return the zval associated
 *  with it.
 *
 *  @author mpal
 **/
static zval *hp_zval_at_key(char *key, zval *values)
{
	zval *result = NULL;

	if (Z_TYPE_P(values) == IS_ARRAY)
	{
		HashTable *ht;
		uint len = strlen(key);

		ht = Z_ARRVAL_P(values);
		result = zend_hash_str_find(ht, key, len);
	}

	return result;
}

/**
 * 	@phpng
 *  Convert the PHP array of strings to an emalloced array of strings. Note,
 *  this method duplicates the string data in the PHP array.
 *
 *  @author mpal
 **/
static char **hp_strings_in_zval(zval *values)
{
	char **result;
	size_t count;
	size_t ix = 0;

	if (!values)
	{
		return NULL;
	}

	if (Z_TYPE_P(values) == IS_ARRAY)
	{
		HashTable *ht;
		zend_ulong num_key;
		zend_string * key;
		zval * val;

		ht = Z_ARRVAL_P(values);
		count = zend_hash_num_elements(ht);

		if ((result = (char**) malloc(sizeof(char*) * (count + 1))) == NULL)
		{
			return result;
		}

		ZEND_HASH_FOREACH_KEY_VAL(ht,num_key,key,val)
		{
			if (!key)
			{
				if (Z_TYPE_P(val) == IS_STRING
						&& strcmp(Z_STRVAL_P(val), ROOT_SYMBOL)
								!= 0)
				{/* do not ignore "main" */
					result[ix] = strdup(Z_STRVAL_P(val));
					ix++;
				}
			}
		}ZEND_HASH_FOREACH_END();
	}
	else if (Z_TYPE_P(values) == IS_STRING)
	{
		if ((result = (char**) malloc(sizeof(char*) * 2)) == NULL)
		{
			return result;
		}
		result[0] = strdup(Z_STRVAL_P(values));
		ix = 1;
	}
	else
	{
		result = NULL;
	}

	/* NULL terminate the array */
	if (result != NULL)
	{
		result[ix] = NULL;
	}

	return result;
}

/**
 *  @phpng
 *  Free this memory at the end of profiling
 */
static inline void hp_array_del(char **name_array)
{
	if (name_array != NULL)
	{
		int i = 0;
		for (; name_array[i] != NULL && i < XHPROF_MAX_IGNORED_FUNCTIONS; i++)
		{
			free(name_array[i]);
		}
		free(name_array);
	}
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */

