/**
 * The MIT License
 *
 * Copyright (C) 2024 MBition GmbH
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
 *   Ravi Sankar P <ravi_sankar dot ponnurangam at mercedes-benz.com>
 *
 * Rewritten version of `memory` plugin.
 * Features:
 *   - support of logging of richos memory and pressure
 * Credits to the authors of `memory.c` for their inspiration.
 */

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

#define PLUGIN_NAME "memory2"
#define LOG_KEY PLUGIN_NAME " plugin: "

#define MEMINFO_BUFFER_SIZE 2048
#define ZRAM_BUFFER_SIZE 1024
#define VMSTAT_BUFFER_SIZE 8192
#define PRESSURE_BUFFER_SIZE 1024

#define _MB(mem) ((mem) / 1024 / 1024)

static const char *ZRAM_MEM_PATH = "/sys/block/zram0/mm_stat";
static const char *MEMINFO_PATH = "/proc/meminfo";
static const char *MEMORY_PRESSURE_PATH = "/proc/pressure/memory";
static const char *VMSTAT_PATH = "/proc/vmstat";

static bool values_absolute = true;
static bool values_percentage;
static bool extra_stats = false;
static bool memory_usage = false;
static bool memory_pressure = false;
static bool memory_zram = false;
static bool detailed_slab_info = false;
static bool pgscan = false;

typedef struct meminfo_stats_s {
  gauge_t mem_total;
  gauge_t mem_used;
  gauge_t mem_buffered;
  gauge_t mem_cached;
  gauge_t mem_free;
  gauge_t mem_available;
  gauge_t mem_anon_pages;
  gauge_t mem_mapped;
  gauge_t mem_shmem;
  gauge_t mem_slab_total;
  gauge_t mem_slab_reclaimable;
  gauge_t mem_slab_unreclaimable;
  gauge_t mem_swap_total;
  gauge_t mem_swap_free;
  gauge_t mem_swap_cached;
} meminfo_stats_t;

typedef struct zram_mem_s {
  gauge_t orig;
  gauge_t compr;
  gauge_t used;
  gauge_t ratio;
} zram_mem_t;

typedef struct pressure_stats_s {
  gauge_t some_avg10;
  // gauge_t some_avg60;
  // gauge_t some_avg300;
  // unsigned long some_total;
} pressure_stats_t;

typedef struct vm_stats_s {
  unsigned long pgscan_direct;
  unsigned long pgscan_direct_throttle;
} vm_stats_t;

static meminfo_stats_t memory_stats;
static pressure_stats_t mem_pressure_stats;
static zram_mem_t zram_stats;
static vm_stats_t vm_stats;

//==============================================================================
// I/O utils
//==============================================================================

//------------------------------------------------------------------------------
static ssize_t m2_read_file_content(const char *filename, void *buf,
                                    size_t bufsize) {
  char *buf_ptr;
  size_t len;
  errno = 0;

  int fd = open(filename, O_RDONLY);
  if (fd < 0) {
    if (errno != ENOENT)
      ERROR(LOG_KEY "Failed to open '%s': %s.", filename, STRERRNO);
    return -1;
  }

  buf_ptr = buf;
  len = bufsize;

  ssize_t n = 0;
  while (42) {
    ssize_t status = read(fd, (void *)buf_ptr, len);

    if (status < 0) {

      if ((EAGAIN == errno) || (EINTR == errno))
        continue;

      ERROR(LOG_KEY "Failed to read from '%s': %s.", filename, STRERRNO);
      close(fd);
      return -1;
    }

    n += status;

    if (status == 0)
      break;

    buf_ptr += status;
    len -= status;

    if (len == 0) {
      WARNING(LOG_KEY "Buffer too small when reading from '%s'.", filename);
      break;
    }
  }

  close(fd);

  return n;
}

//------------------------------------------------------------------------------
static ssize_t m2_read_text_file_content(const char *filename, char *buf,
                                         size_t bufsize) {
  ssize_t ret = m2_read_file_content(filename, buf, bufsize - 1);
  if (ret < 0)
    return ret;

  buf[ret] = '\0';
  return ret + 1;
}

//==============================================================================
// parse utils
//==============================================================================

