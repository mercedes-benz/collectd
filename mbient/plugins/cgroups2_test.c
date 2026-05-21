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

#include "cgroups2.c"
#include "liboconfig/oconfig.h"
#include "testing.h"
#include "utils/common/common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TEST_DIR "/tmp/fs.cgroups/"
#define TEST_FILE "main.service"

// Missing in src/daemon/utils_cache_mock.c
value_t *uc_get_value(const data_set_t *ds, const value_list_t *vl) {
  return NULL;
}

const char *test_config_plain = "LoadPlugin cgroups2\n"
                                "<Plugin cgroups2>\n"
                                "  MaxLevel 0\n"
                                "</Plugin>\n";

/* ************************************************************************** */
/* utility functions */
/* ************************************************************************** */

/* -------------------------------------------------------------------------- */
static int write_to_file(const char *file, const char *content) {
  FILE *fp = fopen(file, "w");
  if (!fp) {
    perror("fopen");
    return -1;
  }
  if (content != NULL)
    fputs(content, fp);
  fclose(fp);
  return 0;
}

/* -------------------------------------------------------------------------- */
static int create_empty_file(const char *path) {
  return write_to_file(path, NULL);
}

/* -------------------------------------------------------------------------- */
static int create_dir(const char *path) {
  int rc = mkdir(path, S_IRWXU);
  if (rc != 0 && errno != EEXIST) {
    perror("mkdir");
    exit(1);
  }
  return 0;
}

/* -------------------------------------------------------------------------- */
static int remove_directory(const char *path) {
  DIR *dir = opendir(path);
  if (dir == NULL) {
    perror("opendir");
    return -1;
  }

  struct dirent *entry;
  char full_path[1024];

  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

    if (entry->d_type == DT_DIR) {
      if (remove_directory(full_path) != 0) {
        return -1;
      }
    } else {
      if (unlink(full_path) != 0) {
        perror("unlink");
        return -1;
      }
    }
  }

  closedir(dir);

  if (rmdir(path) != 0) {
    perror("rmdir");
    return -1;
  }

  return 0;
}

/* ************************************************************************** */
/* start up and shut down */
/* ************************************************************************** */

static int get_config(const char *data, oconfig_item_t **cfg) {
  char cfg_fname[64] = {"/tmp/collectd-cfg-XXXXXX"};
  OK(mkstemp(cfg_fname));
  printf("configuration file: %s", cfg_fname);
  FILE *cfg_file = fopen(cfg_fname, "w+");
  CHECK_NOT_NULL(cfg_file);
  size_t data_size = strlen(data);
  size_t written = fwrite(data, 1, data_size, cfg_file);
  OK(data_size == written);
  CHECK_ZERO(fclose(cfg_file));

  *cfg = oconfig_parse_file(cfg_fname);
  if (*cfg == NULL) {
    printf("========================================\n");
    printf("%s \n", data);
    printf("========================================\n");
    return -1;
  }
  CHECK_ZERO(remove(cfg_fname));
  return 0;
}

static int del_config(oconfig_item_t **cfg) {
  if (cfg == NULL || *cfg == NULL) {
    return -1;
  }

  oconfig_free(*cfg);
  *cfg = NULL;

  return 0;
}

/* ************************************************************************** */
/* tests */
/* ************************************************************************** */

/* -------------------------------------------------------------------------- */
static int count_files(int dirfd, const char *dir_name, const char *file_name,
                       void *user_data) {
  (*((int *)user_data))++;
  return 0;
}

