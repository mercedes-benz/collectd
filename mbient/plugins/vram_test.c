/**
 * SPDX-FileCopyrightText: 2021 Daimler AG
 **/

#include <errno.h>
#include "testing.h"
#include "vram.c"

/* ************************************************************************** */
/* tests */
/* ************************************************************************** */

/* -------------------------------------------------------------------------- */
DEF_TEST(test_vram_get_process_name) {
  char buf[256];
  CHECK_ZERO(vram_get_process_name(1, buf, sizeof(buf)));
  EXPECT_EQ_STR(buf, "systemd");

  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(test_vram_unique_list) {
  vram_client_info_t source = {6,
                               {{1, "foo", 10},
                                {2, "foo", 20},
                                {3, "bar", 31},
                                {4, "foo", 40},
                                {5, "bar", 51},
                                {6, "baz", 62}}};
  vram_client_info_t target;
  CHECK_ZERO(vram_clients_info_create_unique(&target, &source));
  EXPECT_EQ_INT(target.entries_num, 3);
  EXPECT_EQ_STR(target.entries[0].name, "foo");
  EXPECT_EQ_STR(target.entries[1].name, "bar");
  EXPECT_EQ_STR(target.entries[2].name, "baz");
  EXPECT_EQ_INT(target.entries[0].vram_used, 70);
  EXPECT_EQ_INT(target.entries[1].vram_used, 82);
  EXPECT_EQ_INT(target.entries[2].vram_used, 62);

  return 0;
}

/* ************************************************************************** */
/* main */
/* ************************************************************************** */

int main(void) {
  RUN_TEST(test_vram_get_process_name);
  RUN_TEST(test_vram_unique_list);

  END_TEST;
}
