/**
 * The MIT License
 *
 * Copyright (C) 2020 MBition GmbH
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Frank Bielig <frank dot bielig at mbition.io>
 */

#include "collectd.h"

#include "filter_chain.h"
#include "utils/common/common.h"
#include "utils_cache.h"

/* ************************************************************************** */
/* types */
/* ************************************************************************** */

struct mov_match_s;
typedef struct mov_match_s mov_match_t;
struct mov_match_s {
  value_list_t vl;
  const data_set_t *type_ds;

  gauge_t min;
  gauge_t max;
  int invert;
  int satisfy;

  char **data_sources;
  size_t data_sources_num;
};

/* ************************************************************************** */
/* constants */
/* ************************************************************************** */

#define LOG_KEY "match other value: "
#define PLACEHOLDER '-'

#define SATISFY_ALL 0
#define SATISFY_ANY 1

/* **************************************************************************
 */
/* internal helper functions */
/* **************************************************************************
 */

/* --------------------------------------------------------------------------
 */
static void mov_free_match(mov_match_t *m) {
  if (m == NULL)
    return;

  if (m->data_sources != NULL) {
    for (size_t i = 0; i < m->data_sources_num; ++i)
      free(m->data_sources[i]);
    free(m->data_sources);
  }

  free(m);
}

/* -------------------------------------------------------------------------- */
static int mov_config_add_satisfy(mov_match_t *m, oconfig_item_t *ci) {
  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING)) {
    ERROR(LOG_KEY "`%s' needs exactly one string argument.", ci->key);
    return -1;
  }

  if (strcasecmp("All", ci->values[0].value.string) == 0)
    m->satisfy = SATISFY_ALL;
  else if (strcasecmp("Any", ci->values[0].value.string) == 0)
    m->satisfy = SATISFY_ANY;
  else {
    ERROR(LOG_KEY "Passing `%s' to the `%s' option is invalid. "
                  "The argument must either be `All' or `Any'.",
          ci->values[0].value.string, ci->key);
    return -1;
  }

  return 0;
}

/* -------------------------------------------------------------------------- */
static int mov_config_add_data_source(mov_match_t *m, oconfig_item_t *ci) {
  size_t new_data_sources_num;
  char **temp;

  /* Check number of arbuments. */
  if (ci->values_num < 1) {
    ERROR(LOG_KEY "`%s' needs at least one argument.", ci->key);
    return -1;
  }

  /* Check type of arguments */
  for (int i = 0; i < ci->values_num; i++) {
    if (ci->values[i].type == OCONFIG_TYPE_STRING)
      continue;

    ERROR(LOG_KEY "`%s' accepts only string arguments "
                  "(argument %i is a %s).",
          ci->key, i + 1,
          (ci->values[i].type == OCONFIG_TYPE_BOOLEAN) ? "truth value"
                                                       : "number");
    return -1;
  }

  /* Allocate space for the char pointers */
  new_data_sources_num = m->data_sources_num + ((size_t)ci->values_num);
  temp = realloc(m->data_sources, new_data_sources_num * sizeof(char *));
  if (temp == NULL) {
    ERROR(LOG_KEY "realloc failed.");
    return -1;
  }
  m->data_sources = temp;

  /* Copy the strings, allocating memory as needed. */
  for (int i = 0; i < ci->values_num; i++) {
    /* If we get here, there better be memory for us to write to. */
    assert(m->data_sources_num < new_data_sources_num);

    size_t j = m->data_sources_num;
    m->data_sources[j] = sstrdup(ci->values[i].value.string);
    if (m->data_sources[j] == NULL) {
      ERROR(LOG_KEY "sstrdup failed.");
      continue;
    }
    m->data_sources_num++;
  }

  return 0;
}

/* -------------------------------------------------------------------------- */
static int mov_config_add_vlstring(char *to, oconfig_item_t *ci) {
  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING)) {
    ERROR(LOG_KEY "`%s' needs exactly one string argument.", ci->key);
    return -1;
  }
  if (to[0] != PLACEHOLDER) {
    ERROR(LOG_KEY "`%s' is defined multiple times.", ci->key);
    return -1;
  }

  sstrncpy(to, ci->values[0].value.string, DATA_MAX_NAME_LEN);

  return 0;
}