DEF_TEST(handle_file) {
  cg2_config("MaxLevel", "1");

  CHECK_ZERO(create_dir(TEST_DIR));
  CHECK_ZERO(create_dir(TEST_DIR "g1.slice"));
  CHECK_ZERO(create_dir(TEST_DIR "g2.slice"));
  CHECK_ZERO(create_dir(TEST_DIR "g3.slice"));
  CHECK_ZERO(create_dir(TEST_DIR "g3.slice/g31.slice"));
  CHECK_ZERO(create_dir(TEST_DIR "g3.slice/g32.slice"));
  CHECK_ZERO(create_dir(TEST_DIR "g4.mount"));
  CHECK_ZERO(create_empty_file(TEST_DIR "g1.slice/" TEST_FILE));
  CHECK_ZERO(create_empty_file(TEST_DIR "g2.slice/" TEST_FILE));
  CHECK_ZERO(create_empty_file(TEST_DIR "g3.slice/g31.slice/" TEST_FILE));
  CHECK_ZERO(create_empty_file(TEST_DIR "g3.slice/g32.slice/" TEST_FILE));
  CHECK_ZERO(create_empty_file(TEST_DIR "g4.mount/" TEST_FILE));

  int count = 0;
  cg2_file_handler_t h = {.file_name = TEST_FILE, .dir_pattern = NULL,
                           .callback = count_files, .user_data = &count};
  cg2_handle_files_multi(TEST_DIR, &h, 1, 0);
  EXPECT_EQ_INT(count, 3);

  count = 0;
  h.dir_pattern = SLICE_SUFFIX;
  cg2_handle_files_multi(TEST_DIR, &h, 1, 0);
  EXPECT_EQ_INT(count, 2);

  count = 0;
  h.file_name = "unknown.file";
  h.dir_pattern = NULL;
  cg2_handle_files_multi(TEST_DIR, &h, 1, 0);
  EXPECT_EQ_INT(count, 0);

  count = 0;
  h.file_name = TEST_FILE;
  h.dir_pattern = ".unknown";
  cg2_handle_files_multi(TEST_DIR, &h, 1, 0);
  EXPECT_EQ_INT(count, 0);

  CHECK_ZERO(remove_directory(TEST_DIR));

  return 0;
}

/* -------------------------------------------------------------------------- */
/* Verify that cg2_handle_files_multi() visits each handler's file exactly
 * the right number of times in a single traversal, respecting per-handler
 * dir_pattern filters. */
DEF_TEST(handle_file_multi) {
  cg2_config("MaxLevel", "1");

  /* Directory layout:
   *   TEST_DIR/
   *     g1.slice/   main.service   extra.file
   *     g2.slice/   main.service
   *     g3.slice/
   *       g31.slice/ main.service
   *       g32.slice/ main.service
   *     g4.mount/   main.service   extra.file
   */
  CHECK_ZERO(create_dir(TEST_DIR));
  CHECK_ZERO(create_dir(TEST_DIR "g1.slice"));
  CHECK_ZERO(create_dir(TEST_DIR "g2.slice"));
  CHECK_ZERO(create_dir(TEST_DIR "g3.slice"));
  CHECK_ZERO(create_dir(TEST_DIR "g3.slice/g31.slice"));
  CHECK_ZERO(create_dir(TEST_DIR "g3.slice/g32.slice"));
  CHECK_ZERO(create_dir(TEST_DIR "g4.mount"));
  CHECK_ZERO(create_empty_file(TEST_DIR "g1.slice/" TEST_FILE));
  CHECK_ZERO(create_empty_file(TEST_DIR "g2.slice/" TEST_FILE));
  CHECK_ZERO(create_empty_file(TEST_DIR "g3.slice/g31.slice/" TEST_FILE));
  CHECK_ZERO(create_empty_file(TEST_DIR "g3.slice/g32.slice/" TEST_FILE));
  CHECK_ZERO(create_empty_file(TEST_DIR "g4.mount/" TEST_FILE));
  CHECK_ZERO(create_empty_file(TEST_DIR "g1.slice/extra.file"));
  CHECK_ZERO(create_empty_file(TEST_DIR "g4.mount/extra.file"));

  int count_main = 0;  /* counts TEST_FILE hits with SLICE_SUFFIX filter */
  int count_extra = 0; /* counts extra.file hits with NULL filter        */

  cg2_file_handler_t handlers[] = {
      {
          .file_name = TEST_FILE,
          .dir_pattern = SLICE_SUFFIX,
          .callback = count_files,
          .user_data = &count_main,
      },
      {
          .file_name = "extra.file",
          .dir_pattern = NULL, /* all directories */
          .callback = count_files,
          .user_data = &count_extra,
      },
  };

  int ret = cg2_handle_files_multi(TEST_DIR, handlers,
                                   STATIC_ARRAY_SIZE(handlers), 0);
  OK(ret > 0);

  /* SLICE_SUFFIX handler: g1.slice and g2.slice => 2
   * (g3.slice itself has no TEST_FILE;
   *  g4.mount is entered via the NULL-pattern handler but SLICE_SUFFIX
   *  dir_pattern check prevents reading TEST_FILE there;
   *  g31.slice, g32.slice not reached: MaxLevel 1 stops descent) */
  EXPECT_EQ_INT(count_main, 2);

  /* NULL-pattern handler: g1.slice and g4.mount both have extra.file => 2 */
  EXPECT_EQ_INT(count_extra, 2);

  /* A single-handler call with an unknown file must return 0 matches */
  int count_none = 0;
  cg2_file_handler_t h_none = {
      .file_name = "no.such.file",
      .dir_pattern = NULL,
      .callback = count_files,
      .user_data = &count_none,
  };
  ret = cg2_handle_files_multi(TEST_DIR, &h_none, 1, 0);
  EXPECT_EQ_INT(count_none, 0);

  /* A single-handler call with a non-matching dir_pattern must return 0 */
  int count_bad_pat = 0;
  cg2_file_handler_t h_bad_pat = {
      .file_name = TEST_FILE,
      .dir_pattern = ".unknown",
      .callback = count_files,
      .user_data = &count_bad_pat,
  };
  ret = cg2_handle_files_multi(TEST_DIR, &h_bad_pat, 1, 0);
  EXPECT_EQ_INT(count_bad_pat, 0);

  CHECK_ZERO(remove_directory(TEST_DIR));

  return 0;
}

