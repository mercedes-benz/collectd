/**
 * SPDX-FileCopyrightText: 2021 MBition GmbH
 **/

#include "testing.h"
#include "write_dlt.cpp"

#define TEST_HOSTNAME "test_hostname"
#define TEST_PLUGIN "test_plugin"
#define TEST_PLUGIN_INST "test_plugin_inst"
#define TEST_TYPE "cpu"
#define TEST_TYPE_INST "test_type_inst"
#define TEST_VALUE_TYPE "value"
#define TEST_VALUE 42

/* ========================================================================== */
/* utilities */
/* ========================================================================== */

value_list_t *create_test_value_list() {
  value_list_t *vl = (value_list_t *)calloc(1, sizeof(*vl));
  assert(vl != NULL);
  strncpy(vl->host, TEST_HOSTNAME, DATA_MAX_NAME_LEN);
  strncpy(vl->plugin, TEST_PLUGIN, DATA_MAX_NAME_LEN);
  strncpy(vl->plugin_instance, TEST_PLUGIN_INST, DATA_MAX_NAME_LEN);
  strncpy(vl->type, TEST_TYPE, DATA_MAX_NAME_LEN);
  strncpy(vl->type_instance, TEST_TYPE_INST, DATA_MAX_NAME_LEN);
  vl->values_len = 1;
  vl->values = (value_t *)calloc(1, sizeof(*vl->values));
  assert(vl->values != NULL);
  vl->values[0].derive = TEST_VALUE;
  return vl;
}

/* -------------------------------------------------------------------------- */
void destroy_test_value_list(value_list_t **vl) {
  if (*vl == NULL)
    return;
  if ((*vl)->values)
    free((*vl)->values);
  sfree(*vl);
}

/* -------------------------------------------------------------------------- */
data_set_t *create_test_data_set() {
  data_set_t *ds = (data_set_t *)calloc(1, sizeof(*ds));
  assert(ds != NULL);
  strncpy(ds->type, TEST_TYPE, DATA_MAX_NAME_LEN);
  ds->ds_num = 1;
  ds->ds = (data_source_t *)calloc(1, sizeof(*ds->ds));
  assert(ds->ds != NULL);
  strncpy(ds->ds[0].name, TEST_VALUE_TYPE, DATA_MAX_NAME_LEN);
  ds->ds[0].type = DS_TYPE_DERIVE;
  ds->ds[0].min = 0;
  ds->ds[0].max = 100;
  return ds;
}

/* -------------------------------------------------------------------------- */
void destroy_test_data_set(data_set_t **ds) {
  if (*ds == NULL)
    return;
  if (((*ds)->ds) != NULL)
    free((*ds)->ds);
  sfree(*ds);
}

/* ========================================================================== */
/* start up and shut down */
/* ========================================================================== */

static int setup(void) {
  int status;
  status = wdlt_init();
  if (status != 0) {
    printf("ERROR: wdlt_init() failed with %d\n", status);
    return -1;
  }

  return 0;
}

/* -------------------------------------------------------------------------- */
static int teardown(void) {
  int status;
  status = wdlt_shutdown();
  if (status != 0) {
    printf("error: wdlt_shutdown() failed with %d\n", status);
    return -1;
  }

  return 0;
}

/* ========================================================================== */
/* tests */
/* ========================================================================== */

DEF_TEST(level_matching) {
  DltLogLevelType level;
  if (setup() == 0) {
    level = wdlt_level_list_get("abc");
    EXPECT_EQ_INT(DLT_LOG_INFO, level);
  }
  teardown();

  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(log_graphite) {

  char buffer[4096];
  char chkbuf[4096];
  snprintf(chkbuf, sizeof(chkbuf),
           "%s.%s.%s;host=%s;plugin=%s;plugin_instance=%s;type=%s;type_"
           "instance=%s;ds_name=%s %d",
           TEST_PLUGIN, TEST_TYPE, TEST_VALUE_TYPE, TEST_HOSTNAME, TEST_PLUGIN,
           TEST_PLUGIN_INST, TEST_TYPE, TEST_TYPE_INST, TEST_VALUE_TYPE,
           TEST_VALUE);

  if (setup() == 0) {
    value_list_t *vl = create_test_value_list();
    data_set_t *ds = create_test_data_set();
    wdlt_write_graphite(buffer, sizeof(buffer), ds, vl,
                        GRAPHITE_USE_TAGS | GRAPHITE_ALWAYS_APPEND_DS);
    EXPECT_IN_STR(chkbuf, buffer);
    destroy_test_data_set(&ds);
    destroy_test_value_list(&vl);
  }
  teardown();
  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(log_json) {

  char buffer[4096];
  char chkbuf[4096];
  snprintf(chkbuf, sizeof(chkbuf),
           "[{\"values\":[%d],\"dstypes\":[\"derive\"],\"dsnames\":[\"%s\"],"
           "\"time\":0.000,\"interval\":0.000,\"host\":\"%s\","
           "\"plugin\":\"%s\",\"plugin_instance\":\"%s\","
           "\"type\":\"%s\",\"type_instance\":\"%s\"}]",
           TEST_VALUE, TEST_VALUE_TYPE, TEST_HOSTNAME, TEST_PLUGIN,
           TEST_PLUGIN_INST, TEST_TYPE, TEST_TYPE_INST);

  if (setup() == 0) {
    value_list_t *vl = create_test_value_list();
    data_set_t *ds = create_test_data_set();
    wdlt_write_json(buffer, sizeof(buffer), ds, vl);
    EXPECT_IN_STR(chkbuf, buffer);
    destroy_test_data_set(&ds);
    destroy_test_value_list(&vl);
  }
  teardown();
  return 0;
}

/* ========================================================================== */
/* main */
/* ========================================================================== */

int main(void) {
  RUN_TEST(level_matching);
  RUN_TEST(log_graphite);
  RUN_TEST(log_json);

  END_TEST;
}
