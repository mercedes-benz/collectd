/**
 * The MIT License
 *
 * Copyright (C) 2025 MBition GmbH
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
 *   Dmytro Nikitin <dmytro dot nikitin at intellias.com>
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include "liboconfig/oconfig.h"
#include "osp_partition_manger.c"
#include "testing.h"

static int notify_counter = 0;
static int fclose_counter = 0;
static int closedir_counter = 0;
static int readdir_counter = 0;
static int msg_equal_counter = 0;

const char dev_swup_counters[] = "1,1,0";
const char dev_swup_events[] = "2025-07-17 10:32:45,fsck";
const char dev_swup_counters_msg[] =
    "partition manager counter info: -dev-swup1,1,0;";
const char dev_swup_events_msg[] =
    "partition manager events info: -dev-swup: 2025-07-17 10:32:45,fsck ;";

static int osp_notify_mock(char *buf, int buf_len) {
  if (strncmp(buf, dev_swup_counters_msg, buf_len) == 0) {
    ++msg_equal_counter;
    ++notify_counter;
  }
  if (strncmp(buf, dev_swup_events_msg, buf_len) == 0) {
    ++msg_equal_counter;
    ++notify_counter;
  }
  return 0;
}
static FILE *fopen_osp_mock(const char *buf, const char *mode) {
  if (strstr(buf, "counters") != NULL) {
    return fmemopen((char *)dev_swup_counters, strlen(dev_swup_counters), "r");
  } else if (strstr(buf, "events") != NULL) {
    return fmemopen((char *)dev_swup_events, strlen(dev_swup_events), "r");
  }
  return NULL;
}
static int fclose_osp_mock(FILE *file) {
  fclose(file);
  return ++fclose_counter;
}

static DIR *opendir_osp_mock(const char *) { return (DIR *)0xDEADBEEF; };
static struct dirent *readdir_osp_mock(DIR *) {
  static struct dirent _entry;

  if (readdir_counter == 0) {
    strcpy(_entry.d_name, "-dev-swup-counters.txt");
    ++readdir_counter;
    return &_entry;
  } else if (readdir_counter == 1) {
    strcpy(_entry.d_name, "-dev-swup-events.txt");
    ++readdir_counter;
    return &_entry;
  }
  return NULL;
};

static int closedir_osp_mock(DIR *) {
  ++closedir_counter;
  return 0;
}

static void set_up() {
  notify = osp_notify_mock;
  fopen_osp = fopen_osp_mock;
  fclose_osp = fclose_osp_mock;
  opendir_osp = opendir_osp_mock;
  readdir_osp = readdir_osp_mock;
  closedir_osp = closedir_osp_mock;
}

static void teardown() {
  notify_counter = 0;
  fclose_counter = 0;
  closedir_counter = 0;
  readdir_counter = 0;
  msg_equal_counter = 0;
}

DEF_TEST(PartitionManagerParserTest) {
  int result = 0;
  set_up();
  partition_manager_parser("/mnt/backup/partition-manager/");
  EXPECT_EQ_INT(notify_counter, 2);
  EXPECT_EQ_INT(fclose_counter, 2);
  EXPECT_EQ_INT(closedir_counter, 1);
  EXPECT_EQ_INT(readdir_counter, 2);
  EXPECT_EQ_INT(msg_equal_counter, 2);
  teardown();
  return result;
}

int main(void) {
  RUN_TEST(PartitionManagerParserTest);
  END_TEST;
}