/* --------------------------------------------------------------------------
 */
DEF_TEST(load_config) {
  oconfig_item_t *cfg = NULL;
  CHECK_ZERO(get_config(test_config_plain, &cfg));

  CHECK_ZERO(del_config(&cfg));
  return 0;
}

/* --------------------------------------------------------------------------
 */
DEF_TEST(plugin_config) {
  oconfig_item_t *cfg = NULL;
  CHECK_ZERO(get_config(test_config_plain, &cfg));

  cg2_init();
  for (int co = 0; co < 5; ++co)
    cg2_read();

  CHECK_ZERO(del_config(&cfg));
  return 0;
}

/* --------------------------------------------------------------------------
 */
DEF_TEST(read_sys_fs_cgroup) {
  const char *base_dir = "/sys/fs/cgroup";
  cg2_file_handler_t handlers[] = {
      {
          .file_name = "cpu.pressure",
          .dir_pattern = ".slice",
          .callback = cg2_handle_files_log,
          .user_data = NULL,
      },
  };
  cg2_handle_files_multi(base_dir, handlers, STATIC_ARRAY_SIZE(handlers), 0);

  return 0;
}

/* --------------------------------------------------------------------------
 */
// This test is limited to one-time reading because the tested function relies
// on `uc_get_value` from utils_cache.c which is not available in these tests.

DEF_TEST(read_cpu_pressure) {
  const char *slice_name = "cherry.slice/";

  struct stat st = {0};
  if (stat(slice_name, &st) == -1) {
    mkdir(slice_name, 0700);
  }

  cg2_cpu_usage_info_t usage;
  cg2_cpu_usage_info_init(&usage);

  const char *usage_fn = "cherry.slice/cpu.stat";
  if (write_to_file(usage_fn,
                    "usage_usec 0\nuser_usec 1000000\nsystem_usec 1000000\n") <
      0) {
    perror("writing cpu.stat failed");
    return -1;
  }

  DIR *slice_dir = opendir(slice_name);
  if (slice_dir == NULL) {
    perror("opendir() failed");
    return -1;
  }
  int slice_fd = dirfd(slice_dir);
  if (slice_fd == -1) {
    perror("dirfd() faile");
    return -1;
  }

  OK(cg2_handle_v2_cpu_stat(slice_fd, slice_name, "cpu.stat", &usage) == 0);
  OK(usage.entries_num == 1);
  OK(usage.entries[0].perc_now == 0);
  OK(usage.entries[0].perc_avg != 0);

  cg2_pressure_info_t pressure;
  cg2_pressure_info_init(&pressure);

  const char *pressure_fn = "cherry.slice/cpu.pressure";
  if (write_to_file(pressure_fn,
                    "some avg10=0.00 avg60=0.00 avg300=0.00 total=100000\n") <
      0) {
    perror("Error: ");
    return -1;
  }

  void *data = &pressure;
  OK(cg2_handle_pressure(slice_fd, slice_name, "cpu.pressure", data) == 0);
  OK(pressure.entries_num == 1);
  OK(strcmp(pressure.entries[0].cgroup, "cherry") == 0);
  // OK(pressure.entries[0].some_pressure == PRESSURE_UNKNOWN);

  if (closedir(slice_dir) == -1) {
    perror("closedir() failed");
    return -1;
  }
  unlink(usage_fn);
  unlink(pressure_fn);
  unlink(slice_name);

  return 0;
}

/* --------------------------------------------------------------------------
 */