/* -------------------------------------------------------------------------- */
static int mov_config_add_gauge(gauge_t *ret_value, oconfig_item_t *ci) {

  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_NUMBER)) {
    ERROR(LOG_KEY "`%s' needs exactly one numeric argument.", ci->key);
    return -1;
  }

  *ret_value = ci->values[0].value.number;

  return 0;
}

/* -------------------------------------------------------------------------- */
static int mov_config_add_boolean(int *ret_value, oconfig_item_t *ci) {

  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_BOOLEAN)) {
    ERROR(LOG_KEY "`%s' needs exactly one boolean argument.", ci->key);
    return -1;
  }

  if (ci->values[0].value.boolean)
    *ret_value = 1;
  else
    *ret_value = 0;

  return 0;
}

/* ************************************************************************** */
/* plugin functions */
/* ************************************************************************** */

/* -------------------------------------------------------------------------- */
static int mov_create(const oconfig_item_t *ci, void **user_data) {
  mov_match_t *m;
  int status;

  m = calloc(1, sizeof(*m));
  if (m == NULL) {
    ERROR(LOG_KEY "mov_create: calloc failed.");
    return -ENOMEM;
  }

  memset(&m->vl, 0, sizeof(m->vl));
  m->vl.host[0] = PLACEHOLDER;
  m->vl.plugin[0] = PLACEHOLDER;
  m->vl.plugin_instance[0] = PLACEHOLDER;
  m->vl.type[0] = PLACEHOLDER;
  m->vl.type_instance[0] = PLACEHOLDER;
  m->type_ds = NULL;
  m->min = NAN;
  m->max = NAN;
  m->invert = 0;
  m->satisfy = SATISFY_ALL;
  m->data_sources = NULL;
  m->data_sources_num = 0;

  status = 0;
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp("Type", child->key) == 0) {
      status = mov_config_add_vlstring(m->vl.type, child);
    } else if (strcasecmp("TypeInstance", child->key) == 0) {
      status = mov_config_add_vlstring(m->vl.type_instance, child);
    } else if (strcasecmp("Plugin", child->key) == 0) {
      status = mov_config_add_vlstring(m->vl.plugin, child);
    } else if (strcasecmp("PluginInstance", child->key) == 0) {
      status = mov_config_add_vlstring(m->vl.plugin_instance, child);
    } else if (strcasecmp("Host", child->key) == 0) {
      status = mov_config_add_vlstring(m->vl.host, child);
    } else if (strcasecmp("Min", child->key) == 0) {
      status = mov_config_add_gauge(&m->min, child);
    } else if (strcasecmp("Max", child->key) == 0) {
      status = mov_config_add_gauge(&m->max, child);
    } else if (strcasecmp("Invert", child->key) == 0) {
      status = mov_config_add_boolean(&m->invert, child);
    } else if (strcasecmp("Satisfy", child->key) == 0) {
      status = mov_config_add_satisfy(m, child);
    } else if (strcasecmp("DataSource", child->key) == 0) {
      status = mov_config_add_data_source(m, child);
    } else {
      ERROR(LOG_KEY "The `%s' configuration option is not "
                    "understood and will be ignored.",
            child->key);
      status = 0;
    }

    if (status != 0)
      break;
  }

  /* Additional sanity-checking */
  while (status == 0) {
    if (isnan(m->min) && isnan(m->max)) {
      ERROR("Neither minimum nor maximum are defined. "
            "This match will be ignored.");
      status = -1;
    }

    break;
  }

  if (status != 0) {
    mov_free_match(m);
    return status;
  }

  *user_data = m;
  return 0;
}

/* -------------------------------------------------------------------------- */
static int mov_destroy(void **user_data) {
  if ((user_data != NULL) && (*user_data != NULL))
    mov_free_match(*user_data);
  return 0;
}

