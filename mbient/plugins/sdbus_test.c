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

#include "sdbus.c"
#include "testing.h"

/* ************************************************************************** */
/* start up and shut down */
/* ************************************************************************** */

static sd_bus *bus = NULL;

static int teardown(void) {

  if (bus != NULL)
    CHECK_ZERO(sdbus_close(&bus));

  return 0;
}

static int setup(sdbus_bind_t type) {
  teardown();
  return sdbus_acquire(&bus, type, false);
}

/* ************************************************************************** */
/* tests */
/* ************************************************************************** */

/* -------------------------------------------------------------------------- */
DEF_TEST(connect) {
  CHECK_ZERO(setup(TARGET_LOCAL_USER));

  teardown();

  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(search) {
  if (setup(TARGET_LOCAL_USER) == 0) {
    char **names = sdbus_names(bus, false);
    CHECK_NOT_NULL(names);
    int count = strv_length(names);
    INFO("found %d names", count);
    OK1(count > 0, "at least on service should be listed");
    strv_free(names);
  }

  teardown();
  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(read) {
  CHECK_ZERO(sdbus_init());
  OK1(sdbus_read() == 0, "read statistics");
  CHECK_ZERO(sdbus_shutdown());
  return 0;
}

/* ************************************************************************** */
/* main */
/* ************************************************************************** */

int main(void) {
  RUN_TEST(connect);
  RUN_TEST(search);
  RUN_TEST(read);

  printf("failures: %d", fail_count__);
  END_TEST;
}
