/*
 * ldmsd_env.c
 *
 *  Created on: Apr 9, 2020
 *      Author: nnich
 */

#include "ldmsd.h"
#include "ldmsd_request.h"

void ldmsd_env___del(ldmsd_cfgobj_t obj)
{
	ldmsd_env_t env = (ldmsd_env_t)obj;
	if (env->name)
		free(env->name);
	if (env->value)
		free(env->value);
	ldmsd_cfgobj___del(obj);
}

static json_entity_t __add_value(ldmsd_env_t env, json_entity_t result)
{
	return json_dict_build(result, JSON_STRING_VALUE, "value", env->value);
}

json_entity_t ldmsd_env_query(ldmsd_cfgobj_t obj)
{
	json_entity_t query;
	ldmsd_env_t env = (ldmsd_env_t)obj;
	query = ldmsd_cfgobj_query_result_new(obj);
	if (!query)
		return NULL;
	query = __add_value(env, query);
	return ldmsd_result_new(0, NULL, query);
}

json_entity_t ldmsd_env_update(ldmsd_cfgobj_t obj, short enabled,
				json_entity_t dft, json_entity_t spc)
{
	return ldmsd_result_new(EINVAL,
			"Environment variable cannot be updated.", NULL);
}

json_entity_t ldmsd_env_create(const char *name, short enable, json_entity_t dft,
					json_entity_t spc, uid_t uid, gid_t gid)
{
	int rc;
	json_entity_t value;
	char *value_s;
	value = json_value_find(spc, "value");
	value_s = json_value_str(value)->str;
	ldmsd_env_t env = (ldmsd_env_t)ldmsd_cfgobj_new(name, LDMSD_CFGOBJ_ENV,
				sizeof(*env), ldmsd_env___del,
				ldmsd_env_update,
				ldmsd_cfgobj_delete,
				ldmsd_env_query,
				ldmsd_env_query,
				NULL,
				NULL,
				uid, gid, 0770, enable);
	if (env) {
		env->name = strdup(name);
		if (!env->name)
			goto oom;
		env->value = strdup(value_s);
		if (!env->value)
			goto oom;
		rc = setenv(name, value_s, 1);
		if (rc) {
			char msg[1024];
			snprintf(msg, 1024, "Failed to export "
				"the environment variable '%s'\n", name);
			ldmsd_log(LDMSD_LERROR, "%s", msg);
			return ldmsd_result_new(rc, msg, NULL);
		}
	}
	return ldmsd_result_new(0, NULL, NULL);
oom:
	ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
	return NULL;
}
