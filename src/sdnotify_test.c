/**
 * SPDX-FileCopyrightText: 2023 MBition GmbH
 **/

#include "sdnotify.c"
#include "testing.h"
#include "utils/common/common.h"

#define TEST_HOSTNAME "test_hostname"
#define TEST_PLUGIN "test_plugin"
#define TEST_PLUGIN_INST "test_plugin_inst"
#define TEST_TYPE "sdnotify"
#define TEST_TYPE_INST "test_type_inst"
#define TEST_VALUE_TYPE "value"
#define TEST_VALUE 42

static int watchdog_enabled = 0;
static uint64_t watchdog_interval = 5e6;
int sd_watchdog_enabled(int unset_environment, uint64_t *usec) {
  *usec = watchdog_interval;
  return watchdog_enabled;
}

static char notify_msg[256];
int sd_notify(int unset_environment, const char *state) {
  if (state != NULL) {
    sstrncpy(notify_msg, state, sizeof(notify_msg));
  }
  return 0;
}

int sd_test_no_watchdog(int wd_ret) {
  watchdog_enabled = wd_ret;
  OK(sdnotify_watchdog_check() > 0);
  OK(!ready_on_init);
  OK(!watchdog_on_read);
  OK(!watchdog_on_write);
  OK(!stopping_on_shutdown);
  return 0;
}

int sd_test_msg(bool *flag, int (*handler)(), const char *msg) {
  watchdog_enabled = 1;
  watchdog_interval = 2e7;
  notify_msg[0] = '\0';
  *flag = msg && *msg;
  OK1((*handler)() == 0, "call handler");
  EXPECT_EQ_STR(notify_msg, msg);
  return 0;
}

DEF_TEST(sdnotify_watchdog_check) {
  int ret = 0;
  ret += sd_test_no_watchdog(-1);
  ret += sd_test_no_watchdog(0);
  ret += sd_test_no_watchdog(1);
  return ret;
}

DEF_TEST(sdnotify_init_enabled) {
  return sd_test_msg(&ready_on_init, sdnotify_init, "READY=1");
}
DEF_TEST(sdnotify_init_disabled) {
  return sd_test_msg(&ready_on_init, sdnotify_init, "");
}

DEF_TEST(sdnotify_read_enabled) {
  return sd_test_msg(&watchdog_on_read, sdnotify_read, "WATCHDOG=1");
}
DEF_TEST(sdnotify_read_disabled) {
  return sd_test_msg(&watchdog_on_read, sdnotify_read, "");
}

DEF_TEST(sdnotify_write_enabled) {
  return sd_test_msg(&watchdog_on_write, sdnotify_write, "WATCHDOG=1");
}
DEF_TEST(sdnotify_write_disabled) {
  return sd_test_msg(&watchdog_on_write, sdnotify_write, "");
}

DEF_TEST(sdnotify_shutdown_enabled) {
  return sd_test_msg(&stopping_on_shutdown, sdnotify_shutdown, "STOPPING=1");
}
DEF_TEST(sdnotify_shutdown_disabled) {
  return sd_test_msg(&stopping_on_shutdown, sdnotify_shutdown, "");
}

/* ========================================================================== */
/* main */
/* ========================================================================== */

int main(void) {
  RUN_TEST(sdnotify_watchdog_check);
  RUN_TEST(sdnotify_init_enabled);
  RUN_TEST(sdnotify_init_disabled);
  RUN_TEST(sdnotify_read_enabled);
  RUN_TEST(sdnotify_read_disabled);
  RUN_TEST(sdnotify_write_enabled);
  RUN_TEST(sdnotify_write_disabled);
  RUN_TEST(sdnotify_shutdown_enabled);
  RUN_TEST(sdnotify_shutdown_disabled);
  END_TEST;
}
