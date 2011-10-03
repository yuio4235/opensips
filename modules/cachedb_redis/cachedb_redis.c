#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include "../../sr_module.h"
#include "../../dprint.h"
#include "../../error.h"
#include "../../pt.h"
#include "../../cachedb/cachedb.h"

#include "cachedb_redis_dbase.h"

static int mod_init(void);
static int child_init(int);
static void destroy(void);

static str cache_mod_name = str_init("redis");
struct cachedb_url *redis_script_urls = NULL;

int set_connection(unsigned int type, void *val)
{
	return cachedb_store_url(&redis_script_urls,(char *)val);
}

static param_export_t params[]={
	{ "cachedb_url",                 STR_PARAM|USE_FUNC_PARAM, (void *)&set_connection},
	{0,0,0}
};


/** module exports */
struct module_exports exports= {
	"cachedb_redis",					/* module name */
	MODULE_VERSION,
	DEFAULT_DLFLAGS,			/* dlopen flags */
	0,						/* exported functions */
	params,						/* exported parameters */
	0,							/* exported statistics */
	0,							/* exported MI functions */
	0,							/* exported pseudo-variables */
	0,							/* extra processes */
	mod_init,					/* module initialization function */
	(response_function) 0,      /* response handling function */
	(destroy_function)destroy,	/* destroy function */
	child_init			        /* per-child init function */
};


/**
 * init module function
 */
static int mod_init(void)
{
	cachedb_engine cde;

	LM_NOTICE("initializing module cachedb_redis ...\n");

	cde.name = cache_mod_name;

	cde.cdb_func.init = redis_init;
	cde.cdb_func.destroy = redis_destroy;
	cde.cdb_func.get = redis_get;
	cde.cdb_func.set = redis_set;
	cde.cdb_func.remove = redis_remove;
	cde.cdb_func.add = redis_add;
	cde.cdb_func.sub = redis_sub;

	if (register_cachedb(&cde) < 0) {
		LM_ERR("failed to initialize cachedb_redis\n");
		return -1;
	}

	return 0;
}

static int child_init(int rank)
{
	struct cachedb_url *it;
	cachedb_con *con;
	/* TODO - maybe filter here only SIP working children */

	for (it = redis_script_urls;it;it=it->next) {
		LM_DBG("iterating through conns - [%.*s]\n",it->url.len,it->url.s);
		con = redis_init(&it->url);
		if (con == NULL) {
			LM_ERR("failed to open connection\n");
			return -1;
		}
		if (cachedb_put_connection(&cache_mod_name,con) < 0) {
			LM_ERR("failed to insert connection\n");
			return -1;
		}
	}

	cachedb_free_url(redis_script_urls);
	return 0;
}

/*
 * destroy function
 */
static void destroy(void)
{
	LM_NOTICE("destroy module cachedb_redis ...\n");
	cachedb_end_connections(&cache_mod_name);
	return;
}