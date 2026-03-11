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

#include "liboconfig/oconfig.h"
#include "processes2.c"
#include "testing.h"

#include <stdio.h>

const char *test_config_plain = "LoadPlugin processes2\n"
                                "<Plugin processes2>\n"
                                "  #ProcessMatch name cmd_line user_name\n"
                                "  #ProcessMatch root \"\" root\n"
                                "  #ProcessMatch fbielig \"\" fbielig\n"
                                "  ProcessMatch \"user-%U\" \".*\"\n"
                                "</Plugin>\n";

const char *test_config_program = "<ProcessGroup user_cluster>\n"
                                  "  ProcessMatch \"program-%N\" \".*\"\n"
                                  "</ProcessGroup>\n";

const char *test_config_user = "<ProcessGroup user_cluster>\n"
                               "  ProcessMatch \"user-%U\" \".*\"\n"
                               "</ProcessGroup>\n"
                               "#ProcessMatch root \"\" root\n";

const char *test_config_cgroup = "<ProcessGroup cgroup>\n"
                                 "  CollectCpuRank true\n"
                                 "  ProcessMatch \"cgroup-%G\" \".*\"\n"
                                 "</ProcessGroup>\n";

const char *test_config_dynamic = "<ProcessGroup user_cluster>\n"
                                  "  ProcessMatch \"%U-%N\" \".*\"\n"
                                  "</ProcessGroup>\n"
                                  "<ProcessGroup cgroup_cluster>\n"
                                  "  ProcessMatch \"cgroup-%G\" \".*\"\n"
                                  "</ProcessGroup>\n";