//------------------------------------------------------------------------------
int m2_parse_meminfo_values(meminfo_stats_t *stats, char *buf) {
  char *line, *saveptr;
  int found = 0;

  line = strtok_r(buf, "\n", &saveptr);
  while (line != NULL && found < 15) {
    gauge_t *val = NULL;
    size_t off = 0;
    if (strncasecmp(line, "MemTotal:", 9) == 0) {
      val = &stats->mem_total;
      off = 9;
    } else if (strncasecmp(line, "MemFree:", 8) == 0) {
      val = &stats->mem_free;
      off = 8;
    } else if (strncasecmp(line, "Buffers:", 8) == 0) {
      val = &stats->mem_buffered;
      off = 8;
    } else if (strncasecmp(line, "Cached:", 7) == 0) {
      val = &stats->mem_cached;
      off = 7;
    } else if (strncasecmp(line, "MemAvailable:", 13) == 0) {
      val = &stats->mem_available;
      off = 13;
    } else if (strncasecmp(line, "AnonPages:", 10) == 0) {
      val = &stats->mem_anon_pages;
      off = 10;
    } else if (strncasecmp(line, "Mapped:", 7) == 0) {
      val = &stats->mem_mapped;
      off = 7;
    } else if (strncasecmp(line, "Shmem:", 6) == 0) {
      val = &stats->mem_shmem;
      off = 6;
    } else if (strncasecmp(line, "Slab:", 5) == 0) {
      val = &stats->mem_slab_total;
      off = 5;
    } else if (strncasecmp(line, "SReclaimable:", 13) == 0) {
      val = &stats->mem_slab_reclaimable;
      off = 13;
      detailed_slab_info = true;
    } else if (strncasecmp(line, "SUnreclaim:", 11) == 0) {
      val = &stats->mem_slab_unreclaimable;
      off = 11;
      detailed_slab_info = true;
    } else if (strncasecmp(line, "SwapTotal:", 10) == 0) {
      val = &stats->mem_swap_total;
      off = 10;
    } else if (strncasecmp(line, "SwapFree:", 9) == 0) {
      val = &stats->mem_swap_free;
      off = 9;
    } else if (strncasecmp(line, "SwapCached:", 11) == 0) {
      val = &stats->mem_swap_cached;
      off = 11;
    } else {
      line = strtok_r(NULL, "\n", &saveptr);
      continue;
    }

    *val = 1024.0 * atof(line + off);

    line = strtok_r(NULL, "\n", &saveptr);
  }

  return (found == 14) ? 0 : -1;
}

//------------------------------------------------------------------------------
int m2_read_meminfo_values(meminfo_stats_t *stats) {
  char buf[MEMINFO_BUFFER_SIZE];

  if (m2_read_text_file_content(MEMINFO_PATH, buf, sizeof(buf)) < 0) {
    return -1;
  }

  return m2_parse_meminfo_values(stats, buf);
}

//------------------------------------------------------------------------------
int m2_parse_pressure_values(pressure_stats_t *stats, char *buf) {
  char *line, *saveptr;
  char *fields[10];
  int numfields;

  line = strtok_r(buf, "\n", &saveptr);
  while (line != NULL) {
    if (strncmp(line, "some ", 5) == 0) {
      numfields = strsplit(buf, fields, STATIC_ARRAY_SIZE(fields));
      if (numfields < 2)
        continue;
      stats->some_avg10 = atof(fields[1] + 6);
      break;
    }
    line = strtok_r(NULL, "\n", &saveptr);
  }

  return 0;
}

//------------------------------------------------------------------------------
int m2_read_pressure_values(pressure_stats_t *stats) {
  char buf[PRESSURE_BUFFER_SIZE];

  if (!memory_pressure) {
    return 0;
  }

  if (m2_read_text_file_content(MEMORY_PRESSURE_PATH, buf, sizeof(buf)) < 0) {
    return -1;
  }

  return m2_parse_pressure_values(stats, buf);
}

//------------------------------------------------------------------------------
int m2_parse_vmstat_values(vm_stats_t *stats, char *buf) {
  char *line, *saveptr;
  int found = 0;

  line = strtok_r(buf, "\n", &saveptr);
  while (line != NULL && found < 2) {
    if (strncmp(line, "pgscan_direct ", 14) == 0) {
      stats->pgscan_direct = strtoul(line + 14, NULL, 10);
      found++;
    } else if (strncmp(line, "pgscan_direct_throttle ", 23) == 0) {
      stats->pgscan_direct_throttle = strtoul(line + 23, NULL, 10);
      found++;
    }
    line = strtok_r(NULL, "\n", &saveptr);
  }

  return (found == 2) ? 0 : -1;
}

//------------------------------------------------------------------------------
int m2_read_vmstat_values(vm_stats_t *stats) {
  char buf[VMSTAT_BUFFER_SIZE];

  if (!pgscan) {
    return 0;
  }

  if (m2_read_text_file_content(VMSTAT_PATH, buf, sizeof(buf)) < 0) {
    return -1;
  }

  return m2_parse_vmstat_values(stats, buf);
}

//------------------------------------------------------------------------------
int m2_parse_zram_values(zram_mem_t *stats, char *buf) {
  char *line, *saveptr;
  char *fields[10];
  int numfields;

  line = strtok_r(buf, "\n", &saveptr);
  while (line != NULL) {
    numfields = strsplit(buf, fields, STATIC_ARRAY_SIZE(fields));

    if (numfields >= 8) {
      stats->orig = atof(fields[0]);
      stats->compr = atof(fields[1]);
      stats->used = atof(fields[2]);
      stats->ratio = (stats->used * 100.0) / stats->orig;

      return 0;
    }

    line = strtok_r(NULL, "\n", &saveptr);
  }

  return -1;
}

