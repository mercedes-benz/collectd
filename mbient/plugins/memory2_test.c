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
#include "memory2.c"
#include "testing.h"

#include <stdio.h>

const char *test_config_plain = "LoadPlugin memory2\n"
                                "<Plugin memory2>\n"
                                "  #ProcessMatch name cmd_line user_name\n"
                                "  #ProcessMatch root \"\" root\n"
                                "  #ProcessMatch fbielig \"\" fbielig\n"
                                "  ProcessMatch \"user-%U\" \".*\"\n"
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

int plugin_dispatch_multivalue(value_list_t const *vl, bool store_percentage,
                               int store_type, ...) {
  return 0;
}

/* ************************************************************************** */
/* tests */
/* ************************************************************************** */

/* -------------------------------------------------------------------------- */
DEF_TEST(plugin_config) {
  oconfig_item_t *cfg = NULL;
  CHECK_ZERO(get_config(test_config_plain, &cfg));

  m2_config(cfg);
  m2_init();

  OK(memory_zram == false);

  CHECK_ZERO(del_config(&cfg));
  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(parse_pressure) {
  char *buf = strdup("some avg10=0.42 avg60=0.00 avg300=0.00 total=2186726\n"
                     "full avg10=0.00 avg60=0.00 avg300=0.00 total=2112751\n");

  pressure_stats_t stats;
  m2_parse_pressure_values(&stats, buf);
  free(buf);

  EXPECT_EQ_DOUBLE(stats.some_avg10, 0.42);
  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(parse_vmstat) {
  char *buf = strdup("nr_free_pages 483819\n"                      //
                     "nr_zone_inactive_anon 460456\n"              //
                     "nr_zone_active_anon 4619580\n"               //
                     "nr_zone_inactive_file 5513441\n"             //
                     "nr_zone_active_file 678548\n"                //
                     "nr_zone_unevictable 347272\n"                //
                     "nr_zone_write_pending 3764\n"                //
                     "nr_mlock 26350\n"                            //
                     "nr_bounce 0\n"                               //
                     "nr_zspages 0\n"                              //
                     "nr_free_cma 0\n"                             //
                     "nr_unaccepted 0\n"                           //
                     "numa_hit 3147784622\n"                       //
                     "numa_miss 0\n"                               //
                     "numa_foreign 0\n"                            //
                     "numa_interleave 9518\n"                      //
                     "numa_local 3147674002\n"                     //
                     "numa_other 0\n"                              //
                     "nr_inactive_anon 460456\n"                   //
                     "nr_active_anon 4619580\n"                    //
                     "nr_inactive_file 5513441\n"                  //
                     "nr_active_file 678548\n"                     //
                     "nr_unevictable 347272\n"                     //
                     "nr_slab_reclaimable 3458003\n"               //
                     "nr_slab_unreclaimable 435524\n"              //
                     "nr_isolated_anon 0\n"                        //
                     "nr_isolated_file 0\n"                        //
                     "workingset_nodes 186530\n"                   //
                     "workingset_refault_anon 11653\n"             //
                     "workingset_refault_file 6691252\n"           //
                     "workingset_activate_anon 11653\n"            //
                     "workingset_activate_file 9214\n"             //
                     "workingset_restore_anon 0\n"                 //
                     "workingset_restore_file 75421\n"             //
                     "workingset_nodereclaim 128378\n"             //
                     "nr_anon_pages 5136524\n"                     //
                     "nr_mapped 804915\n"                          //
                     "nr_file_pages 6485860\n"                     //
                     "nr_dirty 3764\n"                             //
                     "nr_writeback 0\n"                            //
                     "nr_writeback_temp 0\n"                       //
                     "nr_shmem 738124\n"                           //
                     "nr_shmem_hugepages 0\n"                      //
                     "nr_shmem_pmdmapped 0\n"                      //
                     "nr_file_hugepages 0\n"                       //
                     "nr_file_pmdmapped 0\n"                       //
                     "nr_anon_transparent_hugepages 0\n"           //
                     "nr_vmscan_write 36802\n"                     //
                     "nr_vmscan_immediate_reclaim 0\n"             //
                     "nr_dirtied 89416507\n"                       //
                     "nr_written 86696848\n"                       //
                     "nr_throttled_written 0\n"                    //
                     "nr_kernel_misc_reclaimable 0\n"              //
                     "nr_foll_pin_acquired 17981\n"                //
                     "nr_foll_pin_released 17981\n"                //
                     "nr_kernel_stack 69600\n"                     //
                     "nr_page_table_pages 63459\n"                 //
                     "nr_sec_page_table_pages 4024\n"              //
                     "nr_iommu_pages 4024\n"                       //
                     "nr_swapcached 5994\n"                        //
                     "pgpromote_success 0\n"                       //
                     "pgpromote_candidate 0\n"                     //
                     "pgdemote_kswapd 0\n"                         //
                     "pgdemote_direct 0\n"                         //
                     "pgdemote_khugepaged 0\n"                     //
                     "nr_hugetlb 0\n"                              //
                     "nr_dirty_threshold 1311766\n"                //
                     "nr_dirty_background_threshold 655082\n"      //
                     "nr_memmap_pages 0\n"                         //
                     "nr_memmap_boot_pages 260608\n"               //
                     "pgpgin 64995196\n"                           //
                     "pgpgout 396619421\n"                         //
                     "pswpin 12745\n"                              //
                     "pswpout 35275\n"                             //
                     "pgalloc_dma 1024\n"                          //
                     "pgalloc_dma32 29713988\n"                    //
                     "pgalloc_normal 3273378127\n"                 //
                     "pgalloc_movable 0\n"                         //
                     "pgalloc_device 0\n"                          //
                     "allocstall_dma 0\n"                          //
                     "allocstall_dma32 0\n"                        //
                     "allocstall_normal 3076\n"                    //
                     "allocstall_movable 1105\n"                   //
                     "allocstall_device 0\n"                       //
                     "pgskip_dma 0\n"                              //
                     "pgskip_dma32 2563\n"                         //
                     "pgskip_normal 92143\n"                       //
                     "pgskip_movable 0\n"                          //
                     "pgskip_device 0\n"                           //
                     "pgfree 3337380749\n"                         //
                     "pgactivate 276008\n"                         //
                     "pgdeactivate 0\n"                            //
                     "pglazyfree 7238087\n"                        //
                     "pgfault 3101309367\n"                        //
                     "pgmajfault 95765\n"                          //
                     "pglazyfreed 796136\n"                        //
                     "pgrefill 3549586\n"                          //
                     "pgreuse 457801905\n"                         //
                     "pgsteal_kswapd 35157673\n"                   //
                     "pgsteal_direct 265731\n"                     //
                     "pgsteal_khugepaged 0\n"                      //
                     "pgscan_kswapd 35576468\n"                    //
                     "pgscan_direct 308771\n"                      //
                     "pgscan_khugepaged 0\n"                       //
                     "pgscan_direct_throttle 0\n"                  //
                     "pgscan_anon 71472\n"                         //
                     "pgscan_file 35813767\n"                      //
                     "pgsteal_anon 34622\n"                        //
                     "pgsteal_file 35388782\n"                     //
                     "zone_reclaim_success 0\n"                    //
                     "zone_reclaim_failed 0\n"                     //
                     "pginodesteal 0\n"                            //
                     "slabs_scanned 12257013\n"                    //
                     "kswapd_inodesteal 16188\n"                   //
                     "kswapd_low_wmark_hit_quickly 4321\n"         //
                     "kswapd_high_wmark_hit_quickly 36\n"          //
                     "pageoutrun 5058\n"                           //
                     "pgrotated 112011\n"                          //
                     "drop_pagecache 0\n"                          //
                     "drop_slab 0\n"                               //
                     "oom_kill 0\n"                                //
                     "numa_pte_updates 0\n"                        //
                     "numa_huge_pte_updates 0\n"                   //
                     "numa_hint_faults 0\n"                        //
                     "numa_hint_faults_local 0\n"                  //
                     "numa_pages_migrated 0\n"                     //
                     "pgmigrate_success 32933580\n"                //
                     "pgmigrate_fail 183273238\n"                  //
                     "thp_migration_success 0\n"                   //
                     "thp_migration_fail 0\n"                      //
                     "thp_migration_split 0\n"                     //
                     "compact_migrate_scanned 1488721346\n"        //
                     "compact_free_scanned 2624674755\n"           //
                     "compact_isolated 250283121\n"                //
                     "compact_stall 0\n"                           //
                     "compact_fail 0\n"                            //
                     "compact_success 0\n"                         //
                     "compact_daemon_wake 9942\n"                  //
                     "compact_daemon_migrate_scanned 1488721346\n" //
                     "compact_daemon_free_scanned 2624674755\n"    //
                     "htlb_buddy_alloc_success 0\n"                //
                     "htlb_buddy_alloc_fail 0\n"                   //
                     "unevictable_pgs_culled 402309750\n"          //
                     "unevictable_pgs_scanned 401421625\n"         //
                     "unevictable_pgs_rescued 401932255\n"         //
                     "unevictable_pgs_mlocked 1349853\n"           //
                     "unevictable_pgs_munlocked 1181009\n"         //
                     "unevictable_pgs_cleared 0\n"                 //
                     "unevictable_pgs_stranded 142494\n"           //
                     "thp_fault_alloc 0\n"                         //
                     "thp_fault_fallback 0\n"                      //
                     "thp_fault_fallback_charge 0\n"               //
                     "thp_collapse_alloc 0\n"                      //
                     "thp_collapse_alloc_failed 0\n"               //
                     "thp_file_alloc 0\n"                          //
                     "thp_file_fallback 0\n"                       //
                     "thp_file_fallback_charge 0\n"                //
                     "thp_file_mapped 0\n"                         //
                     "thp_split_page 0\n"                          //
                     "thp_split_page_failed 0\n"                   //
                     "thp_deferred_split_page 0\n"                 //
                     "thp_underused_split_page 0\n"                //
                     "thp_split_pmd 0\n"                           //
                     "thp_scan_exceed_none_pte 0\n"                //
                     "thp_scan_exceed_swap_pte 0\n"                //
                     "thp_scan_exceed_share_pte 0\n"               //
                     "thp_split_pud 0\n"                           //
                     "thp_zero_page_alloc 0\n"                     //
                     "thp_zero_page_alloc_failed 0\n"              //
                     "thp_swpout 0\n"                              //
                     "thp_swpout_fallback 0\n"                     //
                     "balloon_inflate 0\n"                         //
                     "balloon_deflate 0\n"                         //
                     "balloon_migrate 0\n"                         //
                     "swap_ra 6041\n"                              //
                     "swap_ra_hit 4380\n"                          //
                     "swpin_zero 140\n"                            //
                     "swpout_zero 1527\n"                          //
                     "ksm_swpin_copy 0\n"                          //
                     "cow_ksm 0\n"                                 //
                     "zswpin 0\n"                                  //
                     "zswpout 0\n"                                 //
                     "zswpwb 0\n"                                  //
                     "direct_map_level2_splits 520\n"              //
                     "direct_map_level3_splits 35\n"               //
                     "nr_unstable 0\n");

  vm_stats_t stats;
  m2_parse_vmstat_values(&stats, buf);
  free(buf);

  EXPECT_EQ_DOUBLE(stats.pgscan_direct, 308771);
  EXPECT_EQ_DOUBLE(stats.pgscan_direct_throttle, 0);
  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(parse_meminfo) {
  char *buf = strdup("MemTotal:       65263080 kB\n"    //
                     "MemFree:         1823056 kB\n"    //
                     "MemAvailable:   39682292 kB\n"    //
                     "Buffers:         3461432 kB\n"    //
                     "Cached:         22499224 kB\n"    //
                     "SwapCached:        23976 kB\n"    //
                     "Active:         21323004 kB\n"    //
                     "Inactive:       23950608 kB\n"    //
                     "Active(anon):   18629576 kB\n"    //
                     "Inactive(anon):  1841752 kB\n"    //
                     "Active(file):    2693428 kB\n"    //
                     "Inactive(file): 22108856 kB\n"    //
                     "Unevictable:     1403948 kB\n"    //
                     "Mlocked:          105400 kB\n"    //
                     "SwapTotal:       4194300 kB\n"    //
                     "SwapFree:        4102036 kB\n"    //
                     "Zswap:                 0 kB\n"    //
                     "Zswapped:              0 kB\n"    //
                     "Dirty:              6528 kB\n"    //
                     "Writeback:             0 kB\n"    //
                     "AnonPages:      20706044 kB\n"    //
                     "Mapped:          3210768 kB\n"    //
                     "Shmem:           2967832 kB\n"    //
                     "KReclaimable:   13833332 kB\n"    //
                     "Slab:           15575312 kB\n"    //
                     "SReclaimable:   13833332 kB\n"    //
                     "SUnreclaim:      1741980 kB\n"    //
                     "KernelStack:       69776 kB\n"    //
                     "PageTables:       254688 kB\n"    //
                     "SecPageTables:     16116 kB\n"    //
                     "NFS_Unstable:          0 kB\n"    //
                     "Bounce:                0 kB\n"    //
                     "WritebackTmp:          0 kB\n"    //
                     "CommitLimit:    36825840 kB\n"    //
                     "Committed_AS:   66352816 kB\n"    //
                     "VmallocTotal:   34359738367 kB\n" //
                     "VmallocUsed:      302928 kB\n"    //
                     "VmallocChunk:          0 kB\n"    //
                     "Percpu:            45024 kB\n"    //
                     "HardwareCorrupted:     0 kB\n"    //
                     "AnonHugePages:         0 kB\n"    //
                     "ShmemHugePages:        0 kB\n"    //
                     "ShmemPmdMapped:        0 kB\n"    //
                     "FileHugePages:         0 kB\n"    //
                     "FilePmdMapped:         0 kB\n"    //
                     "Unaccepted:            0 kB\n"    //
                     "HugePages_Total:       0\n"       //
                     "HugePages_Free:        0\n"       //
                     "HugePages_Rsvd:        0\n"       //
                     "HugePages_Surp:        0\n"       //
                     "Hugepagesize:       2048 kB\n"    //
                     "Hugetlb:               0 kB\n"    //
                     "DirectMap4k:     1094848 kB\n"    //
                     "DirectMap2M:    40263680 kB\n"    //
                     "DirectMap1G:    25165824 kB\n"    //
  );

  meminfo_stats_t stats;
  m2_parse_meminfo_values(&stats, buf);
  free(buf);

  EXPECT_EQ_DOUBLE(stats.mem_total, 66829393920);
  EXPECT_EQ_DOUBLE(stats.mem_free, 1866809344);
  EXPECT_EQ_DOUBLE(stats.mem_buffered, 3544506368);
  EXPECT_EQ_DOUBLE(stats.mem_cached, 23039205376);
  EXPECT_EQ_DOUBLE(stats.mem_available, 40634667008);
  EXPECT_EQ_DOUBLE(stats.mem_anon_pages, 21202989056);
  EXPECT_EQ_DOUBLE(stats.mem_mapped, 3287826432);
  EXPECT_EQ_DOUBLE(stats.mem_shmem, 3039059968);
  EXPECT_EQ_DOUBLE(stats.mem_slab_total, 15949119488);
  EXPECT_EQ_DOUBLE(stats.mem_slab_reclaimable, 14165331968);
  EXPECT_EQ_DOUBLE(stats.mem_slab_unreclaimable, 1783787520);
  EXPECT_EQ_DOUBLE(stats.mem_swap_total, 4294963200);
  EXPECT_EQ_DOUBLE(stats.mem_swap_free, 4200484864);
  EXPECT_EQ_DOUBLE(stats.mem_swap_cached, 24551424);

  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(parse_zram) {
  char *buf = strdup("34541568   714805  1146880        0  2146304     8074    "
                     "    0      117      117");

  zram_mem_t stats;
  m2_parse_zram_values(&stats, buf);
  free(buf);

  EXPECT_EQ_DOUBLE(stats.orig, 34541568);
  EXPECT_EQ_DOUBLE(stats.compr, 714805);
  EXPECT_EQ_DOUBLE(stats.used, 1146880);
  EXPECT_EQ_DOUBLE(stats.ratio, 3.32028933949959);

  return 0;
}

/* ************************************************************************** */
/* main */
/* ************************************************************************** */

int main(void) {
  RUN_TEST(plugin_config);

  RUN_TEST(parse_meminfo);
  RUN_TEST(parse_pressure);
  RUN_TEST(parse_vmstat);
  RUN_TEST(parse_zram);

  printf("failures: %d", fail_count__);
  END_TEST;
}