const char *test_config_top = "<ProcessGroup program>\n"
                              "  CollectProportionalSetSize true\n"
                              "  CollectCpuRank true\n"
                              "  CollectCpuPercent absolute\n"
                              "  ProcessMatch \"%N\" \".*\"\n"
                              "</ProcessGroup>\n"
                              "<Chain PostCache>\n"
                              "  <Rule toponly>\n"
                              "    <Match other_value>\n"
                              "      Type ps_cpu_rank\n"
                              "      Max 10\n"
                              "    </Match>\n"
                              "    Target stop\n"
                              "  </Rule>\n"
                              "</Chain>\n"
                              "LoadPlugin csv\n"
                              "<Plugin csv>\n"
                              "  DataDir \"/tmp/collectd\"\n"
                              "  StoreRates false\n"
                              "</Plugin>\n";

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
    printf("%s", data);
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
DEF_TEST(load_config) {
  oconfig_item_t *cfg = NULL;
  CHECK_ZERO(get_config(test_config_plain, &cfg));

  CHECK_ZERO(del_config(&cfg));
  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(plugin_config) {
  oconfig_item_t *cfg = NULL;
  CHECK_ZERO(get_config(test_config_user, &cfg));

  p2_config(cfg);
  p2_init();
  for (int co = 0; co < 5; ++co)
    p2_read();
  p2_shutdown();

  CHECK_ZERO(del_config(&cfg));
  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(cgroup_config) {
  oconfig_item_t *cfg = NULL;
  CHECK_ZERO(get_config(test_config_cgroup, &cfg));

  char buf[256];
  p2_format_cgroup("/abc.slice/abc-def", buf, sizeof(buf));
  CHECK_ZERO(strcmp(buf, "abc__def"));

  p2_config(cfg);
  p2_init();
  for (int co = 0; co < 10; ++co)
    p2_read();
  p2_shutdown();

  CHECK_ZERO(del_config(&cfg));
  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(dynamic_config) {
  oconfig_item_t *cfg = NULL;
  CHECK_ZERO(get_config(test_config_dynamic, &cfg));

  p2_config(cfg);
  p2_init();
  for (int co = 0; co < 2; ++co) {
    printf("= %3d =======================================\n", co);
    p2_read();
  }

  p2_shutdown();

  CHECK_ZERO(del_config(&cfg));
  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(top_filter) {
  oconfig_item_t *cfg = NULL;
  CHECK_ZERO(get_config(test_config_top, &cfg));

  p2_config(cfg);
  p2_init();
  for (int co = 0; co < 3; ++co) {
    printf("= %3d =======================================\n", co);
    p2_read();
  }

  p2_shutdown();

  CHECK_ZERO(del_config(&cfg));
  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(all_processes_status) {
  struct dirent *ent;
  DIR *proc;
  long pid = 0;
  process_entry_t ps;

  if ((proc = opendir("/proc")) == NULL) {
    ERROR("Cannot open '/proc': %s", STRERRNO);
    return -1;
  }

  while ((ent = readdir(proc)) != NULL) {
    if (!isdigit(ent->d_name[0]))
      continue;

    if ((pid = atol(ent->d_name)) < 1)
      continue;

    p2_read_status(pid, &ps);
  }
  CHECK_ZERO(closedir(proc));

  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(all_processes_pss) {
  struct dirent *ent;
  DIR *proc;
  process_entry_t pse;

  if ((proc = opendir("/proc")) == NULL) {
    ERROR("Cannot open '/proc': %s", STRERRNO);
    return -1;
  }

  while ((ent = readdir(proc)) != NULL) {
    if (!isdigit(ent->d_name[0]))
      continue;

    if ((pse.id = atol(ent->d_name)) < 1)
      continue;

    if (pse.id != getpid())
      continue;

    if (p2_read_pss_info(&pse) == 0) {
      derive_t pss_sum = pse.pss_anon + pse.pss_file + pse.pss_shmem;
      // printf("pid: %lu (%ld <=> %ld)\n", pse.id, pse.pss, sum);
      OK(labs(pse.pss - pss_sum) <= 1);
    }
  }
  CHECK_ZERO(closedir(proc));

  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(pretty_size) {

  const unsigned long ONE_KILOBYTE = 1ul << 10;
  const unsigned long ONE_MEGABYTE = 1ul << 20;
  const unsigned long ONE_GIGABYTE = 1ul << 30;

  char buf[64];

  p2_pretty_size(buf, sizeof(buf), ONE_KILOBYTE);
  OK(strncmp(buf, "1.0KB", sizeof(buf)) == 0);

  p2_pretty_size(buf, sizeof(buf), ONE_MEGABYTE);
  OK(strncmp(buf, "1.0MB", sizeof(buf)) == 0);

  p2_pretty_size(buf, sizeof(buf), ONE_GIGABYTE);
  OK(strncmp(buf, "1.0GB", sizeof(buf)) == 0);

  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(memdiff_order) {
  procstat_t *p1[] = {
      &(procstat_t){
          .name = "a", .vmem_rss = 100, .vmem_rss_last = 200}, /* -100 */
      &(procstat_t){
          .name = "b", .vmem_rss = 200, .vmem_rss_last = 100}, /*  100 */
      &(procstat_t){
          .name = "c", .vmem_rss = 300, .vmem_rss_last = 500}, /* -200 */
      &(procstat_t){
          .name = "d", .vmem_rss = 400, .vmem_rss_last = 400}, /*  000 */
      &(procstat_t){
          .name = "e", .vmem_rss = 500, .vmem_rss_last = 000}, /*  500 */
      &(procstat_t){
          .name = "f", .vmem_rss = 600, .vmem_rss_last = 900}, /* -300 */
      &(procstat_t){
          .name = "g", .vmem_rss = 700, .vmem_rss_last = 500}, /*  200 */
      &(procstat_t){
          .name = "h", .vmem_rss = 800, .vmem_rss_last = 950}, /* -150 */
      &(procstat_t){
          .name = "i", .vmem_rss = 900, .vmem_rss_last = 1400}, /* -500 */
  };

  size_t p1size = STATIC_ARRAY_SIZE(p1);
  qsort(p1, p1size, sizeof(procstat_t *), sort_vmem_rssdiff);

  OK(p1[0]->name[0] == 'e'); /* -500 */
  OK(p1[1]->name[0] == 'i'); /*  500 */
  OK(p1[2]->name[0] == 'f'); /* -300 */
  OK(p1[3]->name[0] == 'g'); /*  200 */
  OK(p1[4]->name[0] == 'c'); /* -200 */
  OK(p1[5]->name[0] == 'h'); /* -150 */
  OK(p1[6]->name[0] == 'b'); /*  100 */
  OK(p1[7]->name[0] == 'a'); /* -100 */
  OK(p1[8]->name[0] == 'd'); /*    0 */

  return 0;
}

/* -------------------------------------------------------------------------- */
/*
24047 root      0:00 sshd: root@pts/0
24948 root      0:00 sshd: root@pts/1
25788 root      0:00 sshd: root@pts/2
*/
DEF_TEST(parsing_cmdline) {
  oconfig_item_t *cfg = NULL;
  CHECK_ZERO(get_config(test_config_program, &cfg));

  p2_config(cfg);
  p2_init();
  for (int co = 0; co < 5; ++co) {
    p2_read();
    sleep(1);
  }
  p2_shutdown();

  CHECK_ZERO(del_config(&cfg));
  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(process_classification1) {

  cdtime_t now = cdtime();

  proc_cluster_t *cluster = p2_cluster_register("test");
  p2_statlist_register(cluster, "%N", ".*", NULL, NULL, true);

  process_entry_t pe_tu = {.id = 1, .name = "tuner"};
  process_entry_t pe_tc = {.id = 2, .name = "tuner-common"};

  p2_cluster_add(now, pe_tu.name, "/usr/bin/tuner", "nobody", "cg1", &pe_tu);
  p2_cluster_add(now, pe_tc.name, "/usr/bin/tuner-common", "nobody", "cg1",
                 &pe_tc);

  CHECK_NOT_NULL(cluster->procs);
  CHECK_NOT_NULL(cluster->procs->next);
  CHECK_NOT_NULL(cluster->procs->next->next);
  CHECK_ZERO(cluster->procs->next->next->next);

  p2_cluster_destroy();

  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(process_classification2) {

  cdtime_t now = cdtime();

  proc_cluster_t *cluster = p2_cluster_register("test");
  p2_statlist_register(cluster, "%N", ".*", NULL, NULL, true);

  process_entry_t pe_i1 = {.id = 1, .name = "idle_inject/1"};
  process_entry_t pe_i2 = {.id = 2, .name = "idle_inject/2"};

  p2_cluster_add(now, pe_i1.name, NULL, "root", "cg1", &pe_i1);
  p2_cluster_add(now, pe_i2.name, NULL, "root", "cg1", &pe_i2);

  CHECK_NOT_NULL(cluster->procs);
  CHECK_NOT_NULL(cluster->procs->next);
  CHECK_ZERO(cluster->procs->next->next);

  p2_cluster_destroy();

  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(process_classification3) {

  cdtime_t now = cdtime();

  proc_cluster_t *cluster = p2_cluster_register("test");
  p2_statlist_register(cluster, "%N", ".*", NULL, NULL, true);

  process_entry_t pe_a1 = {
      .id = 3, .name = "com.mercedes.adapter", .cpu_user_counter = 1000};
  process_entry_t pe_a2 = {
      .id = 4, .name = "com.mercedes.adapter:loader", .cpu_user_counter = 2000};

  p2_cluster_add(now, pe_a1.name, pe_a1.name, "root", "cg1", &pe_a1);
  p2_cluster_add(now, pe_a2.name, pe_a2.name, "root", "cg1", &pe_a2);

  CHECK_NOT_NULL(cluster->procs);
  CHECK_NOT_NULL(cluster->procs->next);
  CHECK_NOT_NULL(cluster->procs->next->next);
  CHECK_ZERO(cluster->procs->next->next->next);
  p2_cluster_submit(now);
  p2_cluster_destroy();

  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(process_classification4) {

  cdtime_t now = cdtime();

  proc_cluster_t *cluster = p2_cluster_register("test");
  p2_statlist_register(cluster, "%N", ".*", NULL, NULL, true);

  process_entry_t pe_a11 = {
      .id = 11,
      .name = "com.microsoft.windowsintune.companyportal:omadm_client_process"};
  process_entry_t pe_a12 = {
      .id = 12,
      .name = "com.microsoft.windowsintune.companyportal.omadm_client_process"};

  p2_cluster_add(now, pe_a11.name, pe_a11.name, "root", "cg1", &pe_a11);
  p2_cluster_add(now, pe_a12.name, pe_a12.name, "root", "cg1", &pe_a12);

  CHECK_NOT_NULL(cluster->procs);
  CHECK_NOT_NULL(cluster->procs->next);
  CHECK_ZERO(cluster->procs->next->next);

  p2_cluster_submit(now);
  p2_cluster_destroy();

  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(list_pids) {
  char buffer[256];
  char *bufp;
  size_t bufs;

  procstat_entry_t e9 = {
      .id = 9, .cpu_user_counter = 9000, .cpu_system_counter = 9, .next = NULL};
  procstat_entry_t e8 = {
      .id = 8, .cpu_user_counter = 1000, .cpu_system_counter = 8, .next = &e9};
  procstat_entry_t e7 = {
      .id = 7, .cpu_user_counter = 7000, .cpu_system_counter = 7, .next = &e8};
  procstat_entry_t e6 = {
      .id = 6, .cpu_user_counter = 1000, .cpu_system_counter = 6, .next = &e7};
  procstat_entry_t e5 = {
      .id = 5, .cpu_user_counter = 5000, .cpu_system_counter = 5, .next = &e6};
  procstat_entry_t e4 = {
      .id = 4, .cpu_user_counter = 1000, .cpu_system_counter = 4, .next = &e5};
  procstat_entry_t e3 = {
      .id = 3, .cpu_user_counter = 3000, .cpu_system_counter = 3, .next = &e4};
  procstat_entry_t e2 = {
      .id = 2, .cpu_user_counter = 1000, .cpu_system_counter = 2, .next = &e3};
  procstat_entry_t e1 = {
      .id = 1, .cpu_user_counter = 1000, .cpu_system_counter = 1, .next = &e2};
  procstat_entry_t e0 = {
      .id = 0, .cpu_user_counter = 0, .cpu_system_counter = 0, .next = &e1};

  bufp = buffer;
  bufs = sizeof(buffer);
  EXPECT_EQ_INT(p2_list_pids(&bufp, &bufs, &e0, 10), 0);
  EXPECT_EQ_STR(buffer, "9,7,5,3,8,6,4,2,1");
  bufp = buffer;
  bufs = sizeof(buffer);
  EXPECT_EQ_INT(p2_list_pids(&bufp, &bufs, &e0, 5), 0);
  EXPECT_EQ_STR(buffer, "9,7,5,3,8,...");
  bufp = buffer;
  bufs = sizeof(buffer);
  EXPECT_EQ_INT(p2_list_pids(&bufp, &bufs, &e0, 1), 0);
  EXPECT_EQ_STR(buffer, "9,...");
  bufp = buffer;
  bufs = sizeof(buffer);
  EXPECT_EQ_INT(p2_list_pids(&bufp, &bufs, &e9, 10), 0);
  EXPECT_EQ_STR(buffer, "9");
  bufp = buffer;
  bufs = 10;
  EXPECT_EQ_INT(p2_list_pids(&bufp, &bufs, &e0, 10), 0);
  EXPECT_EQ_STR(buffer, "9,7,5,3,8");
  printf("%s", buffer);

  return 0;
}

/* ************************************************************************** */
/* main */
/* ************************************************************************** */

int main(void) {
  RUN_TEST(load_config);
  RUN_TEST(plugin_config);
  RUN_TEST(cgroup_config);
  RUN_TEST(dynamic_config);
  RUN_TEST(top_filter);
  RUN_TEST(all_processes_status);
  RUN_TEST(all_processes_pss);
  RUN_TEST(pretty_size);
  RUN_TEST(parsing_cmdline);
  RUN_TEST(process_classification1);
  RUN_TEST(process_classification2);
  RUN_TEST(process_classification3);
  RUN_TEST(process_classification4);
  RUN_TEST(memdiff_order);
  RUN_TEST(list_pids);

  printf("failures: %d", fail_count__);
  END_TEST;
}