//------------------------------------------------------------------------------
int m2_read_zram_values(zram_mem_t *stats) {
  char buf[ZRAM_BUFFER_SIZE];

  if (!memory_zram) {
    return 0;
  }

  if (m2_read_text_file_content(ZRAM_MEM_PATH, buf, sizeof(buf)) < 0) {
    return -1;
  }

  return m2_parse_zram_values(stats, buf);
}

//------------------------------------------------------------------------------
static void m2_check_zram_availablity() {

  memory_zram = true;
  if (m2_read_zram_values(&zram_stats) < 0) {
    memory_zram = false;
    INFO(LOG_KEY "zram is not available");
  } else {
    INFO(LOG_KEY "zram is available");
  }
}

//==============================================================================
// publishing utils
//==============================================================================

#define MEMORY_SUBMIT(...)                                                     \
  do {                                                                         \
    if (values_absolute)                                                       \
      plugin_dispatch_multivalue(vl, false, DS_TYPE_GAUGE, __VA_ARGS__, NULL); \
    if (values_percentage)                                                     \
      plugin_dispatch_multivalue(vl, true, DS_TYPE_GAUGE, __VA_ARGS__, NULL);  \
  } while (0)

//------------------------------------------------------------------------------
static int m2_notify(value_list_t *vl) {
#if KERNEL_LINUX
  notification_t n = {NOTIF_OKAY, cdtime(),    "", "",  PLUGIN_NAME,
                      "",         "mem_usage", "", NULL};
  char *msgp = n.message;
  size_t msgn = sizeof(n.message);
  char buf[256] = {0};
  char *bufp = buf;
  size_t bufn = sizeof(buf);
  int r = 0;
  if (memory_usage) {
    if (pgscan) {
      snprintf(msgp, msgn,
               "memory total %s - "
               "total/free/available/buffers/cached/shmem/slab/swaptotal/"
               "swapfree/swapcached (MB)/pgscan_direct/pgscan_direct_throttle",
               hostname_g);
      r = snprintf(
          bufp, bufn, "%.f/%.f/%.f/%.f/%.f/%.f/%.f/%.f/%.f/%.f/%lu/%lu",
          _MB(memory_stats.mem_total), _MB(memory_stats.mem_free),
          _MB(memory_stats.mem_available), _MB(memory_stats.mem_buffered),
          _MB(memory_stats.mem_cached), _MB(memory_stats.mem_shmem),
          _MB(memory_stats.mem_slab_total), _MB(memory_stats.mem_swap_total),
          _MB(memory_stats.mem_swap_free), _MB(memory_stats.mem_swap_cached),
          vm_stats.pgscan_direct, vm_stats.pgscan_direct_throttle);
    } else {
      snprintf(msgp, msgn,
               "memory total %s - "
               "total/free/available/buffers/cached/shmem/slab/swaptotal/"
               "swapfree/swapcached (MB)",
               hostname_g);
      r = snprintf(
          bufp, bufn, "%.f/%.f/%.f/%.f/%.f/%.f/%.f/%.f/%.f/%.f",
          _MB(memory_stats.mem_total), _MB(memory_stats.mem_free),
          _MB(memory_stats.mem_available), _MB(memory_stats.mem_buffered),
          _MB(memory_stats.mem_cached), _MB(memory_stats.mem_shmem),
          _MB(memory_stats.mem_slab_total), _MB(memory_stats.mem_swap_total),
          _MB(memory_stats.mem_swap_free), _MB(memory_stats.mem_swap_cached));
    }

    if (r < 0 || r > bufn) {
      ERROR(LOG_KEY "Error while adding memory usage to buffer");
      return -1;
    }

    if (plugin_notification_meta_add_string(&n, "", buf) != 0) {
      ERROR(LOG_KEY "Error adding meta information to notification.");
      return -1;
    }

    plugin_dispatch_notification(&n);
    if (n.meta != NULL)
      plugin_notification_meta_free(n.meta);
  }

  // special notification for compressed RAM-based block device
  if (memory_zram) {
    char buff[256] = {0};
    bufp = buff;
    bufn = sizeof(buff);
    notification_t z = {NOTIF_OKAY, cdtime(),   "", "",  PLUGIN_NAME,
                        "",         "mem_zram", "", NULL};
    msgp = z.message;
    msgn = sizeof(z.message);
    snprintf(msgp, msgn, "memory zram %s - orig/compr/used (MB)/ratio (%%)",
             hostname_g);
    r = snprintf(bufp, bufn, "%.f/%.f/%.f/%.f", _MB(zram_stats.orig),
                 _MB(zram_stats.compr), _MB(zram_stats.used), zram_stats.ratio);
    if (r < 0 || r > bufn) {
      ERROR(LOG_KEY "Error while adding zram memory usage to buffer");
      return -1;
    }

    if (plugin_notification_meta_add_string(&z, "", buff) != 0) {
      ERROR(LOG_KEY "Error adding meta information to notification.");
      return -1;
    }
    plugin_dispatch_notification(&z);
    if (z.meta != NULL)
      plugin_notification_meta_free(z.meta);
  }

  if (memory_pressure) {
    notification_t p = {NOTIF_OKAY, cdtime(),       "", "",  PLUGIN_NAME,
                        "",         "mem_pressure", "", NULL};
    msgp = p.message;
    msgn = sizeof(p.message);
    snprintf(msgp, msgn, "memory pressure %s: %.2f", hostname_g,
             mem_pressure_stats.some_avg10);

    plugin_dispatch_notification(&p);
    if (p.meta != NULL)
      plugin_notification_meta_free(p.meta);
  }

  if (memory_stats.mem_total <
      (memory_stats.mem_free + memory_stats.mem_buffered +
       memory_stats.mem_cached + memory_stats.mem_slab_total))
    return -1;

  memory_stats.mem_used =
      memory_stats.mem_total -
      (memory_stats.mem_free + memory_stats.mem_buffered +
       memory_stats.mem_cached + memory_stats.mem_slab_total);

  /* SReclaimable and SUnreclaim were introduced in kernel 2.6.19
   * They sum up to the value of Slab, which is available on older & newer
   * kernels. So SReclaimable/SUnreclaim are submitted if available, and Slab
   * if not. */
  if (detailed_slab_info)
    MEMORY_SUBMIT("used", memory_stats.mem_used, "buffered",
                  memory_stats.mem_buffered, "cached", memory_stats.mem_cached,
                  "free", memory_stats.mem_free, "slab_unrecl",
                  memory_stats.mem_slab_unreclaimable, "slab_recl",
                  memory_stats.mem_slab_reclaimable);
  else
    MEMORY_SUBMIT("used", memory_stats.mem_used, "buffered",
                  memory_stats.mem_buffered, "cached", memory_stats.mem_cached,
                  "free", memory_stats.mem_free, "slab",
                  memory_stats.mem_slab_total);
  if (extra_stats)
    MEMORY_SUBMIT("available", memory_stats.mem_available, "anon_pages",
                  memory_stats.mem_anon_pages, "mapped",
                  memory_stats.mem_mapped, "shmem", memory_stats.mem_shmem);
#endif

  return 0;
}