/* -------------------------------------------------------------------------- */
static int mov_match(const data_set_t *ds, const value_list_t *vl,
                     notification_meta_t __attribute__((unused)) * *meta,
                     void **user_data) {

  mov_match_t *m;
  gauge_t *values;
  int result;

  if ((user_data == NULL) || (*user_data == NULL))
    return -1;

  m = *user_data;

  // assemble other value list
  value_list_t ovl;
  memcpy(&ovl, &m->vl, sizeof(ovl));
  if (ovl.host[0] == PLACEHOLDER)
    sstrncpy(ovl.host, vl->host, sizeof(ovl.host));
  if (ovl.plugin[0] == PLACEHOLDER)
    sstrncpy(ovl.plugin, vl->plugin, sizeof(ovl.plugin));
  if (ovl.plugin_instance[0] == PLACEHOLDER)
    sstrncpy(ovl.plugin_instance, vl->plugin_instance,
             sizeof(ovl.plugin_instance));
  if (ovl.type[0] == PLACEHOLDER)
    sstrncpy(ovl.type, vl->type, sizeof(ovl.type));
  if (ovl.type_instance[0] == PLACEHOLDER)
    sstrncpy(ovl.type_instance, vl->type_instance, sizeof(ovl.type_instance));

  if (m->type_ds == NULL) {
    m->type_ds = plugin_get_ds(m->vl.type);
    if (m->type_ds == NULL) {
      ERROR(LOG_KEY "no dataset for type '%s' found", m->vl.type);
      return -1;
    }
#if COLLECT_DEBUG
    DEBUG(LOG_KEY "found data type definition '%s'", m->type_ds->type);
    for (size_t co = 0; co < m->type_ds->ds_num; ++co) {
      data_source_t *ds = &m->type_ds->ds[co];
      DEBUG(LOG_KEY "  %lu:  '%s' (type=%d, min=%g, max=%g)", co, ds->name,
            ds->type, ds->min, ds->max);
    }
#endif
  }

  values = uc_get_rate(m->type_ds, &ovl);
  if (values == NULL) {
    DEBUG(LOG_KEY "Retrieving the current rate from the cache failed.");
    return m->invert ? FC_MATCH_MATCHES : FC_MATCH_NO_MATCH;
  }

  result = FC_MATCH_NO_MATCH;
  for (size_t i = 0; i < ds->ds_num; i++) {
    int value_matches = 0;

    /* Check if this data source is relevant. */
    if (m->data_sources != NULL) {
      size_t j;

      for (j = 0; j < m->data_sources_num; j++)
        if (strcasecmp(ds->ds[i].name, m->data_sources[j]) == 0)
          break;

      /* No match, ignore this data source. */
      if (j >= m->data_sources_num)
        continue;
    }

    DEBUG(LOG_KEY "type = %s; current = %g; min = %g; max = %g; invert = %s;",
          ds->ds[i].name, values[i], m->min, m->max,
          m->invert ? "true" : "false");

    if ((!isnan(m->min) && (values[i] < m->min)) ||
        (!isnan(m->max) && (values[i] > m->max)))
      value_matches = 0;
    else
      value_matches = 1;

    if (m->invert) {
      if (value_matches)
        value_matches = 0;
      else
        value_matches = 1;
    }

    if (value_matches != 0) {
      result = FC_MATCH_MATCHES;
      if (m->satisfy == SATISFY_ANY)
        break;
    } else {
      result = FC_MATCH_NO_MATCH;
      if (m->satisfy == SATISFY_ALL)
        break;
    }
  }

  free(values);
  DEBUG(LOG_KEY "  match result: %d", result);

  return result;
}

/* ************************************************************************** */
/* configuration */
/* ************************************************************************** */

void module_register(void) {
  match_proc_t mproc = {0};

  mproc.create = mov_create;
  mproc.destroy = mov_destroy;
  mproc.match = mov_match;

  fc_register_match("other_value", mproc);
}