DEF_TEST(pretty_size) {

  const unsigned long ONE_KILOBYTE = 1ul << 10;
  const unsigned long ONE_MEGABYTE = 1ul << 20;
  const unsigned long ONE_GIGABYTE = 1ul << 30;

  char buf[64];

  OK(cg2_memory_unit == CG2_MEMORY_UNIT_AUTO);

  cg2_pretty_size(buf, sizeof(buf), ONE_KILOBYTE);
  OK(strncmp(buf, "1.0KB", sizeof(buf)) == 0);

  cg2_pretty_size(buf, sizeof(buf), ONE_MEGABYTE);
  OK(strncmp(buf, "1.0MB", sizeof(buf)) == 0);

  cg2_pretty_size(buf, sizeof(buf), ONE_GIGABYTE);
  OK(strncmp(buf, "1.0GB", sizeof(buf)) == 0);

  cg2_memory_unit = CG2_MEMORY_UNIT_KB;

  cg2_pretty_size(buf, sizeof(buf), ONE_KILOBYTE);
  OK(strncmp(buf, "1", sizeof(buf)) == 0);

  cg2_pretty_size(buf, sizeof(buf), ONE_MEGABYTE);
  OK(strncmp(buf, "1024", sizeof(buf)) == 0);

  cg2_pretty_size(buf, sizeof(buf), ONE_GIGABYTE);
  OK(strncmp(buf, "1048576", sizeof(buf)) == 0);

  cg2_memory_unit = CG2_MEMORY_UNIT_MB;

  cg2_pretty_size(buf, sizeof(buf), ONE_KILOBYTE);
  OK(strncmp(buf, "0", sizeof(buf)) == 0);

  cg2_pretty_size(buf, sizeof(buf), ONE_MEGABYTE);
  OK(strncmp(buf, "1", sizeof(buf)) == 0);

  cg2_pretty_size(buf, sizeof(buf), ONE_GIGABYTE);
  OK(strncmp(buf, "1024", sizeof(buf)) == 0);

  return 0;
}

/* --------------------------------------------------------------------------
 */
DEF_TEST(cg2_format_cgroup) {
  char internal[256];
  char pretty[256];

  cg2_format_cgroup("/cpuacct.stat", internal, STATIC_ARRAY_SIZE(internal));
  cg2_format_cgroup_for_notification(internal, pretty,
                                     STATIC_ARRAY_SIZE(pretty));
  OK(strncmp(internal, "__", sizeof(internal)) == 0);
  OK(strncmp(pretty, "/", sizeof(pretty)) == 0);

  cg2_format_cgroup("/a.slice/b.slice/cpuacct.stat", internal,
                    STATIC_ARRAY_SIZE(internal));
  cg2_format_cgroup_for_notification(internal, pretty,
                                     STATIC_ARRAY_SIZE(pretty));
  OK(strncmp(internal, "a__b", sizeof(internal)) == 0);
  OK(strncmp(pretty, "a/b", sizeof(pretty)) == 0);

  cg2_format_cgroup("/a_b.slice/cpuacct.stat", internal,
                    STATIC_ARRAY_SIZE(internal));
  cg2_format_cgroup_for_notification(internal, pretty,
                                     STATIC_ARRAY_SIZE(pretty));
  OK(strncmp(internal, "a_b", sizeof(internal)) == 0);
  OK(strncmp(pretty, "a_b", sizeof(pretty)) == 0);

  cg2_format_cgroup("/a:b.slice/cpuacct.stat", internal,
                    STATIC_ARRAY_SIZE(internal));
  cg2_format_cgroup_for_notification(internal, pretty,
                                     STATIC_ARRAY_SIZE(pretty));
  OK(strncmp(internal, "a_b", sizeof(internal)) == 0);
  OK(strncmp(pretty, "a_b", sizeof(pretty)) == 0);

  cg2_format_cgroup("/a.slice/b\\x2dc/d", internal,
                    STATIC_ARRAY_SIZE(internal));
  cg2_format_cgroup_for_notification(internal, pretty,
                                     STATIC_ARRAY_SIZE(pretty));
  OK(strncmp(internal, "a__b-c", sizeof(internal)) == 0);
  OK(strncmp(pretty, "a/b-c", sizeof(pretty)) == 0);

  return 0;
}

/* **************************************************************************
 */
/* main */
/* **************************************************************************
 */

int main(void) {
  RUN_TEST(handle_file);
  RUN_TEST(handle_file_multi);
  RUN_TEST(load_config);
  RUN_TEST(plugin_config);
  RUN_TEST(read_sys_fs_cgroup);
  RUN_TEST(read_cpu_pressure);
  RUN_TEST(pretty_size);
  RUN_TEST(cg2_format_cgroup);

  printf("failures: %d", fail_count__);
  END_TEST;
}