//==============================================================================
// collectd plugin interface
//==============================================================================

//------------------------------------------------------------------------------
static int m2_config(oconfig_item_t *ci) {
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;
    if (strcasecmp("ValuesAbsolute", child->key) == 0)
      cf_util_get_boolean(child, &values_absolute);
    else if (strcasecmp("ValuesPercentage", child->key) == 0)
      cf_util_get_boolean(child, &values_percentage);
    else if (strcasecmp("ExtraStats", child->key) == 0)
      cf_util_get_boolean(child, &extra_stats);
    else if (strcasecmp("MemoryUsage", child->key) == 0)
      cf_util_get_boolean(child, &memory_usage);
    else if (strcasecmp("MemoryPressure", child->key) == 0)
      cf_util_get_boolean(child, &memory_pressure);
    else if (strcasecmp("PageScan", child->key) == 0)
      cf_util_get_boolean(child, &pgscan);
    else
      ERROR("memory plugin: Invalid configuration option: "
            "\"%s\".",
            child->key);
  }

  return 0;
}

//------------------------------------------------------------------------------
static int m2_init(void) {

  m2_check_zram_availablity();

  return 0;
}

//------------------------------------------------------------------------------
static int m2_read(void) {
#if KERNEL_LINUX
  value_t v[1];
  value_list_t vl = VALUE_LIST_INIT;

  vl.values = v;
  vl.values_len = STATIC_ARRAY_SIZE(v);
  sstrncpy(vl.plugin, "memory2", sizeof(vl.plugin));
  sstrncpy(vl.type, "memory", sizeof(vl.type));
  vl.time = cdtime();

  m2_read_meminfo_values(&memory_stats);
  m2_read_pressure_values(&mem_pressure_stats);
  m2_read_zram_values(&zram_stats);
  m2_read_vmstat_values(&vm_stats);

  m2_notify(&vl);
#endif

  return 0;
}

//------------------------------------------------------------------------------
void module_register(void) {
  plugin_register_complex_config("memory2", m2_config);
  plugin_register_init("memory2", m2_init);
  plugin_register_read("memory2", m2_read);
} /* void module_register */
