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
 *
 * Rewritten version of `processes` plugin for Linux.
 * Features:
 *   - allows process groups
 *   - process filter support command line, user and cgroup
 *   - notification of top cpu and memory consumers
 *   - supports cpu percentages
 * Credits to the authors of `processes.c` for their inspiration.
 */

#include "collectd.h"

#include "plugin.h"
#include "utils/avltree/avltree.h"
#include "utils/common/common.h"

#define PLUGIN_NAME "processes2"
#define LOG_KEY PLUGIN_NAME " plugin: "

#if HAVE_LIBTASKSTATS
#include "utils/taskstats/taskstats.h"
#include "utils_complain.h"
#endif // HAVE_LIBTASKSTATS

#if KERNEL_LINUX
#if HAVE_LINUX_CONFIG_H
#include <linux/config.h>
#endif
#ifndef CONFIG_HZ
#define CONFIG_HZ 100
#endif // KERNEL_LINUX

#elif defined(__QNX__)
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/procfs.h>
#include <sys/time.h>
#else
#error "No applicable platform, only Linux and QNX are supported."
#endif

#if HAVE_REGEX_H
#include <regex.h>
#endif

#if HAVE_PWD_H
#include <pwd.h>
#endif

#if HAVE_KSTAT_H
#include <kstat.h>
#endif

#ifdef HAVE_SYS_CAPABILITY_H
#include <sys/capability.h>
#endif

#include <sys/sysinfo.h>

#ifndef CMDLINE_BUFFER_SIZE
#if defined(ARG_MAX) && (ARG_MAX < 4096)
#define CMDLINE_BUFFER_SIZE ARG_MAX
#else
#define CMDLINE_BUFFER_SIZE 4096
#endif
#endif

#define USER_NAME_BUFFER_SIZE DATA_MAX_NAME_LEN
#define CGROUP_BUFFER_SIZE DATA_MAX_NAME_LEN
#define PROCSTAT_NAME_LEN DATA_MAX_NAME_LEN
#define CLUSTER_NAME_LEN DATA_MAX_NAME_LEN
#define CGROUP_CATEGORY "name=systemd"

#define NUMERIC_CMP(v1, v2) ((v1 > v2) - (v1 < v2))

//------------------------------------------------------------------------------
#if !defined(__QNX__)
typedef struct process_entry_s {
  unsigned long id;
  char name[PROCSTAT_NAME_LEN];
  unsigned long uid;

  cdtime_t start_time;

  unsigned long num_proc;
  unsigned long num_lwp;
  unsigned long num_fd;
  unsigned long num_maps;
  unsigned long vmem_size;
  unsigned long vmem_rss;
  unsigned long vmem_data;
  unsigned long vmem_code;
  unsigned long vmem_swap;
  unsigned long stack_size;

  derive_t vmem_minflt_counter;
  derive_t vmem_majflt_counter;

  derive_t cpu_user_counter;
  derive_t cpu_system_counter;

  // io data
  derive_t io_rchar;
  derive_t io_wchar;
  derive_t io_syscr;
  derive_t io_syscw;
  derive_t io_diskr;
  derive_t io_diskw;
  bool has_io;

  derive_t cswitch_vol;
  derive_t cswitch_invol;
  bool has_cswitch;

#if HAVE_LIBTASKSTATS
  ts_delay_t delay;
#endif
  bool has_delay;
  bool has_fd;
  bool has_maps;

  // proportional set size
  derive_t pss;
  derive_t pss_anon;
  derive_t pss_file;
  derive_t pss_shmem;
  bool has_pss;
} process_entry_t;

//------------------------------------------------------------------------------
typedef struct procstat_entry_s {
  unsigned long id;
  unsigned char age;

  cdtime_t start_time;

  derive_t vmem_minflt_counter;
  derive_t vmem_majflt_counter;

  derive_t cpu_user_counter;
  derive_t cpu_system_counter;

  // io data
  derive_t io_rchar;
  derive_t io_wchar;
  derive_t io_syscr;
  derive_t io_syscw;
  derive_t io_diskr;
  derive_t io_diskw;

  derive_t cswitch_vol;
  derive_t cswitch_invol;

#if HAVE_LIBTASKSTATS
  value_to_rate_state_t delay_cpu;
  value_to_rate_state_t delay_blkio;
  value_to_rate_state_t delay_swapin;
  value_to_rate_state_t delay_freepages;
#endif

  // proportional set size
  derive_t pss;
  derive_t pss_anon;
  derive_t pss_file;
  derive_t pss_shmem;

  struct procstat_entry_s *next;
} procstat_entry_t;

//------------------------------------------------------------------------------
#define P2_CMP_FALSE 0
#define P2_CMP_TRUE 1
#define P2_CMP_REGEX 2
#define P2_CMP_EXACT 3
#define P2_CMP_START 4

typedef struct p2_pattern_s {
#if HAVE_REGEX_H
  regex_t *re;
#endif
  char *str;
  size_t len;
  int cmp;
} p2_pattern_t;

#endif // QNX

//------------------------------------------------------------------------------
typedef struct procstat {
  char name[PROCSTAT_NAME_LEN];
#if !defined(__QNX__)
  cdtime_t start_time;

  p2_pattern_t cmd_pattern;
  p2_pattern_t user_pattern;
  p2_pattern_t cgroup_pattern;
  unsigned long num_proc;
  unsigned long num_lwp;
  unsigned long num_fd;
  unsigned long num_maps;
  unsigned long vmem_size;
  unsigned long vmem_size_last;
  unsigned long vmem_rss;
  unsigned long vmem_rss_last;
  unsigned long vmem_data;
  unsigned long vmem_code;
  unsigned long vmem_swap;
  unsigned long vmem_swap_last;
  unsigned long stack_size;

  derive_t vmem_minflt_counter;
  derive_t vmem_majflt_counter;

  derive_t cpu_user_counter;
  derive_t cpu_user_last;
  derive_t cpu_system_counter;
  derive_t cpu_system_last;

  gauge_t cpu_user_percent_all;
  gauge_t cpu_user_percent_now;
  gauge_t cpu_system_percent_all;
  gauge_t cpu_system_percent_now;
  gauge_t cpu_total_percent_all;
  gauge_t cpu_total_percent_now;

  // ranking
  gauge_t cpu_user_rank_now;
  gauge_t cpu_system_rank_now;
  gauge_t cpu_total_rank_now;
  gauge_t cpu_user_rank_all;
  gauge_t cpu_system_rank_all;
  gauge_t cpu_total_rank_all;

  // io data
  derive_t io_rchar;
  derive_t io_rchar_last;
  derive_t io_wchar;
  derive_t io_wchar_last;
  derive_t io_syscr;
  derive_t io_syscw;
  derive_t io_diskr;
  derive_t io_diskw;

  derive_t cswitch_vol;
  derive_t cswitch_invol;

  // Linux delay accounting. unit is ns/s.
  gauge_t delay_cpu;
  gauge_t delay_blkio;
  gauge_t delay_swapin;
  gauge_t delay_freepages;

  // proportional set size
  derive_t pss;
  derive_t pss_anon;
  derive_t pss_file;
  derive_t pss_shmem;

  bool report_fd_num;
  bool report_maps_num;
  bool report_ctx_switch;
  bool report_delay;
  bool report_pss_info;

  // internal data
  bool hidden;
  struct procstat *next;
  struct procstat_entry_s *instances;
#else
  unsigned int num_thread;
  unsigned int num_fd;

  unsigned long long base_address;
  unsigned long long initial_stack;
  unsigned long long process_stack;

  unsigned long long cpu_user_time;
  unsigned long long cpu_system_time;

  bool report_fd_num;
  bool report_maps_num;
  bool report_ctx_switch;
  bool report_delay;

  struct procstat *next;
#endif
} procstat_t;

#if !defined(__QNX__)
//------------------------------------------------------------------------------
typedef struct proc_cluster_s {
  char name[CLUSTER_NAME_LEN];
  bool report_fd_num;
  bool report_maps_num;
  bool report_ctx_switch;
  bool report_delay;
  bool report_pss_info;
  bool report_cpu_rank;
  bool report_cpu_percent;
  bool report_cpu_relative;

  int notify_cpu_top;
  int notify_cpu_top_single_line;
  int notify_mem_top;
  int notify_memdiff_top;
  int notify_mem_top_single_line;
  int notify_io_top_read_single_line;
  int notify_io_top_write_single_line;

  bool needs_cmd;
  bool needs_user;
  bool needs_cgroup;

  procstat_t *procs;
  struct proc_cluster_s *next;
} proc_cluster_t;

//------------------------------------------------------------------------------
static proc_cluster_t *cluster_head_g = NULL;

static cdtime_t last_read_time = 0;
static int HERTZ = 0;
bool p2_config_called = false;

#endif

#if KERNEL_LINUX
static long pagesize_g;
static void p2_fill_details(const procstat_t *ps, process_entry_t *entry);
int getargs(void *processBuffer, int bufferLen, char *argsBuffer, int argsLen);
#endif /* KERNEL_LINUX */

#if HAVE_LIBTASKSTATS
static ts_t *taskstats_handle;
#endif

#if !defined(__QNX__)

//==============================================================================
// Forward
//==============================================================================

static void p2_submit_proc_list(cdtime_t now, proc_cluster_t *cluster,
                                procstat_t *ps);

//==============================================================================
// System utils
//==============================================================================

//------------------------------------------------------------------------------
static cdtime_t p2_get_uptime() {
  struct sysinfo info;

  if (sysinfo(&info) != 0) {
    ERROR("uptime plugin: Error calling sysinfo: %s", STRERRNO);
    return -1;
  }

  return TIME_T_TO_CDTIME_T(info.uptime);
}

//==============================================================================
// I/O utils
//==============================================================================

//------------------------------------------------------------------------------
static ssize_t p2_read_file_contents(const char *filename, void *buf,
                                     size_t bufsize) {
  char *buf_ptr;
  size_t len;
  errno = 0;

  int fd = open(filename, O_RDONLY);
  if (fd < 0) {
    if (errno != ENOENT)
      WARNING(LOG_KEY "Failed to open '%s': %s.", filename, STRERRNO);
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

      WARNING(LOG_KEY "Failed to read from '%s': %s.", filename, STRERRNO);
      close(fd);
      return -1;
    }

    n += status;

    if (status == 0)
      break;

    buf_ptr += status;
    len -= status;

    if (len == 0)
      break;
  }

  close(fd);

  return n;
}

//------------------------------------------------------------------------------
static ssize_t p2_read_text_file_contents(const char *filename, char *buf,
                                          size_t bufsize) {
  ssize_t ret = p2_read_file_contents(filename, buf, bufsize - 1);
  if (ret < 0)
    return ret;

  buf[ret] = '\0';
  return ret + 1;
}

//------------------------------------------------------------------------------
static derive_t p2_get_derive(const char **buf) {
  char *end;
  errno = 0;
  long ret = strtol(*buf, &end, 10);
  *buf = end;
  if (errno != 0)
    ret = -1;
  switch (*end) {
  case '\n':
  case '\0':
  case ' ':
  case '\t':
    return (derive_t)ret;
  default:
    return -1;
  }
}

//------------------------------------------------------------------------------
static derive_t p2_get_derive_bytes(const char **buf) {
  derive_t size = p2_get_derive(buf);
  derive_t fac = 1;

  if (size < 0)
    return size;
  if ((**buf != '\0') && isspace(**buf))
    ++*buf;

  if (**buf != '\0') {

    switch (tolower(**buf)) {
    case 'b':
      break;
    case 'k':
      fac = 1024;
      ++*buf;
      break;
    case 'm':
      fac *= 1024 * 1024;
      ++*buf;
      break;
    case 'g':
      fac *= 1024 * 1024 * 1024;
      ++*buf;
      break;
    }
  }
  if ((**buf != '\0') && ((**buf == 'b') || (**buf == 'B')))
    ++*buf;

  return fac * size;
}

//------------------------------------------------------------------------------
static unsigned long p2_get_ul(const char **buf) {
  char *end;
  unsigned long ret = strtoul(*buf, &end, 10);
  *buf = end;
  return ret;
}

//------------------------------------------------------------------------------
static unsigned long long p2_get_ull(const char **buf) {
  char *end;
  unsigned long long ret = strtoull(*buf, &end, 10);
  *buf = end;
  return ret;
}

//==============================================================================
// Format
//==============================================================================

//------------------------------------------------------------------------------
static void p2_pretty_size(char *buf, size_t sz, size_t bytes) {
  if (bytes >= 1024 * 1024 * 1024) {
    ssnprintf(buf, sz, "%.1fGB", (float)bytes / 1024 / 1024 / 1024);
  } else if (bytes >= 1024 * 1024) {
    ssnprintf(buf, sz, "%.1fMB", (float)bytes / 1024 / 1024);
  } else if (bytes >= 1024) {
    ssnprintf(buf, sz, "%.1fKB", (float)bytes / 1024);
  } else {
    ssnprintf(buf, sz, "%lu Bytes", bytes);
  }
}

//------------------------------------------------------------------------------
static void p2_pretty_ssize(char *buf, size_t sz, ssize_t bytes) {
  if (bytes < 0) {
    buf[0] = '-';
    --sz;
    ++buf;
    bytes = -bytes;
  }
  p2_pretty_size(buf, sz, bytes);
}

//------------------------------------------------------------------------------
static char *p2_format_cgroup(const char *cgroup, char *res, size_t res_len) {
  size_t res_co = 0;

  if (!cgroup || !*cgroup || (cgroup[1] == '\0') || (cgroup[1] == '\n')) {
    sstrncpy(res, "__", res_len);
  } else {
    const char *start = cgroup;
    if (*start == '/')
      ++start;
    const char *point = NULL;
    for (const char *ch = start; *ch; ++ch) {

      switch (*ch) {
      case '/':
        res[res_co++] = '_';
        res[res_co++] = '_';
        if (point == NULL)
          point = ch;
        size_t size = point - start;
        if (strncmp(start, ch + 1, point - start) == 0) {
          if (ch[size + 1] == '-')
            ch += size + 1;
        }
        start = ch + 1;
        point = NULL;
        break;
      case '.':
        point = ch;
        if (strncmp(ch + 1, "slice", 5) == 0)
          ch += 5;
        else if (strncmp(ch + 1, "scope", 5) == 0)
          ch += 5;
        else if (strncmp(ch + 1, "service", 7) == 0) {
          ch += 7;
          // go back until upper entry
          for (; res_co > 1; --res_co) {
            if ((res[res_co - 1] == '_') && (res[res_co - 2] == '_')) {
              res_co -= 2;
              break;
            }
          }
        } else
          res[res_co++] = '.';
        break;
      case '\\':
        if (strncmp(ch + 1, "x2d", 3) == 0) {
          ch += 3;
          res[res_co++] = '-';
        }
        break;
      case ':':
        res[res_co++] = '_';
        break;
      case '\n':
        break;
      default:
        res[res_co++] = *ch;
        point = NULL;
        break;
      }
      if (res_co >= res_len - 1)
        break;
    }
    res[res_co] = '\0';
  }
  // DEBUG(LOG_KEY "cgroup format:  '%s' --> '%s'", cgroup, res);

  return res;
}

//==============================================================================
// Pattern
//==============================================================================

//------------------------------------------------------------------------------
static int p2_pattern_create(p2_pattern_t *pattern, const char *input,
                             int cmp) {
  pattern->re = NULL;
  pattern->str = NULL;
  pattern->len = 0;
  pattern->cmp = cmp;

  if (input == NULL || *input == '\0') {
    pattern->cmp = P2_CMP_TRUE;
    return 0;
  }

  if ((input[0] == '.') && (input[1] == '*') && (input[2] == '\0')) {
    pattern->cmp = P2_CMP_TRUE;
    return 0;
  }

  if (input[0] == '=') {
    ++input;
    pattern->cmp = P2_CMP_EXACT;
  }

  if (input[0] == '>') {
    ++input;
    pattern->cmp = P2_CMP_START;
  }

  pattern->str = sstrdup(input);
  pattern->len = strlen(pattern->str);
  if (pattern->cmp != P2_CMP_REGEX)
    return 0;

#if HAVE_REGEX_H
  pattern->re = malloc(sizeof(*pattern->re));
  if (pattern->re == NULL) {
    WARNING(LOG_KEY "pattern create: malloc failed.");
    return -1;
  }

  int status = regcomp(pattern->re, input, REG_EXTENDED | REG_NOSUB);
  if (status != 0) {
    WARNING(LOG_KEY "compiling the regular expression \"%s\" failed.",
            input + 1);
    sfree(pattern->re);
    return -1;
  }

  return 0;
#else
  pattern->cmp = P2_CMP_FALSE;
  ERROR(LOG_KEY "pattern create: "
                "Regular expression \"%s\" found in config "
                "file, but support for regular expressions "
                "has been disabled at compile time.",
        input);
  return -1;
#endif
}

//------------------------------------------------------------------------------
static void p2_pattern_destroy(p2_pattern_t *pattern) {
  if (pattern->str)
    free(pattern->str);
  if (pattern->re) {
    regfree(pattern->re);
    free(pattern->re);
  }
}

//------------------------------------------------------------------------------
static bool p2_pattern_match(p2_pattern_t *pattern, const char *input) {
  switch (pattern->cmp) {
  case P2_CMP_FALSE:
    return false;
  case P2_CMP_TRUE:
    return true;
  case P2_CMP_REGEX:
    return regexec(pattern->re, input, 0, NULL, 0) == 0;
  case P2_CMP_EXACT:
    return strcmp(pattern->str, input) == 0;
  case P2_CMP_START:
    return strncmp(pattern->str, input, pattern->len) == 0;
  default:
    WARNING(LOG_KEY "unknown compare type %d", pattern->cmp);
    return false;
  }
}

//==============================================================================
// User Cache
//==============================================================================

//------------------------------------------------------------------------------
typedef struct p2_user_info_s {
  uid_t uid;
  char name[USER_NAME_BUFFER_SIZE];
} ps_user_info_t;

//------------------------------------------------------------------------------
c_avl_tree_t *p2_user_info_cache = NULL;

//------------------------------------------------------------------------------
static int p2_user_info_compare(const ps_user_info_t *u1,
                                const ps_user_info_t *u2) {
  assert(u1 != NULL);
  assert(u2 != NULL);
  if (u1->uid < u2->uid)
    return -1;
  if (u1->uid > u2->uid)
    return 1;
  return 0;
}

//------------------------------------------------------------------------------
void p2_user_info_cache_show() {
  if (p2_user_info_cache == NULL)
    return;

  ps_user_info_t *key;
  ps_user_info_t *user_info;

  DEBUG(LOG_KEY "user list:");
  c_avl_iterator_t *iter = c_avl_get_iterator(p2_user_info_cache);
  while (c_avl_iterator_next(iter, (void **)&key, (void **)&user_info) == 0) {
    DEBUG(LOG_KEY "  %u: %s", user_info->uid, user_info->name);
  }
  c_avl_iterator_destroy(iter);
}

//------------------------------------------------------------------------------
static void p2_user_info_cache_destroy() {
  if (p2_user_info_cache == NULL)
    return;

  ps_user_info_t *key;
  ps_user_info_t *user_info;

  c_avl_iterator_t *iter = c_avl_get_iterator(p2_user_info_cache);
  while (c_avl_iterator_next(iter, (void **)&key, (void **)&user_info) == 0)
    sfree(user_info);
  c_avl_iterator_destroy(iter);
  c_avl_destroy(p2_user_info_cache);

  p2_user_info_cache = NULL;
}

//------------------------------------------------------------------------------
static int p2_user_info_add(uid_t uid, const char *name) {
  assert(p2_user_info_cache != NULL);
  ps_user_info_t *user_info = calloc(1, sizeof(*user_info));
  if (user_info == NULL)
    return -1;
  user_info->uid = uid;
  sstrncpy(user_info->name, name, sizeof(user_info->name));
  if (c_avl_insert(p2_user_info_cache, user_info, user_info) != 0) {
    sfree(user_info);
    return -1;
  }
  return 0;
}

//------------------------------------------------------------------------------
static void p2_user_info_cache_init() {
  if (p2_user_info_cache != NULL)
    p2_user_info_cache_destroy();

  p2_user_info_cache =
      c_avl_create((int (*)(const void *, const void *))p2_user_info_compare);

  struct passwd *pwd;
  while ((pwd = getpwent()) != NULL) {
    p2_user_info_add(pwd->pw_uid, pwd->pw_name);
  }
  endpwent();

  // p2_user_info_cache_show();
}

//------------------------------------------------------------------------------
static const char *p2_user_info_get_name(uid_t uid) {
  if (p2_user_info_cache == NULL)
    return NULL;
  ps_user_info_t user_info;
  ps_user_info_t *result;
  user_info.uid = uid;
  if (c_avl_get(p2_user_info_cache, &user_info, (void **)&result) == 0)
    return result->name;
  return "unknown";
}

//==============================================================================
// Process information
//==============================================================================

//------------------------------------------------------------------------------
static char *p2_get_cmdline(long pid, char *name, char *buf, size_t buf_len,
                            bool with_args) {
  char file[PATH_MAX];
  ssize_t n;

  if ((pid < 1) || (NULL == buf) || (buf_len < 2))
    return NULL;

  ssnprintf(file, sizeof(file), "/proc/%li/cmdline", pid);

  n = p2_read_text_file_contents(file, buf, buf_len);

  if ((0 >= n) || (buf[0] == '\0')) {
    // cmdline not available; e.g. kernel thread, zombie
    if (NULL == name)
      return NULL;

    ssnprintf(buf, buf_len, "[%s]", name);
    return buf;
  }

  --n;
  // remove trailing whitespace
  while ((n > 0) && (isspace(buf[n]) || ('\0' == buf[n]))) {
    buf[n] = '\0';
    --n;
  }

  // arguments are separated by '\0' in /proc/<pid>/cmdline
  if (with_args) {
    while (n > 0) {
      if ('\0' == buf[n])
        buf[n] = '#';
      --n;
    }
  }

  return buf;
}

//------------------------------------------------------------------------------
static char *p2_get_username(pid_t pid, char *buf, size_t buf_len) {
  sstrncpy(buf, p2_user_info_get_name(pid), buf_len);
  return buf;
}

//------------------------------------------------------------------------------
static char *p2_get_cgroup(long pid, char *res, size_t res_len) {
  static const size_t cgroup_cat_len = strlen(CGROUP_CATEGORY) + 1;

  // DEBUG(LOG_KEY "get cgroup for pid %ld", pid);
  char filename[PATH_MAX];
  char buffer[1024];

  if ((pid < 1) || (NULL == res) || (res_len < 2))
    return NULL;
  res[0] = '\0';

  ssnprintf(filename, sizeof(filename), "/proc/%li/cgroup", pid);

  if (p2_read_text_file_contents(filename, buffer, sizeof(buffer)) <= 0)
    return res;

  for (const char *cptr = buffer; *cptr; cptr += strcspn(cptr, "\n") + 1) {
    char *name = strchr(cptr, ':');
    if (name != NULL) {
      // check for default slice
      strstripnewline(name);
      if (*(name + 1) == ':') {
        p2_format_cgroup(name + 2, res, res_len);
      }
      if (strncmp(name + 1, CGROUP_CATEGORY ":", cgroup_cat_len) == 0) {
        sstrncpy(res, name + cgroup_cat_len + 1, res_len);
        p2_format_cgroup(name + cgroup_cat_len + 1, res, res_len);
        break;
      }
    }
  }
  // DEBUG(LOG_KEY "cgroup of %lu: %s", pid, res);

  return res;
}

//==============================================================================
// Process Cache
//==============================================================================

//------------------------------------------------------------------------------
typedef struct p2_process_info_s {
  pid_t pid;
  cdtime_t timestamp;
  char cmdline[CMDLINE_BUFFER_SIZE];
  char cgroup[CGROUP_BUFFER_SIZE];
} p2_process_info_t;

//------------------------------------------------------------------------------
c_avl_tree_t *p2_process_info_cache = NULL;

//------------------------------------------------------------------------------
static int p2_process_info_compare(const p2_process_info_t *p1,
                                   const p2_process_info_t *p2) {
  assert(p1 != NULL);
  assert(p2 != NULL);
  if (p1->pid < p2->pid)
    return -1;
  if (p1->pid > p2->pid)
    return 1;
  return 0;
}

//------------------------------------------------------------------------------
void p2_process_info_cache_show() {
  if (p2_process_info_cache == NULL)
    return;

  p2_process_info_t *key;
  p2_process_info_t *process_info;
  unsigned process_num = 0;

  DEBUG(LOG_KEY "process list:");
  c_avl_iterator_t *iter = c_avl_get_iterator(p2_process_info_cache);
  while (c_avl_iterator_next(iter, (void **)&key, (void **)&process_info) ==
         0) {
    DEBUG(LOG_KEY "  %u: cmd=%s; cgroup=%s", process_info->pid,
          process_info->cmdline, process_info->cgroup);
    ++process_num;
  }
  c_avl_iterator_destroy(iter);
  DEBUG(LOG_KEY "found %u processes", process_num);
}

//------------------------------------------------------------------------------
static void p2_process_info_cache_cleanup(cdtime_t now) {
  if (p2_process_info_cache == NULL)
    return;

  const int MAX_CLEAN = 10;
  int clean_size = 0;
  p2_process_info_t *clean_keys[MAX_CLEAN];
  p2_process_info_t *key;
  p2_process_info_t *pi;

  c_avl_iterator_t *iter = c_avl_get_iterator(p2_process_info_cache);
  while (c_avl_iterator_next(iter, (void **)&key, (void **)&pi) == 0) {
    if (pi->timestamp >= now)
      continue;

    DEBUG(LOG_KEY "process cache: outdated process %u", pi->pid);
    clean_keys[clean_size++] = key;

    if (clean_size >= MAX_CLEAN)
      break;
  }
  c_avl_iterator_destroy(iter);

  for (int cco = 0; cco < clean_size; ++cco) {
    DEBUG(LOG_KEY "process cache: remove process %u", clean_keys[cco]->pid);
    c_avl_remove(p2_process_info_cache, clean_keys[cco], NULL, NULL);
    sfree(clean_keys[cco]);
  }
}

//------------------------------------------------------------------------------
static void p2_process_info_cache_destroy() {
  if (p2_process_info_cache == NULL)
    return;

  p2_process_info_t *key;
  p2_process_info_t *process_info;

  c_avl_iterator_t *iter = c_avl_get_iterator(p2_process_info_cache);
  while (c_avl_iterator_next(iter, (void **)&key, (void **)&process_info) == 0)
    sfree(process_info);
  c_avl_iterator_destroy(iter);
  c_avl_destroy(p2_process_info_cache);

  p2_process_info_cache = NULL;
}

//------------------------------------------------------------------------------
static void p2_process_info_cache_init() {
  if (p2_process_info_cache != NULL)
    p2_process_info_cache_destroy();

  p2_process_info_cache = c_avl_create(
      (int (*)(const void *, const void *))p2_process_info_compare);
}

//------------------------------------------------------------------------------
static p2_process_info_t *p2_process_info_add(pid_t pid, const char *cmdline,
                                              const char *cgroup) {
  assert(p2_process_info_cache != NULL);
  p2_process_info_t *process_info = calloc(1, sizeof(*process_info));
  if (process_info == NULL)
    return NULL;
  process_info->pid = pid;
  DEBUG(LOG_KEY "process cache: info add (%d, %s, %s)", pid, cmdline, cgroup);
  if (cmdline != NULL)
    sstrncpy(process_info->cmdline, cmdline, sizeof(process_info->cmdline));
  if (cgroup != NULL)
    sstrncpy(process_info->cgroup, cgroup, sizeof(process_info->cgroup));
  if (c_avl_insert(p2_process_info_cache, process_info, process_info) != 0) {
    sfree(process_info);
    return NULL;
  }
  return process_info;
}

//------------------------------------------------------------------------------
static p2_process_info_t *p2_process_info_get(pid_t pid) {
  if (p2_process_info_cache == NULL)
    return NULL;
  p2_process_info_t process_info;
  p2_process_info_t *result;
  process_info.pid = pid;
  if (c_avl_get(p2_process_info_cache, &process_info, (void **)&result) == 0)
    return result;
  return NULL;
}

//==============================================================================
// Statistic List
//==============================================================================

//------------------------------------------------------------------------------
// remove old entries from instances of processes in list_head_g
static void p2_statlist_entry_reset(procstat_t *ps) {

  ps->start_time = UINT64_MAX;

  ps->num_proc = 0;
  ps->num_lwp = 0;
  ps->num_fd = 0;
  ps->num_maps = 0;
  ps->vmem_size_last = ps->vmem_size;
  ps->vmem_size = 0;
  ps->vmem_rss_last = ps->vmem_rss;
  ps->vmem_rss = 0;
  ps->vmem_data = 0;
  ps->vmem_code = 0;
  ps->vmem_swap_last = ps->vmem_swap;
  ps->vmem_swap = 0;
  ps->stack_size = 0;
  ps->io_rchar_last = 0;
  ps->io_wchar_last = 0;
  ps->cpu_user_last = 0;
  ps->cpu_user_percent_now = 0;
  ps->cpu_user_percent_all = 0;
  ps->cpu_system_last = 0;
  ps->cpu_system_percent_now = 0;
  ps->cpu_system_percent_all = 0;

  ps->cpu_user_rank_now = -1;
  ps->cpu_system_rank_now = -1;
  ps->cpu_total_rank_now = -1;
  ps->cpu_user_rank_all = -1;
  ps->cpu_system_rank_all = -1;
  ps->cpu_total_rank_all = -1;

  ps->delay_cpu = NAN;
  ps->delay_blkio = NAN;
  ps->delay_swapin = NAN;
  ps->delay_freepages = NAN;

  ps->cpu_user_percent_all = -1;
  ps->cpu_system_percent_all = -1;
  ps->cpu_total_percent_all = -1;

  ps->cpu_user_percent_now = -1;
  ps->cpu_system_percent_now = -1;
  ps->cpu_total_percent_now = -1;

  procstat_entry_t *pse_prev = NULL;
  procstat_entry_t *pse = ps->instances;
  while (pse != NULL) {
    if (pse->age > 0) {
      DEBUG(LOG_KEY "Removing this procstat entry cause it's too old: "
                    "id = %lu; name = %s;",
            pse->id, ps->name);

      if (pse_prev == NULL) {
        ps->instances = pse->next;
        free(pse);
        pse = ps->instances;
      } else {
        pse_prev->next = pse->next;
        free(pse);
        pse = pse_prev->next;
      }
    } else {
      pse->age = 1;
      if (pse->start_time < ps->start_time)
        ps->start_time = pse->start_time;
      pse_prev = pse;
      pse = pse->next;
    }
  }
}

//------------------------------------------------------------------------------
// remove old entries from instances of processes in list_head_g
static void p2_statlist_reset(procstat_t *head) {
  for (procstat_t *ps = head; ps != NULL; ps = ps->next) {
    p2_statlist_entry_reset(ps);
  }
}

//------------------------------------------------------------------------------
static procstat_t *p2_statlist_new_elem(proc_cluster_t *cluster,
                                        const char *name,
                                        const char *cmd_pattern, int cmd_cmp,
                                        const char *user_pattern, int user_cmp,
                                        const char *cgroup_pattern,
                                        int cgroup_cmp) {
  procstat_t *new_ps = calloc(1, sizeof(*new_ps));
  if (new_ps == NULL) {
    ERROR(LOG_KEY "ps_list_register: calloc failed.");
    return NULL;
  }
  sstrncpy(new_ps->name, name, sizeof(new_ps->name));
  new_ps->hidden = strchr(name, '%') != NULL;

  new_ps->io_rchar = -1;
  new_ps->io_wchar = -1;
  new_ps->io_syscr = -1;
  new_ps->io_syscw = -1;
  new_ps->io_diskr = -1;
  new_ps->io_diskw = -1;
  new_ps->cswitch_vol = -1;
  new_ps->cswitch_invol = -1;
  new_ps->pss = -1;
  new_ps->pss_anon = -1;
  new_ps->pss_file = -1;
  new_ps->pss_shmem = -1;

  new_ps->report_maps_num = cluster->report_maps_num;
  new_ps->report_ctx_switch = cluster->report_ctx_switch;
  new_ps->report_delay = cluster->report_delay;
  new_ps->report_pss_info = cluster->report_pss_info;

  if (p2_pattern_create(&new_ps->cmd_pattern, cmd_pattern, cmd_cmp) != 0) {
    sfree(new_ps);
    return NULL;
  }

  if (p2_pattern_create(&new_ps->user_pattern, user_pattern, user_cmp) != 0) {
    sfree(new_ps);
    return NULL;
  }

  if (p2_pattern_create(&new_ps->cgroup_pattern, cgroup_pattern, cgroup_cmp) !=
      0) {
    sfree(new_ps);
    return NULL;
  }

  // update need of special process information
  if ((new_ps->cmd_pattern.cmp > P2_CMP_TRUE) || (strstr(name, "%N") != NULL))
    cluster->needs_cmd = true;
  if ((new_ps->user_pattern.cmp > P2_CMP_TRUE) || (strstr(name, "%U") != NULL))
    cluster->needs_user = true;
  if ((new_ps->cgroup_pattern.cmp > P2_CMP_TRUE) ||
      (strstr(name, "%G") != NULL))
    cluster->needs_cgroup = true;

  p2_statlist_reset(new_ps);

  return new_ps;
}

//------------------------------------------------------------------------------
static procstat_t *
p2_statlist_register(proc_cluster_t *cluster, const char *name,
                     const char *cmd_pattern, const char *user_pattern,
                     const char *cgroup_pattern, bool only_new_entry) {
  procstat_t *ptr;

  // check for duplicates
  for (ptr = cluster->procs; ptr != NULL; ptr = ptr->next) {
    if (strcmp(ptr->name, name) == 0) {
      if (!only_new_entry) {
        WARNING(LOG_KEY "You have configured more "
                        "than one 'Process' or "
                        "'ProcessMatch' with the same name '%s'. "
                        "All but the first setting will be "
                        "ignored.",
                name);
      }
      return NULL;
    }
  }

  procstat_t *new_ps = p2_statlist_new_elem(
      cluster, name, cmd_pattern, P2_CMP_REGEX, user_pattern, P2_CMP_REGEX,
      cgroup_pattern, P2_CMP_REGEX);

  for (ptr = cluster->procs; ptr != NULL; ptr = ptr->next) {
    if (ptr->next == NULL)
      break;
  }

  // append new element
  if (ptr == NULL)
    cluster->procs = new_ps;
  else
    ptr->next = new_ps;

  return new_ps;
}

//------------------------------------------------------------------------------
static size_t p2_strncpy(char *buffer, size_t bufsize, size_t bufpos,
                         const char *input) {
  if (input == NULL)
    return bufpos;
  while (bufpos + 1 < bufsize && *input) {
    buffer[bufpos++] = *input++;
  }
  return bufpos;
}

//------------------------------------------------------------------------------
static bool p2_statlist_match(proc_cluster_t *cluster, const char *name,
                              const char *cmdline, const char *username,
                              const char *cgroup, procstat_t **ps_ptr) {
#define LOG_KEY_FUNC LOG_KEY "p2_statlist_match()"
  // DEBUG(LOG_KEY_FUNC "name=%s, cmd=%s, user=%s, cgroup=%s against %s", name,
  //       cmdline, username, cgroup, (*ps_ptr)->name);
  procstat_t *ps = *ps_ptr;

  const char *check_name = cmdline == NULL ? name : cmdline;
  // remove relative path
  if (strncmp(check_name, "./", 2) == 0)
    check_name += 2;

  if (!p2_pattern_match(&ps->cmd_pattern, check_name))
    return false;
  if (!p2_pattern_match(&ps->user_pattern, username))
    return false;
  if (!p2_pattern_match(&ps->cgroup_pattern, cgroup))
    return false;
  if (strchr(ps->name, '%') == NULL)
    return true;

  // DEBUG(LOG_KEY_FUNC "name=%s, cmd=%s, user=%s, cgroup=%s against %s", name,
  //       cmdline, username, cgroup, (*ps_ptr)->name);

  // name composition
  char name_buffer[DATA_MAX_NAME_LEN];
  size_t name_bufpos = 0;
  // command line matching
  const char *cmd_pattern = ps->cmd_pattern.str;
  size_t cmd_bufpos = 0;
  int cmd_cmp = P2_CMP_REGEX;
  char cmd_buffer[DATA_MAX_NAME_LEN];
  // user name matching
  const char *user_pattern = ps->user_pattern.str;
  int user_cmp = P2_CMP_REGEX;
  // cgroup matching
  const char *cgroup_pattern = ps->cgroup_pattern.str;
  int cgroup_cmp = P2_CMP_REGEX;

  const char *format = ps->name;
  for (; *format != 0 && name_bufpos < sizeof(name_buffer); ++format) {
    if (*format != '%') {
      name_buffer[name_bufpos++] = *format;
      continue;
    }
    ++format;
    switch (*format) {
    case ('N'):
      // create process name and corresponding command line pattern
      for (const char *cptr = check_name;
           name_bufpos + 1 < sizeof(name_buffer) && *cptr != '\0'; ++cptr) {
        char ch = *cptr;

        // combine system processes like kworker, cpuhd, idle_inject
        if ((cmdline == NULL) && (ch == '/'))
          break;
        if ((*check_name == '[') && (ch == '/'))
          break;

        cmd_buffer[cmd_bufpos++] = *cptr;

        if (!isalnum(ch)) {
          if (strchr("[]", ch) != NULL) {
            continue;
          }
          ch = '_';
        }
        name_buffer[name_bufpos++] = ch;
      }

      cmd_buffer[cmd_bufpos] = '\0';
      cmd_pattern = cmd_buffer;
      cmd_cmp =
          strcmp(cmd_buffer, check_name) == 0 ? P2_CMP_EXACT : P2_CMP_START;
      break;
    case ('U'):
      name_bufpos =
          p2_strncpy(name_buffer, sizeof(name_buffer), name_bufpos, username);
      user_pattern = username;
      user_cmp = P2_CMP_EXACT;
      break;
    case ('G'):
      name_bufpos =
          p2_strncpy(name_buffer, sizeof(name_buffer), name_bufpos, cgroup);
      cgroup_pattern = cgroup;
      cgroup_cmp = P2_CMP_EXACT;
      break;
    default:
      ERROR(LOG_KEY_FUNC "unknown format specifier %%%c", *format);
      return false;
    }
  }
  assert(name_bufpos < sizeof(name_buffer));
  name_buffer[name_bufpos] = '\0';

  DEBUG(LOG_KEY_FUNC "  add new process pattern: '%s' "
                     "(check=%s, pattern=%s, cmdline=%s, cmdcmp=%d)",
        name_buffer, check_name, cmd_pattern, cmdline, cmd_cmp);
  procstat_t *new_ps =
      p2_statlist_new_elem(cluster, name_buffer, cmd_pattern, cmd_cmp,
                           user_pattern, user_cmp, cgroup_pattern, cgroup_cmp);
  if (new_ps == NULL) {
    ERROR(LOG_KEY_FUNC "could not create process pattern for '%s'",
          name_buffer);
    return false;
  }

  new_ps->next = ps;
  if (cluster->procs == ps) {
    cluster->procs = new_ps;
  } else {
    procstat_t *prev_ps = cluster->procs;
    for (; prev_ps->next != ps; prev_ps = prev_ps->next) {
    }
    assert(prev_ps->next == ps);
    prev_ps->next = new_ps;
  }
  *ps_ptr = new_ps;

  return true;
#undef LOG_KEY_FUNC
}

//------------------------------------------------------------------------------
static void p2_stat_update_counter(derive_t *group_counter,
                                   derive_t *group_diff, derive_t *curr_counter,
                                   derive_t new_counter) {
  // first call, initialization needed
  if (last_read_time <= 0) {
    *curr_counter = new_counter;
    if (group_diff != NULL)
      *group_diff = new_counter;
    return;
  }

  unsigned long curr_value;

  if (new_counter < *curr_counter)
    curr_value = new_counter + (ULONG_MAX - *curr_counter);
  else
    curr_value = new_counter - *curr_counter;

  if (*group_counter == -1)
    *group_counter = 0;

  *curr_counter = new_counter;
  *group_counter += curr_value;

  if (group_diff != NULL) {
    *group_diff += curr_value;
  }
}

//------------------------------------------------------------------------------
#if HAVE_LIBTASKSTATS
static void ps_update_delay_one(gauge_t *out_rate_sum,
                                value_to_rate_state_t *state, uint64_t cnt,
                                cdtime_t t) {
  gauge_t rate = NAN;
  int status = value_to_rate(&rate, (value_t){.counter = (counter_t)cnt},
                             DS_TYPE_COUNTER, t, state);
  if ((status != 0) || isnan(rate)) {
    return;
  }

  if (isnan(*out_rate_sum)) {
    *out_rate_sum = rate;
  } else {
    *out_rate_sum += rate;
  }
}

//------------------------------------------------------------------------------
static void ps_update_delay(cdtime_t now, procstat_t *out,
                            procstat_entry_t *prev, process_entry_t *curr) {
  ps_update_delay_one(&out->delay_cpu, &prev->delay_cpu, curr->delay.cpu_ns,
                      now);
  ps_update_delay_one(&out->delay_blkio, &prev->delay_blkio,
                      curr->delay.blkio_ns, now);
  ps_update_delay_one(&out->delay_swapin, &prev->delay_swapin,
                      curr->delay.swapin_ns, now);
  ps_update_delay_one(&out->delay_freepages, &prev->delay_freepages,
                      curr->delay.freepages_ns, now);
}
#endif

//------------------------------------------------------------------------------
// add process entry to 'instances' of process 'name' (or refresh it)
static void p2_statlist_add(cdtime_t now, proc_cluster_t *cluster,
                            const char *name, const char *cmdline,
                            const char *username, const char *cgroup,
                            process_entry_t *entry) {
  procstat_entry_t *pse;

  if (entry->id == 0)
    return;

  for (procstat_t *ps = cluster->procs; ps != NULL; ps = ps->next) {
    if (!p2_statlist_match(cluster, name, cmdline, username, cgroup, &ps))
      continue;

#if KERNEL_LINUX
    p2_fill_details(ps, entry);
#endif

    for (pse = ps->instances; pse != NULL; pse = pse->next)
      if ((pse->id == entry->id) || (pse->next == NULL))
        break;

    if ((pse == NULL) || (pse->id != entry->id)) {
      procstat_entry_t *new;

      new = calloc(1, sizeof(*new));
      if (new == NULL)
        return;
      new->id = entry->id;

      if (pse == NULL)
        ps->instances = new;
      else
        pse->next = new;

      pse = new;
    }

    pse->age = 0;
    pse->start_time = entry->start_time;
    if (pse->start_time < ps->start_time)
      ps->start_time = ps->start_time;
    ps->num_proc += entry->num_proc;
    ps->num_lwp += entry->num_lwp;
    ps->num_fd += entry->num_fd;
    ps->num_maps += entry->num_maps;
    ps->vmem_size += entry->vmem_size;
    ps->vmem_rss += entry->vmem_rss;
    ps->vmem_data += entry->vmem_data;
    ps->vmem_code += entry->vmem_code;
    ps->vmem_swap += entry->vmem_swap;
    ps->stack_size += entry->stack_size;

    if ((entry->io_rchar != -1) && (entry->io_wchar != -1)) {
      p2_stat_update_counter(&ps->io_rchar, &ps->io_rchar_last, &pse->io_rchar,
                             entry->io_rchar);
      p2_stat_update_counter(&ps->io_wchar, &ps->io_wchar_last, &pse->io_wchar,
                             entry->io_wchar);
    }

    if ((entry->io_syscr != -1) && (entry->io_syscw != -1)) {
      p2_stat_update_counter(&ps->io_syscr, NULL, &pse->io_syscr,
                             entry->io_syscr);
      p2_stat_update_counter(&ps->io_syscw, NULL, &pse->io_syscw,
                             entry->io_syscw);
    }

    if ((entry->io_diskr != -1) && (entry->io_diskw != -1)) {
      p2_stat_update_counter(&ps->io_diskr, NULL, &pse->io_diskr,
                             entry->io_diskr);
      p2_stat_update_counter(&ps->io_diskw, NULL, &pse->io_diskw,
                             entry->io_diskw);
    }

    if ((entry->cswitch_vol != -1) && (entry->cswitch_invol != -1)) {
      p2_stat_update_counter(&ps->cswitch_vol, NULL, &pse->cswitch_vol,
                             entry->cswitch_vol);
      p2_stat_update_counter(&ps->cswitch_invol, NULL, &pse->cswitch_invol,
                             entry->cswitch_invol);
    }

    if (entry->pss != -1) {
      p2_stat_update_counter(&ps->pss, NULL, &pse->pss, entry->pss);
    }
    if (entry->pss_anon != -1) {
      p2_stat_update_counter(&ps->pss_anon, NULL, &pse->pss_anon,
                             entry->pss_anon);
    }
    if (entry->pss_file != -1) {
      p2_stat_update_counter(&ps->pss_file, NULL, &pse->pss_file,
                             entry->pss_file);
    }
    if (entry->pss_shmem != -1) {
      p2_stat_update_counter(&ps->pss_shmem, NULL, &pse->pss_shmem,
                             entry->pss_shmem);
    }

    p2_stat_update_counter(&ps->vmem_minflt_counter, NULL,
                           &pse->vmem_minflt_counter,
                           entry->vmem_minflt_counter);
    p2_stat_update_counter(&ps->vmem_majflt_counter, NULL,
                           &pse->vmem_majflt_counter,
                           entry->vmem_majflt_counter);

    p2_stat_update_counter(&ps->cpu_user_counter, &ps->cpu_user_last,
                           &pse->cpu_user_counter, entry->cpu_user_counter);
    p2_stat_update_counter(&ps->cpu_system_counter, &ps->cpu_system_last,
                           &pse->cpu_system_counter, entry->cpu_system_counter);

#if HAVE_LIBTASKSTATS
    if (entry->has_delay)
      ps_update_delay(now, ps, pse, entry);
#endif

    break;
  }
}

//------------------------------------------------------------------------------
static void p2_statlist_destroy(procstat_t *head) {
  while (head != NULL) {
    procstat_entry_t *pse = head->instances;
    while (pse != NULL) {
      procstat_entry_t *pse_cur = pse;
      pse = pse->next;
      free(pse_cur);
    }
    procstat_t *ps_prev = head;
    head = head->next;

    p2_pattern_destroy(&ps_prev->cmd_pattern);
    p2_pattern_destroy(&ps_prev->user_pattern);
    p2_pattern_destroy(&ps_prev->cgroup_pattern);
    free(ps_prev);
  }
}

//==============================================================================
// Cluster List
//==============================================================================

//------------------------------------------------------------------------------
static proc_cluster_t *p2_cluster_register(const char *name) {
  proc_cluster_t *new;
  proc_cluster_t *ptr;

  new = calloc(1, sizeof(*new));
  if (new == NULL) {
    ERROR(LOG_KEY "p2_cluster_register: calloc failed.");
    return NULL;
  }
  INFO(LOG_KEY "add cluster %s", name);
  sstrncpy(new->name, name, sizeof(new->name));
  new->procs = NULL;

  new->report_fd_num = false;
  new->report_maps_num = false;
  new->report_ctx_switch = false;
  new->report_delay = false;
  new->report_pss_info = false;
  new->report_cpu_rank = false;
  new->report_cpu_percent = false;

  new->notify_cpu_top = 0;
  new->notify_cpu_top_single_line = 0;
  new->notify_mem_top = 0;
  new->notify_memdiff_top = 0;
  new->notify_mem_top_single_line = 0;
  new->notify_io_top_read_single_line = 0;
  new->notify_io_top_write_single_line = 0;

  new->needs_cmd = false;
  new->needs_user = false;
  new->needs_cgroup = false;

  for (ptr = cluster_head_g; ptr != NULL; ptr = ptr->next) {
    if (strcmp(ptr->name, name) == 0) {
      WARNING(LOG_KEY "You have configured more "
                      "than one 'ProcessGroup' with same name. "
                      "All but the first setting will be "
                      "ignored.");
      sfree(new);
      return NULL;
    }

    if (ptr->next == NULL)
      break;
  }

  if (ptr == NULL)
    cluster_head_g = new;
  else
    ptr->next = new;

  return new;
}

//------------------------------------------------------------------------------
static char *p2_cluster_get_cmdline(long pid, char *name, char *buf,
                                    size_t buf_len) {
  bool read_needed = false;
  for (proc_cluster_t *pc = cluster_head_g; pc != NULL; pc = pc->next) {
    if (pc->needs_cmd) {
      read_needed = true;
      break;
    }
  }
  return read_needed ? p2_get_cmdline(pid, name, buf, buf_len, false) : NULL;
}

//------------------------------------------------------------------------------
static char *p2_cluster_get_username(long pid, char *buf, size_t buf_len) {
  bool read_needed = false;
  for (proc_cluster_t *pc = cluster_head_g; pc != NULL; pc = pc->next) {
    if (pc->needs_user) {
      read_needed = true;
      break;
    }
  }
  return read_needed ? p2_get_username(pid, buf, buf_len) : NULL;
}

//------------------------------------------------------------------------------
static char *p2_cluster_get_cgroup(long pid, char *buf, size_t buf_len) {
  bool read_needed = false;
  for (proc_cluster_t *pc = cluster_head_g; pc != NULL; pc = pc->next) {
    if (pc->needs_cgroup) {
      read_needed = true;
      break;
    }
  }
  return read_needed ? p2_get_cgroup(pid, buf, buf_len) : NULL;
}

//------------------------------------------------------------------------------
// add process entry to 'instances' of process 'name' (or refresh it)
static void p2_cluster_add(cdtime_t now, const char *name, const char *cmdline,
                           const char *username, const char *cgroup,
                           process_entry_t *entry) {
  for (proc_cluster_t *pc = cluster_head_g; pc != NULL; pc = pc->next) {
    p2_statlist_add(now, pc, name, cmdline, username, cgroup, entry);
  }
}

//------------------------------------------------------------------------------
static void p2_cluster_reset(void) {
  for (proc_cluster_t *pc = cluster_head_g; pc != NULL; pc = pc->next) {
    p2_statlist_reset(pc->procs);
  }
}

//------------------------------------------------------------------------------
static void p2_cluster_destroy(void) {
  while (cluster_head_g != NULL) {
    proc_cluster_t *pc_prev = cluster_head_g;
    cluster_head_g = cluster_head_g->next;
    p2_statlist_destroy(pc_prev->procs);
    free(pc_prev);
  }
}

//------------------------------------------------------------------------------
int sort_cpu_user_now(const void *a, const void *b) {
  procstat_t *proc1 = *(procstat_t **)b;
  procstat_t *proc2 = *(procstat_t **)a;
  return NUMERIC_CMP(proc1->cpu_user_last, proc2->cpu_user_last);
}

int sort_cpu_system_now(const void *a, const void *b) {
  procstat_t *proc1 = *(procstat_t **)b;
  procstat_t *proc2 = *(procstat_t **)a;
  return NUMERIC_CMP(proc1->cpu_system_last, proc2->cpu_system_last);
}

int sort_cpu_total_now(const void *a, const void *b) {
  procstat_t *proc1 = *(procstat_t **)b;
  procstat_t *proc2 = *(procstat_t **)a;
  return NUMERIC_CMP((proc1->cpu_system_last + proc1->cpu_user_last),
                     (proc2->cpu_system_last + proc2->cpu_user_last));
}

int sort_cpu_user_all(const void *a, const void *b) {
  procstat_t *proc1 = *(procstat_t **)b;
  procstat_t *proc2 = *(procstat_t **)a;
  return NUMERIC_CMP(proc1->cpu_user_counter, proc2->cpu_user_counter);
}

int sort_cpu_system_all(const void *a, const void *b) {
  procstat_t *proc1 = *(procstat_t **)b;
  procstat_t *proc2 = *(procstat_t **)a;
  return NUMERIC_CMP(proc1->cpu_system_counter, proc2->cpu_system_counter);
}

int sort_cpu_total_all(const void *a, const void *b) {
  procstat_t *proc1 = *(procstat_t **)b;
  procstat_t *proc2 = *(procstat_t **)a;
  return NUMERIC_CMP((proc1->cpu_system_counter + proc1->cpu_user_counter),
                     (proc2->cpu_system_counter + proc2->cpu_user_counter));
}

int sort_vmem_rss(const void *a, const void *b) {
  procstat_t *proc1 = *(procstat_t **)b;
  procstat_t *proc2 = *(procstat_t **)a;
  return NUMERIC_CMP(proc1->vmem_rss, proc2->vmem_rss);
}

int sort_io_rd(const void *a, const void *b) {
  procstat_t *proc1 = *(procstat_t **)b;
  procstat_t *proc2 = *(procstat_t **)a;
  return NUMERIC_CMP(proc1->io_rchar_last, proc2->io_rchar_last);
}

int sort_io_wr(const void *a, const void *b) {
  procstat_t *proc1 = *(procstat_t **)b;
  procstat_t *proc2 = *(procstat_t **)a;
  return NUMERIC_CMP(proc1->io_wchar_last, proc2->io_wchar_last);
}

int sort_vmem_rssdiff(const void *a, const void *b) {
  procstat_t *proc1 = *(procstat_t **)a;
  procstat_t *proc2 = *(procstat_t **)b;

  // Calculate signed differences to determine direction
  long asigned_diff = (long)proc1->vmem_rss - (long)proc1->vmem_rss_last;
  long bsigned_diff = (long)proc2->vmem_rss - (long)proc2->vmem_rss_last;

  // Calculate absolute differences
  unsigned long adiff = (asigned_diff >= 0) ? asigned_diff : -asigned_diff;
  unsigned long bdiff = (bsigned_diff >= 0) ? bsigned_diff : -bsigned_diff;

  // Primary sort: by absolute difference (descending)
  if (adiff > bdiff)
    return -1;
  if (adiff < bdiff)
    return 1;

  // Secondary sort: when absolute values are equal, positive differences first
  if (asigned_diff > 0 && bsigned_diff <= 0)
    return -1;
  if (asigned_diff <= 0 && bsigned_diff > 0)
    return 1;

  return 0;
}

//------------------------------------------------------------------------------
static void p2_cluster_update_ranking(void) {
#define P2_UPDATE_RANKING(sort_func, sort_attr)                                \
  {                                                                            \
    qsort(sort_arr, num, sizeof(procstat_t *), sort_func);                     \
    for (size_t co = 0; co < num; ++co) {                                      \
      sort_arr[co]->sort_attr = co + 1;                                        \
    }                                                                          \
  }
  for (proc_cluster_t *pc = cluster_head_g; pc != NULL; pc = pc->next) {
    if (!pc->report_cpu_rank) {
      continue;
    }

    size_t num = 0;
    for (procstat_t *ps_ptr = pc->procs; ps_ptr != NULL;
         ps_ptr = ps_ptr->next) {
      ++num;
    }
    if (num <= 0) {
      continue;
    }

    procstat_t **sort_arr = malloc(num * sizeof(procstat_t *));
    procstat_t **sort_ptr = sort_arr;
    for (procstat_t *ps_ptr = pc->procs; ps_ptr != NULL;
         ps_ptr = ps_ptr->next) {
      *sort_ptr++ = ps_ptr;
    }
    P2_UPDATE_RANKING(sort_cpu_user_all, cpu_user_rank_all);
    P2_UPDATE_RANKING(sort_cpu_system_all, cpu_system_rank_all);
    P2_UPDATE_RANKING(sort_cpu_total_all, cpu_total_rank_all);
    P2_UPDATE_RANKING(sort_cpu_user_now, cpu_user_rank_now);
    P2_UPDATE_RANKING(sort_cpu_system_now, cpu_system_rank_now);
    P2_UPDATE_RANKING(sort_cpu_total_now, cpu_total_rank_now);

    free(sort_arr);
  }

#undef P2_UPDATE_RANKING
}

//------------------------------------------------------------------------------
static void p2_cluster_update_cpu_percentage(cdtime_t now) {

  uint64_t interval_all = CDTIME_T_TO_US(p2_get_uptime());
  uint64_t interval_now = CDTIME_T_TO_US(plugin_get_interval());

  DEBUG(LOG_KEY "TIME: interval now=%lu, all=%lu", interval_now, interval_all);
  for (proc_cluster_t *pc = cluster_head_g; pc != NULL; pc = pc->next) {
    if (!pc->report_cpu_percent) {
      continue;
    }

    gauge_t total_all_sum = interval_all;
    gauge_t user_all_sum = total_all_sum;
    gauge_t system_all_sum = total_all_sum;
    gauge_t total_now_sum = interval_now;
    gauge_t user_now_sum = total_now_sum;
    gauge_t system_now_sum = total_now_sum;

    if (pc->report_cpu_relative) {
      user_all_sum = 0;
      user_now_sum = 0;
      system_all_sum = 0;
      system_now_sum = 0;

      for (procstat_t *ps_ptr = pc->procs; ps_ptr != NULL;
           ps_ptr = ps_ptr->next) {

        // if previous data is available
        if (last_read_time > 0) {
          user_now_sum += ps_ptr->cpu_user_last;
          system_now_sum += ps_ptr->cpu_system_last;
        }
        user_all_sum += ps_ptr->cpu_user_counter;
        system_all_sum += ps_ptr->cpu_system_counter;
      }
      total_all_sum = user_all_sum + system_all_sum;
      total_now_sum = user_now_sum + system_now_sum;
    }

    for (procstat_t *ps_ptr = pc->procs; ps_ptr != NULL;
         ps_ptr = ps_ptr->next) {

      DEBUG(LOG_KEY "start time of %s: %.3fs", ps_ptr->name,
            CDTIME_T_TO_DOUBLE(ps_ptr->start_time));

      if (total_all_sum > 0) {
        ps_ptr->cpu_user_percent_all =
            100.0 * ps_ptr->cpu_user_counter / user_all_sum;
        ps_ptr->cpu_system_percent_all =
            100.0 * ps_ptr->cpu_system_counter / system_all_sum;
        ps_ptr->cpu_total_percent_all =
            ps_ptr->cpu_user_percent_all + ps_ptr->cpu_system_percent_all;
      }
      if (total_now_sum > 0) {
        ps_ptr->cpu_user_percent_now =
            100.0 * ps_ptr->cpu_user_last / user_now_sum;
        ps_ptr->cpu_system_percent_now =
            100.0 * ps_ptr->cpu_system_last / system_now_sum;
        ps_ptr->cpu_total_percent_now =
            ps_ptr->cpu_user_percent_now + ps_ptr->cpu_system_percent_now;
      }
    }
  }
}

//------------------------------------------------------------------------------
static void p2_cluster_notify(cdtime_t now) {
  char rss_buf[32];
  char size_buf[32];
  char swap_buf[32];
  char io_rbuf[32];
  char io_wbuf[32];

  // write notification only after second read call
  if (last_read_time == 0)
    return;

  double interval_all = CDTIME_T_TO_US(p2_get_uptime());
  double interval_now = CDTIME_T_TO_US(plugin_get_interval());

  pid_t pid = getpid();

  DEBUG(LOG_KEY "INTERVAL TIME: configured=%.0fms, measured=%lums",
        interval_now / 1000.0, CDTIME_T_TO_MS(now - last_read_time));

  for (proc_cluster_t *pc = cluster_head_g; pc != NULL; pc = pc->next) {
    if (((pc->notify_cpu_top <= 0) || pc->notify_cpu_top_single_line <= 0) &&
        (pc->notify_mem_top <= 0) && (pc->notify_memdiff_top <= 0))
      continue;

    // count number of processes
    // determine statistic about collectd itself
    size_t num = 0;
    procstat_t *selfstat = NULL;
    derive_t total_cpu_user_counter = 0;
    derive_t total_cpu_user_last = 0;
    derive_t total_cpu_system_counter = 0;
    derive_t total_cpu_system_last = 0;
    size_t total_vmem_rss = 0;
    size_t total_vmem_rss_diff_abs = 0;
    size_t total_vmem_rss_diff_pos = 0;
    size_t total_vmem_rss_diff_neg = 0;
    size_t total_vmem_swap = 0;
    size_t total_vmem_swap_diff_abs = 0;
    size_t total_vmem_swap_diff_pos = 0;
    size_t total_vmem_swap_diff_neg = 0;
    size_t total_vmem_size = 0;
    size_t total_vmem_size_diff_abs = 0;
    size_t total_vmem_size_diff_pos = 0;
    size_t total_vmem_size_diff_neg = 0;
    size_t total_num_proc = 0;
    size_t total_num_lwp = 0;

    // find process id of collectd itself
    // sum up all the cpu and memory usages
    for (procstat_t *ps_ptr = pc->procs; ps_ptr != NULL;
         ps_ptr = ps_ptr->next) {
      ++num;
      total_cpu_user_counter += ps_ptr->cpu_user_counter;
      total_cpu_user_last += ps_ptr->cpu_user_last;
      total_cpu_system_counter += ps_ptr->cpu_system_counter;
      total_cpu_system_last += ps_ptr->cpu_system_last;
      total_vmem_rss += ps_ptr->vmem_rss;
      total_vmem_swap += ps_ptr->vmem_swap;
      total_vmem_size += ps_ptr->vmem_size;
      total_num_proc += ps_ptr->num_proc;
      total_num_lwp += ps_ptr->num_lwp;
      if (ps_ptr->vmem_rss > ps_ptr->vmem_rss_last) {
        size_t diff = ps_ptr->vmem_rss - ps_ptr->vmem_rss_last;
        total_vmem_rss_diff_pos += diff;
        total_vmem_rss_diff_abs += diff;
      } else {
        size_t diff = ps_ptr->vmem_rss_last - ps_ptr->vmem_rss;
        total_vmem_rss_diff_neg += diff;
        total_vmem_rss_diff_abs += diff;
      }
      if (ps_ptr->vmem_size > ps_ptr->vmem_size_last) {
        size_t diff = ps_ptr->vmem_size - ps_ptr->vmem_size_last;
        total_vmem_size_diff_pos += diff;
        total_vmem_size_diff_abs += diff;
      } else {
        size_t diff = ps_ptr->vmem_size_last - ps_ptr->vmem_size;
        total_vmem_size_diff_neg += diff;
        total_vmem_size_diff_abs += diff;
      }

      if (ps_ptr->vmem_swap > ps_ptr->vmem_swap_last) {
        size_t diff = ps_ptr->vmem_swap - ps_ptr->vmem_swap_last;
        total_vmem_swap_diff_pos += diff;
        total_vmem_swap_diff_abs += diff;
      } else {
        size_t diff = ps_ptr->vmem_swap_last - ps_ptr->vmem_swap;
        total_vmem_swap_diff_neg += diff;
        total_vmem_swap_diff_abs += diff;
      }

      if (selfstat == NULL) {
        for (procstat_entry_t *inst_ptr = ps_ptr->instances; inst_ptr != NULL;
             inst_ptr = inst_ptr->next) {
          if (inst_ptr->id == pid) {
            selfstat = ps_ptr;
            break;
          }
        }
      }
    }

    double total_user_all = 100.0 * total_cpu_user_counter / interval_all;
    double total_system_all = 100.0 * total_cpu_system_counter / interval_all;
    double total_user_now = 100.0 * total_cpu_user_last / interval_now;
    double total_system_now = 100.0 * total_cpu_system_last / interval_now;

    if (!pc->notify_cpu_top_single_line) {
      notification_t n = {NOTIF_OKAY, now,       "", "",  PLUGIN_NAME,
                          "",         "top-sum", "", NULL};
      sstrncpy(n.plugin_instance, pc->name, sizeof(n.plugin_instance));

      double user_all = 100.0 * total_cpu_user_counter / interval_all;
      double system_all = 100.0 * total_cpu_system_counter / interval_all;
      double user_now = 100.0 * total_cpu_user_last / interval_now;
      double system_now = 100.0 * total_cpu_system_last / interval_now;
      p2_pretty_size(rss_buf, sizeof(rss_buf), total_vmem_rss);
      p2_pretty_size(size_buf, sizeof(size_buf), total_vmem_size);

      ssnprintf(
          n.message, sizeof(n.message),
          "top-sum-%s: cpu=%.1f%%(%.1f%%), name=%.*s, usr=%.1f%%(%.1f%%), "
          "sys=%.1f%%(%.1f%%), prc=%lu, thr=%lu, mem=%s(%s)",
          pc->name, user_now + system_now, user_all + system_all, 32,
          "top-list", user_now, user_all, system_now, system_all,
          total_num_proc, total_num_lwp, rss_buf, size_buf);
      plugin_dispatch_notification(&n);
      if (n.meta != NULL)
        plugin_notification_meta_free(n.meta);
    }

    // no processes found, no action needed
    if (num <= 0) {
      continue;
    }

    // fill array with process information for sorting
    procstat_t **sort_arr = malloc(num * sizeof(procstat_t *));
    procstat_t **sort_ptr = sort_arr;
    for (procstat_t *ps_ptr = pc->procs; ps_ptr != NULL;
         ps_ptr = ps_ptr->next) {
      *sort_ptr++ = ps_ptr;
    }

    //..........................................................................
    // sort by total cpu usage
    if (pc->notify_cpu_top > 0 && !pc->notify_cpu_top_single_line) {
      qsort(sort_arr, num, sizeof(procstat_t *), sort_cpu_total_now);

      // send notifications
      notification_t n = {NOTIF_OKAY, now,       "", "",  PLUGIN_NAME,
                          "",         "cpu_top", "", NULL};
      sstrncpy(n.plugin_instance, pc->name, sizeof(n.plugin_instance));

      bool log_self = true;
      derive_t top_cpu_user_counter = 0;
      derive_t top_cpu_user_last = 0;
      derive_t top_cpu_system_counter = 0;
      derive_t top_cpu_system_last = 0;
      size_t top_vmem_rss = 0;
      size_t top_vmem_size = 0;
      size_t top_num_proc = 0;
      size_t top_num_lwp = 0;

      for (int co = 0; (co < num) && ((co < pc->notify_cpu_top) || log_self);
           ++co) {
        procstat_t *cur = sort_arr[co];

        if ((co < pc->notify_cpu_top) || (cur == selfstat)) {
          if (cur == selfstat)
            log_self = false;

          top_cpu_user_counter += cur->cpu_user_counter;
          top_cpu_user_last += cur->cpu_user_last;
          top_cpu_system_counter += cur->cpu_system_counter;
          top_cpu_system_last += cur->cpu_system_last;
          top_vmem_rss += cur->vmem_rss;
          top_vmem_size += cur->vmem_size;
          top_num_proc += cur->num_proc;
          top_num_lwp += cur->num_lwp;

          double user_all = 100.0 * cur->cpu_user_counter / interval_all;
          double system_all = 100.0 * cur->cpu_system_counter / interval_all;
          double user_now = 100.0 * cur->cpu_user_last / interval_now;
          double system_now = 100.0 * cur->cpu_system_last / interval_now;
          p2_pretty_size(rss_buf, sizeof(rss_buf), cur->vmem_rss);
          p2_pretty_size(size_buf, sizeof(size_buf), cur->vmem_size);

          ssnprintf(n.message, sizeof(n.message),
                    "top-cpu-%s-%d: cpu=%.1f%%(%.1f%%), name=%.*s, "
                    "usr=%.1f%%(%.1f%%), "
                    "sys=%.1f%%(%.1f%%), prc=%lu, thr=%lu, mem=%s(%s)",
                    pc->name, co, user_now + system_now, user_all + system_all,
                    32, cur->name, user_now, user_all, system_now, system_all,
                    cur->num_proc, cur->num_lwp, rss_buf, size_buf);
          plugin_dispatch_notification(&n);
          if (n.meta != NULL)
            plugin_notification_meta_free(n.meta);
        }
      }

      double user_all = 100.0 * top_cpu_user_counter / interval_all;
      double system_all = 100.0 * top_cpu_system_counter / interval_all;
      double user_now = 100.0 * top_cpu_user_last / interval_now;
      double system_now = 100.0 * top_cpu_system_last / interval_now;
      p2_pretty_size(rss_buf, sizeof(rss_buf), top_vmem_rss);
      p2_pretty_size(size_buf, sizeof(size_buf), top_vmem_size);

      ssnprintf(n.message, sizeof(n.message),
                "top-cpu-%s-sum: cpu=%.1f%%(%.1f%%), usr=%.1f%%(%.1f%%), "
                "sys=%.1f%%(%.1f%%), prc=%lu, thr=%lu, mem=%s(%s)",
                pc->name, user_now + system_now, user_all + system_all,
                user_now, user_all, system_now, system_all, top_num_proc,
                top_num_lwp, rss_buf, size_buf);
      plugin_dispatch_notification(&n);
      if (n.meta != NULL)
        plugin_notification_meta_free(n.meta);

      ssnprintf(
          n.message, sizeof(n.message),
          "top-cpu-%s-cov: cpu=%.1f%%(%.1f%%), usr=%.1f%%(%.1f%%), "
          "sys=%.1f%%(%.1f%%), prc=%.1f%%, thr=%.1f%%, mem=%.1f%%(%.1f%%)",
          pc->name,
          100.0 * (user_now + system_now) / (total_user_now + total_system_now),
          100.0 * (user_all + system_all) / (total_user_all + total_system_all),
          100.0 * user_now / total_user_now, 100.0 * user_all / total_user_all,
          100.0 * system_now / total_system_now,
          100.0 * system_all / total_system_all,
          100.0 * top_num_proc / total_num_proc,
          100.0 * top_num_lwp / total_num_lwp,
          100.0 * top_vmem_rss / total_vmem_rss,
          100.0 * top_vmem_size / total_vmem_size);
      plugin_dispatch_notification(&n);
      if (n.meta != NULL)
        plugin_notification_meta_free(n.meta);

    } else if (pc->notify_cpu_top_single_line > 0) {
      qsort(sort_arr, num, sizeof(procstat_t *), sort_cpu_total_now);

      notification_t n = {NOTIF_OKAY, now,       "", "",  PLUGIN_NAME,
                          "",         "cpu_top", "", NULL};
      sstrncpy(n.plugin_instance, pc->name, sizeof(n.plugin_instance));

      bool log_self = true;
      char *msgp = n.message;
      size_t msgn = sizeof(n.message);

      snprintf(msgp, msgn, "cpu top processes - process/cpu usage(%%)/pid");

      for (int co = 0;
           (co < num) && ((co < pc->notify_cpu_top_single_line) || log_self);
           ++co) {
        procstat_t *cur = sort_arr[co];

        if ((co < pc->notify_cpu_top_single_line) || (cur == selfstat)) {
          if (cur == selfstat)
            log_self = false;

          double user_now = 100.0 * cur->cpu_user_last / interval_now;
          double system_now = 100.0 * cur->cpu_system_last / interval_now;
          p2_pretty_size(rss_buf, sizeof(rss_buf), cur->vmem_rss);
          p2_pretty_size(size_buf, sizeof(size_buf), cur->vmem_size);

          char buf[256] = {0};
          char *bufp = buf;
          size_t bufn = sizeof(buf);
          int r = snprintf(bufp, bufn, "%s/%.1f%%/", cur->name,
                           user_now + system_now);
          if (r < 0 || r > bufn) {
            break;
          }
          bufp += r;
          bufn -= r;

          for (procstat_entry_t *inst = cur->instances; inst != NULL;
               inst = inst->next) {
            r = snprintf(bufp, bufn, "%s%lu",
                         (inst == cur->instances) ? "" : ",", inst->id);
            if (r < 0 || r > bufn) {
              break;
            }
            bufp += r;
            bufn -= r;
          }

          if (plugin_notification_meta_add_string(&n, "", buf) != 0) {
            ERROR(LOG_KEY "Error adding meta information to notification.");
            break;
          }
        }
      }

      plugin_dispatch_notification(&n);
      if (n.meta != NULL)
        plugin_notification_meta_free(n.meta);
    }

    //..........................................................................
    // sort by total memory usage
    if (pc->notify_mem_top > 0) {
      qsort(sort_arr, num, sizeof(procstat_t *), sort_vmem_rss);

      // send notifications
      notification_t n = {NOTIF_OKAY, now,       "", "",  PLUGIN_NAME,
                          "",         "mem_top", "", NULL};
      sstrncpy(n.plugin_instance, pc->name, sizeof(n.plugin_instance));

      bool log_self = true;
      derive_t top_cpu_user_counter = 0;
      derive_t top_cpu_user_last = 0;
      derive_t top_cpu_system_counter = 0;
      derive_t top_cpu_system_last = 0;
      size_t top_vmem_rss = 0;
      size_t top_vmem_size = 0;
      size_t top_num_proc = 0;
      size_t top_num_lwp = 0;

      for (int co = 0; (co < num) && ((co < pc->notify_mem_top) || log_self);
           ++co) {
        procstat_t *cur = sort_arr[co];

        if ((co < pc->notify_mem_top) || (cur == selfstat)) {
          if (cur == selfstat)
            log_self = false;

          top_cpu_user_counter += cur->cpu_user_counter;
          top_cpu_user_last += cur->cpu_user_last;
          top_cpu_system_counter += cur->cpu_system_counter;
          top_cpu_system_last += cur->cpu_system_last;
          top_vmem_rss += cur->vmem_rss;
          top_vmem_size += cur->vmem_size;
          top_num_proc += cur->num_proc;
          top_num_lwp += cur->num_lwp;

          double user_all = 100.0 * cur->cpu_user_counter / interval_all;
          double system_all = 100.0 * cur->cpu_system_counter / interval_all;
          double user_now = 100.0 * cur->cpu_user_last / interval_now;
          double system_now = 100.0 * cur->cpu_system_last / interval_now;

          derive_t vmem_rss_diff = cur->vmem_rss - cur->vmem_rss_last;
          derive_t vmem_size_diff = cur->vmem_size - cur->vmem_size_last;

          char rss_diff_buf[32];
          char size_diff_buf[32];

          p2_pretty_size(rss_buf, sizeof(rss_buf), cur->vmem_rss);
          p2_pretty_size(size_buf, sizeof(size_buf), cur->vmem_size);
          p2_pretty_ssize(rss_diff_buf, sizeof(rss_diff_buf), vmem_rss_diff);
          p2_pretty_ssize(size_diff_buf, sizeof(size_diff_buf), vmem_size_diff);

          ssnprintf(n.message, sizeof(n.message),
                    "top-mem-%s-%d: mem=%s(%s), name=%.*s, memdiff=%s(%s), "
                    "cpu=%.1f%%(%.1f%%), "
                    "usr=%.1f%%(%.1f%%), "
                    "sys=%.1f%%(%.1f%%), prc=%lu, thr=%lu",
                    pc->name, co, rss_buf, size_buf, 32, cur->name,
                    rss_diff_buf, size_diff_buf, user_now + system_now,
                    user_all + system_all, user_now, user_all, system_now,
                    system_all, cur->num_proc, cur->num_lwp);
          plugin_dispatch_notification(&n);
          if (n.meta != NULL)
            plugin_notification_meta_free(n.meta);
        }
      }

      double user_all = 100.0 * top_cpu_user_counter / interval_all;
      double system_all = 100.0 * top_cpu_system_counter / interval_all;
      double user_now = 100.0 * top_cpu_user_last / interval_now;
      double system_now = 100.0 * top_cpu_system_last / interval_now;
      p2_pretty_size(rss_buf, sizeof(rss_buf), top_vmem_rss);
      p2_pretty_size(size_buf, sizeof(size_buf), top_vmem_size);

      ssnprintf(
          n.message, sizeof(n.message),
          "top-mem-%s-sum: mem=%s(%s), cpu=%.1f%%(%.1f%%), usr=%.1f%%(%.1f%%), "
          "sys=%.1f%%(%.1f%%), prc=%lu, thr=%lu",
          pc->name, rss_buf, size_buf, user_now + system_now,
          user_all + system_all, user_now, user_all, system_now, system_all,
          top_num_proc, top_num_lwp);
      plugin_dispatch_notification(&n);
      if (n.meta != NULL)
        plugin_notification_meta_free(n.meta);

      ssnprintf(
          n.message, sizeof(n.message),
          "top-mem-%s-cov: mem=%.1f%%(%.1f%%), cpu=%.1f%%(%.1f%%), "
          "usr=%.1f%%(%.1f%%), sys=%.1f%%(%.1f%%), prc=%.1f%%, thr=%.1f%%",
          pc->name, 100.0 * top_vmem_rss / total_vmem_rss,
          100.0 * top_vmem_size / total_vmem_size,
          100.0 * (user_now + system_now) / (total_user_now + total_system_now),
          100.0 * (user_all + system_all) / (total_user_all + total_system_all),
          100.0 * user_now / total_user_now, 100.0 * user_all / total_user_all,
          100.0 * system_now / total_system_now,
          100.0 * system_all / total_system_all,
          100.0 * top_num_proc / total_num_proc,
          100.0 * top_num_lwp / total_num_lwp);
      plugin_dispatch_notification(&n);
      if (n.meta != NULL)
        plugin_notification_meta_free(n.meta);

    } else if (pc->notify_mem_top_single_line > 0) {
      qsort(sort_arr, num, sizeof(procstat_t *), sort_vmem_rss);

      // send notifications
      notification_t n = {NOTIF_OKAY, now,       "", "",  PLUGIN_NAME,
                          "",         "mem_top", "", NULL};
      sstrncpy(n.plugin_instance, pc->name, sizeof(n.plugin_instance));

      bool log_self = true;
      char *msgp = n.message;
      size_t msgn = sizeof(n.message);

      snprintf(
          msgp, msgn,
          "mem top processes - process/memory usage(MB)/swap usage(MB)/pid");

      for (int co = 0;
           (co < num) && ((co < pc->notify_mem_top_single_line) || log_self);
           ++co) {
        procstat_t *cur = sort_arr[co];

        if ((co < pc->notify_mem_top_single_line) || (cur == selfstat)) {
          if (cur == selfstat)
            log_self = false;

          ssnprintf(rss_buf, sizeof(rss_buf), "%.1f",
                    (float)cur->vmem_rss / 1024 / 1024);
          ssnprintf(swap_buf, sizeof(swap_buf), "%.1f",
                    (float)cur->vmem_swap / 1024 / 1024);

          char buf[256] = {0};
          char *bufp = buf;
          size_t bufn = sizeof(buf);
          int r = 0;
          r = snprintf(bufp, bufn, "%s/%s/%s/", cur->name, rss_buf, swap_buf);

          if (r < 0 || r > bufn) {
            break;
          }
          bufp += r;
          bufn -= r;

          for (procstat_entry_t *inst = cur->instances; inst != NULL;
               inst = inst->next) {
            r = snprintf(bufp, bufn, "%s%lu",
                         (inst == cur->instances) ? "" : ",", inst->id);

            if (r < 0 || r > bufn) {
              break;
            }
            bufp += r;
            bufn -= r;
          }

          if (plugin_notification_meta_add_string(&n, "", buf) != 0) {
            ERROR(LOG_KEY "Error adding meta information to notification.");
            break;
          }
        }
      }

      plugin_dispatch_notification(&n);
      if (n.meta != NULL)
        plugin_notification_meta_free(n.meta);
    }

    //..........................................................................
    // sort by total io read usage
    if (pc->notify_io_top_read_single_line > 0) {
      qsort(sort_arr, num, sizeof(procstat_t *), sort_io_rd);

      // send notifications
      notification_t n = {NOTIF_OKAY, now,      "", "",  PLUGIN_NAME,
                          "",         "io_top", "", NULL};
      sstrncpy(n.plugin_instance, pc->name, sizeof(n.plugin_instance));

      bool log_self = true;
      char *msgp = n.message;
      size_t msgn = sizeof(n.message);

      snprintf(msgp, msgn, "io top processes rd - process/rd(MB/s)/pid");

      for (int co = 0; (co < num) &&
                       ((co < pc->notify_io_top_read_single_line) || log_self);
           ++co) {
        procstat_t *cur = sort_arr[co];

        if ((co < pc->notify_io_top_read_single_line) || (cur == selfstat)) {
          if (cur == selfstat)
            log_self = false;

          unsigned int interval = interval_now / 1000000;
          ssnprintf(io_rbuf, sizeof(io_rbuf), "%.2f",
                    ((double)cur->io_rchar_last) / 1024 / 1024 / interval);

          char buf[256] = {0};
          char *bufp = buf;
          size_t bufn = sizeof(buf);
          int r = 0;
          r = snprintf(bufp, bufn, "%s/%s/", cur->name, io_rbuf);

          if (r < 0 || r > bufn) {
            break;
          }
          bufp += r;
          bufn -= r;

          for (procstat_entry_t *inst = cur->instances; inst != NULL;
               inst = inst->next) {
            r = snprintf(bufp, bufn, "%s%lu",
                         (inst == cur->instances) ? "" : ",", inst->id);

            if (r < 0 || r > bufn) {
              break;
            }
            bufp += r;
            bufn -= r;
          }

          if (plugin_notification_meta_add_string(&n, "", buf) != 0) {
            ERROR(LOG_KEY "Error adding meta information to notification.");
            break;
          }
        }
      }

      plugin_dispatch_notification(&n);
      if (n.meta != NULL)
        plugin_notification_meta_free(n.meta);
    }

    //..........................................................................
    // sort by total io write usage
    if (pc->notify_io_top_write_single_line > 0) {
      qsort(sort_arr, num, sizeof(procstat_t *), sort_io_wr);

      // send notifications
      notification_t n = {NOTIF_OKAY, now,      "", "",  PLUGIN_NAME,
                          "",         "io_top", "", NULL};
      sstrncpy(n.plugin_instance, pc->name, sizeof(n.plugin_instance));

      bool log_self = true;
      char *msgp = n.message;
      size_t msgn = sizeof(n.message);

      snprintf(msgp, msgn, "io top processes wr - process/wr(MB/s)/pid");

      for (int co = 0; (co < num) &&
                       ((co < pc->notify_io_top_write_single_line) || log_self);
           ++co) {
        procstat_t *cur = sort_arr[co];

        if ((co < pc->notify_io_top_write_single_line) || (cur == selfstat)) {
          if (cur == selfstat)
            log_self = false;

          unsigned int interval = interval_now / 1000000;
          ssnprintf(io_wbuf, sizeof(io_wbuf), "%.2f",
                    ((double)cur->io_wchar_last) / 1024 / 1024 / interval);

          char buf[256] = {0};
          char *bufp = buf;
          size_t bufn = sizeof(buf);
          int r = 0;
          r = snprintf(bufp, bufn, "%s/%s/", cur->name, io_wbuf);

          if (r < 0 || r > bufn) {
            break;
          }
          bufp += r;
          bufn -= r;

          for (procstat_entry_t *inst = cur->instances; inst != NULL;
               inst = inst->next) {
            r = snprintf(bufp, bufn, "%s%lu",
                         (inst == cur->instances) ? "" : ",", inst->id);

            if (r < 0 || r > bufn) {
              break;
            }
            bufp += r;
            bufn -= r;
          }

          if (plugin_notification_meta_add_string(&n, "", buf) != 0) {
            ERROR(LOG_KEY "Error adding meta information to notification.");
            break;
          }
        }
      }

      plugin_dispatch_notification(&n);
      if (n.meta != NULL)
        plugin_notification_meta_free(n.meta);
    }

    //..........................................................................
    // sort by total memory change
    if (pc->notify_memdiff_top > 0) {
      qsort(sort_arr, num, sizeof(procstat_t *), sort_vmem_rssdiff);

      // send notifications
      notification_t n = {NOTIF_OKAY, now,           "", "",  PLUGIN_NAME,
                          "",         "memdiff_top", "", NULL};
      sstrncpy(n.plugin_instance, pc->name, sizeof(n.plugin_instance));

      bool log_self = true;
      derive_t top_cpu_user_counter = 0;
      derive_t top_cpu_user_last = 0;
      derive_t top_cpu_system_counter = 0;
      derive_t top_cpu_system_last = 0;
      size_t top_vmem_rss = 0;
      ssize_t top_vmem_rss_diff = 0;
      size_t top_vmem_rss_diff_abs = 0;
      size_t top_vmem_rss_diff_pos = 0;
      size_t top_vmem_rss_diff_neg = 0;
      size_t top_vmem_swap = 0;
      ssize_t top_vmem_swap_diff = 0;
      size_t top_vmem_swap_diff_abs = 0;
      size_t top_vmem_swap_diff_pos = 0;
      size_t top_vmem_swap_diff_neg = 0;
      ssize_t top_vmem_size_diff = 0;
      size_t top_vmem_size = 0;
      size_t top_vmem_size_diff_abs = 0;
      size_t top_vmem_size_diff_pos = 0;
      size_t top_vmem_size_diff_neg = 0;
      size_t top_num_proc = 0;
      size_t top_num_lwp = 0;

      for (int co = 0;
           (co < num) && ((co < pc->notify_memdiff_top) || log_self); ++co) {
        procstat_t *cur = sort_arr[co];

        if ((co < pc->notify_memdiff_top) || (cur == selfstat)) {
          if (cur == selfstat)
            log_self = false;

          // as soon as first entry without changed memory usage is reached,
          // all following processes are the same because of sorting
          if (cur->vmem_rss == cur->vmem_rss_last)
            break;

          if (cur->vmem_rss > cur->vmem_rss_last) {
            size_t diff = cur->vmem_rss - cur->vmem_rss_last;
            top_vmem_rss_diff_pos += diff;
            top_vmem_rss_diff_abs += diff;
          } else {
            size_t diff = cur->vmem_rss_last - cur->vmem_rss;
            top_vmem_rss_diff_neg += diff;
            top_vmem_rss_diff_abs += diff;
          }
          if (cur->vmem_size > cur->vmem_size_last) {
            size_t diff = cur->vmem_size - cur->vmem_size_last;
            top_vmem_size_diff_pos += diff;
            top_vmem_size_diff_abs += diff;
          } else {
            size_t diff = cur->vmem_size_last - cur->vmem_size;
            top_vmem_size_diff_neg += diff;
            top_vmem_size_diff_abs += diff;
          }
          if (cur->vmem_swap > cur->vmem_swap_last) {
            size_t diff = cur->vmem_swap - cur->vmem_swap_last;
            top_vmem_swap_diff_pos += diff;
            top_vmem_swap_diff_abs += diff;
          } else {
            size_t diff = cur->vmem_swap_last - cur->vmem_swap;
            top_vmem_swap_diff_neg += diff;
            top_vmem_swap_diff_abs += diff;
          }

          ssize_t vmem_rss_diff =
              (ssize_t)cur->vmem_rss - (ssize_t)cur->vmem_rss_last;
          ssize_t vmem_size_diff =
              (ssize_t)cur->vmem_size - (ssize_t)cur->vmem_size_last;
          ssize_t vmem_swap_diff =
              (ssize_t)cur->vmem_swap - (ssize_t)cur->vmem_swap_last;
          top_vmem_rss_diff += vmem_rss_diff;
          top_vmem_size_diff += vmem_size_diff;
          top_vmem_swap_diff += vmem_swap_diff;

          top_cpu_user_counter += cur->cpu_user_counter;
          top_cpu_user_last += cur->cpu_user_last;
          top_cpu_system_counter += cur->cpu_system_counter;
          top_cpu_system_last += cur->cpu_system_last;
          top_vmem_rss += cur->vmem_rss;
          top_vmem_size += cur->vmem_size;
          top_vmem_swap += cur->vmem_swap;
          top_num_proc += cur->num_proc;
          top_num_lwp += cur->num_lwp;

          double user_all = 100.0 * cur->cpu_user_counter / interval_all;
          double system_all = 100.0 * cur->cpu_system_counter / interval_all;
          double user_now = 100.0 * cur->cpu_user_last / interval_now;
          double system_now = 100.0 * cur->cpu_system_last / interval_now;

          char rss_diff_buf[32];
          char size_diff_buf[32];
          char swap_diff_buff[32];

          p2_pretty_size(rss_buf, sizeof(rss_buf), cur->vmem_rss);
          p2_pretty_size(size_buf, sizeof(size_buf), cur->vmem_size);
          p2_pretty_size(swap_buf, sizeof(swap_buf), cur->vmem_swap);
          p2_pretty_ssize(rss_diff_buf, sizeof(rss_diff_buf), vmem_rss_diff);
          p2_pretty_ssize(size_diff_buf, sizeof(size_diff_buf), vmem_size_diff);
          p2_pretty_ssize(swap_diff_buff, sizeof(swap_diff_buff),
                          vmem_swap_diff);

          ssnprintf(n.message, sizeof(n.message),
                    "top-memdiff-%s-%d: memdiff=%s/%s(%s), name=%.*s, "
                    "mem=%s/%s(%s), "
                    "cpu=%.1f%%(%.1f%%), "
                    "usr=%.1f%%(%.1f%%), "
                    "sys=%.1f%%(%.1f%%), prc=%lu, thr=%lu",
                    pc->name, co, rss_diff_buf, swap_diff_buff, size_diff_buf,
                    32, cur->name, rss_buf, swap_buf, size_buf,
                    user_now + system_now, user_all + system_all, user_now,
                    user_all, system_now, system_all, cur->num_proc,
                    cur->num_lwp);

          plugin_dispatch_notification(&n);
          if (n.meta != NULL)
            plugin_notification_meta_free(n.meta);
        }
      }

      char rss_diff_buf[32];
      char size_diff_buf[32];
      char swap_diff_buf[32];
      char rss_diff_pos_buf[32];
      char rss_diff_neg_buf[32];
      char size_diff_pos_buf[32];
      char size_diff_neg_buf[32];
      char swap_diff_pos_buf[32];
      char swap_diff_neg_buf[32];

      double user_all = 100.0 * top_cpu_user_counter / interval_all;
      double system_all = 100.0 * top_cpu_system_counter / interval_all;
      double user_now = 100.0 * top_cpu_user_last / interval_now;
      double system_now = 100.0 * top_cpu_system_last / interval_now;
      p2_pretty_size(rss_buf, sizeof(rss_buf), top_vmem_rss);
      p2_pretty_size(size_buf, sizeof(size_buf), top_vmem_size);
      p2_pretty_size(swap_buf, sizeof(swap_buf), top_vmem_swap);
      p2_pretty_ssize(rss_diff_buf, sizeof(rss_diff_buf), top_vmem_rss_diff);
      p2_pretty_size(rss_diff_pos_buf, sizeof(rss_diff_pos_buf),
                     top_vmem_rss_diff_pos);
      p2_pretty_size(rss_diff_neg_buf, sizeof(rss_diff_neg_buf),
                     top_vmem_rss_diff_neg);
      p2_pretty_ssize(size_diff_buf, sizeof(size_diff_buf), top_vmem_size_diff);
      p2_pretty_size(size_diff_pos_buf, sizeof(size_diff_pos_buf),
                     top_vmem_size_diff_pos);
      p2_pretty_size(size_diff_neg_buf, sizeof(size_diff_neg_buf),
                     top_vmem_size_diff_neg);

      p2_pretty_ssize(swap_diff_buf, sizeof(swap_diff_buf), top_vmem_swap_diff);
      p2_pretty_size(swap_diff_pos_buf, sizeof(swap_diff_pos_buf),
                     top_vmem_swap_diff_pos);
      p2_pretty_size(swap_diff_neg_buf, sizeof(swap_diff_neg_buf),
                     top_vmem_swap_diff_neg);

      ssnprintf(n.message, sizeof(n.message),
                "top-memdiff-%s-sum: memdiff=%s/%s(%s), "
                "memdiff(+)=%s/%s(%s), memdiff(-)=%s/%s(%s), "
                "mem=%s(%s), cpu=%.1f%%(%.1f%%), "
                "usr=%.1f%%(%.1f%%), "
                "sys=%.1f%%(%.1f%%), prc=%lu, thr=%lu",
                pc->name, rss_diff_buf, swap_diff_buf, size_diff_buf,
                rss_diff_pos_buf, swap_diff_pos_buf, size_diff_pos_buf,
                rss_diff_neg_buf, swap_diff_neg_buf, size_diff_neg_buf, rss_buf,
                size_buf, user_now + system_now, user_all + system_all,
                user_now, user_all, system_now, system_all, top_num_proc,
                top_num_lwp);

      plugin_dispatch_notification(&n);
      if (n.meta != NULL)
        plugin_notification_meta_free(n.meta);

      ssnprintf(
          n.message, sizeof(n.message),
          "top-memdiff-%s-cov: memdiff=%.1f%%/%.1f%%(%.1f%%), "
          "memdiff(+)=%.1f%%/%.1f%%(%.1f%%), "
          "memdiff(-)=%.1f%%/%.1f%%(%.1f%%), "
          "mem=%.1f%%(%.1f%%), cpu=%.1f%%(%.1f%%), "
          "usr=%.1f%%(%.1f%%), sys=%.1f%%(%.1f%%), prc=%.1f%%, thr=%.1f%%",
          pc->name,
          total_vmem_rss_diff_abs
              ? 100.0 * top_vmem_rss_diff_abs / total_vmem_rss_diff_abs
              : 100.0,
          total_vmem_swap_diff_abs
              ? 100.0 * top_vmem_swap_diff_abs / total_vmem_swap_diff_abs
              : 100.0,
          total_vmem_size_diff_abs
              ? 100.0 * top_vmem_size_diff_abs / total_vmem_size_diff_abs
              : 100.0,
          total_vmem_rss_diff_pos
              ? 100.0 * top_vmem_rss_diff_pos / total_vmem_rss_diff_pos
              : 100.0,
          total_vmem_swap_diff_pos
              ? 100.0 * top_vmem_swap_diff_pos / total_vmem_swap_diff_pos
              : 100.0,
          total_vmem_size_diff_pos
              ? 100.0 * top_vmem_size_diff_pos / total_vmem_size_diff_pos
              : 100.0,
          total_vmem_rss_diff_neg
              ? 100.0 * top_vmem_rss_diff_neg / total_vmem_rss_diff_neg
              : 100.0,
          total_vmem_swap_diff_neg
              ? 100.0 * top_vmem_swap_diff_neg / total_vmem_swap_diff_neg
              : 100.0,
          total_vmem_size_diff_neg
              ? 100.0 * top_vmem_size_diff_neg / total_vmem_size_diff_neg
              : 100.0,
          100.0 * top_vmem_rss / total_vmem_rss,
          100.0 * top_vmem_size / total_vmem_size,
          100.0 * (user_now + system_now) / (total_user_now + total_system_now),
          100.0 * (user_all + system_all) / (total_user_all + total_system_all),
          100.0 * user_now / total_user_now, 100.0 * user_all / total_user_all,
          100.0 * system_now / total_system_now,
          100.0 * system_all / total_system_all,
          100.0 * top_num_proc / total_num_proc,
          100.0 * top_num_lwp / total_num_lwp);

      plugin_dispatch_notification(&n);
      if (n.meta != NULL)
        plugin_notification_meta_free(n.meta);
    }

    free(sort_arr);
  }
}

//------------------------------------------------------------------------------
static void p2_cluster_submit(cdtime_t now) {
  p2_cluster_update_ranking();
  p2_cluster_update_cpu_percentage(now);
  for (proc_cluster_t *pc = cluster_head_g; pc != NULL; pc = pc->next) {
    for (procstat_t *ps_ptr = pc->procs; ps_ptr != NULL;
         ps_ptr = ps_ptr->next) {
      p2_submit_proc_list(now, pc, ps_ptr);
    }
  }
  p2_cluster_notify(now);
}

#endif

//==============================================================================
// Configuration
//==============================================================================

//------------------------------------------------------------------------------
#if !defined(__QNX__)
static void p2_config_process(oconfig_item_t *ci, procstat_t *ps) {
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *c = ci->children + i;

    if (strcasecmp(c->key, "CollectContextSwitch") == 0)
      cf_util_get_boolean(c, &ps->report_ctx_switch);
    else if (strcasecmp(c->key, "CollectFileDescriptor") == 0)
      cf_util_get_boolean(c, &ps->report_fd_num);
    else if (strcasecmp(c->key, "CollectMemoryMaps") == 0)
      cf_util_get_boolean(c, &ps->report_maps_num);
    else if (strcasecmp(c->key, "CollectProportionalSetSize") == 0)
      cf_util_get_boolean(c, &ps->report_pss_info);
    else if (strcasecmp(c->key, "CollectDelayAccounting") == 0) {
#if HAVE_LIBTASKSTATS
      cf_util_get_boolean(c, &ps->report_delay);
#else
      WARNING(LOG_KEY "The plugin has been compiled without support "
                      "for the \"CollectDelayAccounting\" option.");
#endif
    } else {
      ERROR(LOG_KEY "Option \"%s\" not allowed here.", c->key);
    }
  }
}

//------------------------------------------------------------------------------
static int p2_config_cluster(proc_cluster_t *cluster, oconfig_item_t *ci) {
#if KERNEL_LINUX
  const size_t max_procname_len = 15;
#endif

  procstat_t *ps;

  for (int i = 0; i < ci->children_num; ++i) {
    oconfig_item_t *c = ci->children + i;

    if (strcasecmp(c->key, "ProcessGroup") == 0) {
      char *name = NULL;
      int status = cf_util_get_string(c, &name);
      if (name == NULL) {
        ERROR(LOG_KEY "'ProcessGroup' expects exactly "
                      "one string argument.");
        continue;
      }
      proc_cluster_t *cluster = p2_cluster_register(name);
      sfree(name);
      if (status != 0) {
        return status;
      }
      p2_config_cluster(cluster, c);
    } else if (strcasecmp(c->key, "Process") == 0) {
      if ((c->values_num != 1) || (OCONFIG_TYPE_STRING != c->values[0].type)) {
        ERROR(LOG_KEY "'Process' expects exactly "
                      "one string argument (got %i).",
              c->values_num);
        continue;
      }

#if KERNEL_LINUX
      if (strlen(c->values[0].value.string) > max_procname_len) {
        WARNING(LOG_KEY "this platform has a %" PRIsz " character limit "
                        "to process names. The 'Process \"%s\"' option will "
                        "not work as expected.",
                max_procname_len, c->values[0].value.string);
      }
#endif

      ps = p2_statlist_register(cluster, c->values[0].value.string, NULL, NULL,
                                NULL, false);

      if (c->children_num != 0 && ps != NULL)
        p2_config_process(c, ps);
    } else if (strcasecmp(c->key, "ProcessMatch") == 0) {
      if ((c->values_num < 2) || (c->values_num > 4) ||
          (OCONFIG_TYPE_STRING != c->values[0].type) ||
          (OCONFIG_TYPE_STRING != c->values[1].type)) {
        ERROR(LOG_KEY "'ProcessMatch' needs "
                      "two to four string arguments (got %i).",
              c->values_num);
        continue;
      }

      const char *cmd_pattern = c->values[1].value.string;
      const char *user_pattern = NULL;
      if (c->values_num >= 3)
        user_pattern = c->values[2].value.string;

      const char *cgroup_pattern = NULL;
      if (c->values_num >= 4)
        cgroup_pattern = c->values[3].value.string;

      ps = p2_statlist_register(cluster, c->values[0].value.string, cmd_pattern,
                                user_pattern, cgroup_pattern, false);

      if (c->children_num != 0 && ps != NULL)
        p2_config_process(c, ps);
    } else if (strcasecmp(c->key, "CollectContextSwitch") == 0) {
      cf_util_get_boolean(c, &cluster->report_ctx_switch);
    } else if (strcasecmp(c->key, "CollectFileDescriptor") == 0) {
      cf_util_get_boolean(c, &cluster->report_fd_num);
    } else if (strcasecmp(c->key, "CollectMemoryMaps") == 0) {
      cf_util_get_boolean(c, &cluster->report_maps_num);
    } else if (strcasecmp(c->key, "CollectProportionalSetSize") == 0) {
      cf_util_get_boolean(c, &cluster->report_pss_info);
    } else if (strcasecmp(c->key, "CollectDelayAccounting") == 0) {
#if HAVE_LIBTASKSTATS
      cf_util_get_boolean(c, &cluster->report_delay);
#else
      WARNING(LOG_KEY "The plugin has been compiled without support "
                      "for the \"CollectDelayAccounting\" option.");
#endif
    } else if (strcasecmp(c->key, "CollectCpuRank") == 0) {
      cf_util_get_boolean(c, &cluster->report_cpu_rank);
    } else if (strcasecmp(c->key, "CollectCpuPercent") == 0) {
      if ((c->values_num != 1) || (OCONFIG_TYPE_STRING != c->values[0].type)) {
        ERROR(LOG_KEY "'CollectCpuPercent' needs one string argument"
                      ", (got %i).",
              c->values_num);
        continue;
      }
      if (strcasecmp(c->values[0].value.string, "absolute") == 0) {
        cluster->report_cpu_percent = true;
        cluster->report_cpu_relative = false;
      } else if (strcasecmp(c->values[0].value.string, "relative") == 0) {
        cluster->report_cpu_percent = true;
        cluster->report_cpu_relative = true;
      } else {
        cluster->report_cpu_percent = false;
        ERROR(LOG_KEY "'CollectCpuPercent' only accepts: absolute, relative");
        continue;
      }
    } else if (strcasecmp(c->key, "NotifyCpuTop") == 0) {
      cf_util_get_int(c, &cluster->notify_cpu_top);
    } else if (strcasecmp(c->key, "NotifyMemTop") == 0) {
      cf_util_get_int(c, &cluster->notify_mem_top);
    } else if (strcasecmp(c->key, "NotifyMemDiffTop") == 0) {
      cf_util_get_int(c, &cluster->notify_memdiff_top);
    } else if (strcasecmp(c->key, "NotifyCpuTopSingleLine") == 0) {
      cf_util_get_int(c, &cluster->notify_cpu_top_single_line);
    } else if (strcasecmp(c->key, "NotifyMemTopSingleLine") == 0) {
      cf_util_get_int(c, &cluster->notify_mem_top_single_line);
    } else if (strcasecmp(c->key, "NotifyIOTopReadSingleLine") == 0) {
      cf_util_get_int(c, &cluster->notify_io_top_read_single_line);
    } else if (strcasecmp(c->key, "NotifyIOTopWriteSingleLine") == 0) {
      cf_util_get_int(c, &cluster->notify_io_top_write_single_line);
    } else {
      ERROR(LOG_KEY "The '%s' configuration option is not "
                    "understood and will be ignored.",
            c->key);
      continue;
    }
  }
  return 0;
}
#endif
//------------------------------------------------------------------------------
// put all pre-defined 'Process' names from config to list_head_g tree
static int p2_config(oconfig_item_t *ci) {
  p2_config_called = true;
#if !defined(__QNX__)
  DEBUG(LOG_KEY "p2_config(%p)", ci);

  p2_user_info_cache_init();
  p2_process_info_cache_init();

  cluster_head_g = NULL;

  proc_cluster_t *default_cluster = p2_cluster_register("");
  if (default_cluster == NULL)
    return -1;
  int ret = p2_config_cluster(default_cluster, ci);
  if (ret) {
    return ret;
  }
#endif
  return 0;
}

//------------------------------------------------------------------------------
static int p2_init(void) {
  if (!p2_config_called) {
    oconfig_item_t empty_config = {0};
    assert(empty_config.children == 0);
    assert(empty_config.children_num == 0);
    assert(empty_config.key == 0);
    assert(empty_config.parent == 0);
    assert(empty_config.values == 0);
    assert(empty_config.values_num == 0);
    p2_config(&empty_config);
  }

  DEBUG(LOG_KEY "p2_init()");

  notification_t n = {NOTIF_OKAY, cdtime(), "", "",  PLUGIN_NAME,
                      "",         "",       "", NULL};
  ssnprintf(n.message, NOTIF_MAX_MSG_LEN, "collectd version: plugin %s %s",
            PLUGIN_NAME, PACKAGE_VERSION);
  plugin_dispatch_notification(&n);
  if (n.meta != NULL)
    plugin_notification_meta_free(n.meta);

#if !defined(__QNX__)
  HERTZ = sysconf(_SC_CLK_TCK);
  if (HERTZ <= 0)
    HERTZ = HZ;

  DEBUG(LOG_KEY "system uptime=%3fs", CDTIME_T_TO_DOUBLE(p2_get_uptime()));

#endif

#if KERNEL_LINUX
  pagesize_g = sysconf(_SC_PAGESIZE);
  DEBUG(LOG_KEY "pagesize_g = %li; CONFIG_HZ = %i;", pagesize_g, CONFIG_HZ);
#endif

#if HAVE_LIBTASKSTATS
  if (taskstats_handle == NULL) {
    taskstats_handle = ts_create();
    if (taskstats_handle == NULL) {
      WARNING(LOG_KEY "Creating taskstats handle failed.");
    }
  }
#endif

  return 0;
}

#if !defined(__QNX__)
//------------------------------------------------------------------------------
// submit global state (e.g.: qty of zombies, running, etc..)
static void p2_submit_state(cdtime_t now, const char *state, double value) {
  value_list_t vl = {.time = now,
                     .values = &(value_t){.gauge = value},
                     .values_len = 1,
                     .meta = NULL};

  sstrncpy(vl.plugin, PLUGIN_NAME, sizeof(vl.plugin));
  sstrncpy(vl.plugin_instance, "", sizeof(vl.plugin_instance));
  sstrncpy(vl.type, "ps_state", sizeof(vl.type));
  sstrncpy(vl.type_instance, state, sizeof(vl.type_instance));

  plugin_dispatch_values(&vl);
}

//------------------------------------------------------------------------------
// submit info about specific process (e.g.: memory taken, cpu usage, * etc..)
static void p2_submit_proc_list(cdtime_t now, proc_cluster_t *cluster,
                                procstat_t *ps) {
  // skip entries with placeholders in name
  if (ps->hidden)
    return;

  value_t values[3];
  value_list_t vl = {.time = now, .values = values, .meta = NULL};

  sstrncpy(vl.plugin, PLUGIN_NAME, sizeof(vl.plugin));
  if (cluster != NULL && cluster->name[0] != '\0') {
    ssnprintf(vl.plugin_instance, sizeof(vl.plugin_instance), "%s-%s",
              cluster->name, ps->name);
  } else {
    sstrncpy(vl.plugin_instance, ps->name, sizeof(vl.plugin_instance));
  }

  sstrncpy(vl.type, "ps_vm", sizeof(vl.type));
  vl.values[0].gauge = ps->vmem_size;
  vl.values_len = 1;
  plugin_dispatch_values(&vl);

  sstrncpy(vl.type, "ps_rss", sizeof(vl.type));
  vl.values[0].gauge = ps->vmem_rss;
  vl.values_len = 1;
  plugin_dispatch_values(&vl);

  sstrncpy(vl.type, "ps_data", sizeof(vl.type));
  vl.values[0].gauge = ps->vmem_data;
  vl.values_len = 1;
  plugin_dispatch_values(&vl);

  sstrncpy(vl.type, "ps_code", sizeof(vl.type));
  vl.values[0].gauge = ps->vmem_code;
  vl.values_len = 1;
  plugin_dispatch_values(&vl);

  sstrncpy(vl.type, "ps_stacksize", sizeof(vl.type));
  vl.values[0].gauge = ps->stack_size;
  vl.values_len = 1;
  plugin_dispatch_values(&vl);

  sstrncpy(vl.type, "ps_cputime", sizeof(vl.type));
  vl.values[0].derive = ps->cpu_user_counter;
  vl.values[1].derive = ps->cpu_system_counter;
  vl.values_len = 2;
  plugin_dispatch_values(&vl);

  sstrncpy(vl.type, "ps_count", sizeof(vl.type));
  vl.values[0].gauge = ps->num_proc;
  vl.values[1].gauge = ps->num_lwp;
  vl.values_len = 2;
  plugin_dispatch_values(&vl);

  sstrncpy(vl.type, "ps_pagefaults", sizeof(vl.type));
  vl.values[0].derive = ps->vmem_minflt_counter;
  vl.values[1].derive = ps->vmem_majflt_counter;
  vl.values_len = 2;
  plugin_dispatch_values(&vl);

  if ((ps->io_rchar != -1) && (ps->io_wchar != -1)) {
    sstrncpy(vl.type, "io_octets", sizeof(vl.type));
    vl.values[0].derive = ps->io_rchar;
    vl.values[1].derive = ps->io_wchar;
    vl.values_len = 2;
    plugin_dispatch_values(&vl);
  }

  if ((ps->io_syscr != -1) && (ps->io_syscw != -1)) {
    sstrncpy(vl.type, "io_ops", sizeof(vl.type));
    vl.values[0].derive = ps->io_syscr;
    vl.values[1].derive = ps->io_syscw;
    vl.values_len = 2;
    plugin_dispatch_values(&vl);
  }

  if ((ps->io_diskr != -1) && (ps->io_diskw != -1)) {
    sstrncpy(vl.type, "disk_octets", sizeof(vl.type));
    vl.values[0].derive = ps->io_diskr;
    vl.values[1].derive = ps->io_diskw;
    vl.values_len = 2;
    plugin_dispatch_values(&vl);
  }

  if (ps->num_fd > 0) {
    sstrncpy(vl.type, "file_handles", sizeof(vl.type));
    vl.values[0].gauge = ps->num_fd;
    vl.values_len = 1;
    plugin_dispatch_values(&vl);
  }

  if (ps->num_maps > 0) {
    sstrncpy(vl.type, "file_handles", sizeof(vl.type));
    sstrncpy(vl.type_instance, "mapped", sizeof(vl.type_instance));
    vl.values[0].gauge = ps->num_maps;
    vl.values_len = 1;
    plugin_dispatch_values(&vl);
  }

  if ((ps->cswitch_vol != -1) && (ps->cswitch_invol != -1)) {
    sstrncpy(vl.type, "contextswitch", sizeof(vl.type));
    sstrncpy(vl.type_instance, "voluntary", sizeof(vl.type_instance));
    vl.values[0].derive = ps->cswitch_vol;
    vl.values_len = 1;
    plugin_dispatch_values(&vl);

    sstrncpy(vl.type, "contextswitch", sizeof(vl.type));
    sstrncpy(vl.type_instance, "involuntary", sizeof(vl.type_instance));
    vl.values[0].derive = ps->cswitch_invol;
    vl.values_len = 1;
    plugin_dispatch_values(&vl);
  }

  if (ps->cpu_total_rank_now >= 0) {
    sstrncpy(vl.type, "ps_cpu_rank", sizeof(vl.type));
    sstrncpy(vl.type_instance, "now", sizeof(vl.type_instance));
    vl.values[0].gauge = ps->cpu_user_rank_now;
    vl.values[1].gauge = ps->cpu_system_rank_now;
    vl.values[2].gauge = ps->cpu_total_rank_now;
    vl.values_len = 3;
    plugin_dispatch_values(&vl);
  }

  if (ps->cpu_total_rank_all >= 0) {
    sstrncpy(vl.type, "ps_cpu_rank", sizeof(vl.type));
    sstrncpy(vl.type_instance, "avg", sizeof(vl.type_instance));
    vl.values[0].gauge = ps->cpu_user_rank_all;
    vl.values[1].gauge = ps->cpu_system_rank_all;
    vl.values[2].gauge = ps->cpu_total_rank_all;
    vl.values_len = 3;
    plugin_dispatch_values(&vl);
  }

  if (ps->cpu_total_percent_now >= 0) {
    sstrncpy(vl.type, "ps_cpu_percent", sizeof(vl.type));
    sstrncpy(vl.type_instance, "now", sizeof(vl.type_instance));
    vl.values[0].gauge = ps->cpu_user_percent_now;
    vl.values[1].gauge = ps->cpu_system_percent_now;
    vl.values[2].gauge = ps->cpu_total_percent_now;
    vl.values_len = 3;
    plugin_dispatch_values(&vl);
  }

  if (ps->cpu_total_percent_all >= 0) {
    sstrncpy(vl.type, "ps_cpu_percent", sizeof(vl.type));
    sstrncpy(vl.type_instance, "avg", sizeof(vl.type_instance));
    vl.values[0].gauge = ps->cpu_user_percent_all;
    vl.values[1].gauge = ps->cpu_system_percent_all;
    vl.values[2].gauge = ps->cpu_total_percent_all;
    vl.values_len = 3;
    plugin_dispatch_values(&vl);
  }

  // submit proportional set sizes
  struct {
    const char *type_instance;
    gauge_t value;
  } pss_metrics[] = {
      {"total", ps->pss},
      {"anon", ps->pss_anon},
      {"file", ps->pss_file},
      {"shmem", ps->pss_shmem},
  };
  sstrncpy(vl.type, "ps_pss", sizeof(vl.type));
  vl.values_len = 1;
  for (size_t i = 0; i < STATIC_ARRAY_SIZE(pss_metrics); i++) {
    if (pss_metrics[i].value >= 0) {
      sstrncpy(vl.type_instance, pss_metrics[i].type_instance,
               sizeof(vl.type_instance));
      vl.values[0].gauge = pss_metrics[i].value;
      plugin_dispatch_values(&vl);
    }
  }

  // The ps->delay_* metrics are in nanoseconds per second. Convert to seconds
  // per second.
  gauge_t const delay_factor = 1000000000.0;

  struct {
    const char *type_instance;
    gauge_t rate_ns;
  } delay_metrics[] = {
      {"delay-cpu", ps->delay_cpu},
      {"delay-blkio", ps->delay_blkio},
      {"delay-swapin", ps->delay_swapin},
      {"delay-freepages", ps->delay_freepages},
  };
  for (size_t i = 0; i < STATIC_ARRAY_SIZE(delay_metrics); i++) {
    if (isnan(delay_metrics[i].rate_ns)) {
      continue;
    }
    sstrncpy(vl.type, "delay_rate", sizeof(vl.type));
    sstrncpy(vl.type_instance, delay_metrics[i].type_instance,
             sizeof(vl.type_instance));
    vl.values[0].gauge = delay_metrics[i].rate_ns / delay_factor;
    vl.values_len = 1;
    plugin_dispatch_values(&vl);
  }

  DEBUG(LOG_KEY
        "DISPATCH() name = %s; num_proc = %lu; num_lwp = %lu; num_fd = "
        "%lu; num_maps = %lu; "
        "vmem_size = %lu; vmem_rss = %lu; vmem_data = %lu; "
        "vmem_code = %lu; "
        "vmem_minflt_counter = %" PRIi64 "; vmem_majflt_counter = %" PRIi64 "; "
        "pss_total = %ld; pss_anon = %ld; pss_file = %ld; pss_shmem = %ld; "
        "cpu_user_counter = %" PRIi64 "; cpu_system_counter = %" PRIi64 "; "
        "io_rchar = %" PRIi64 "; io_wchar = %" PRIi64 "; "
        "io_syscr = %" PRIi64 "; io_syscw = %" PRIi64 "; "
        "io_diskr = %" PRIi64 "; io_diskw = %" PRIi64 "; "
        "cswitch_vol = %" PRIi64 "; cswitch_invol = %" PRIi64 "; "
        "delay_cpu = %g; delay_blkio = %g; "
        "delay_swapin = %g; delay_freepages = %g;"
        "cpu_user_rank_now = %g (%.2f%%, %.2f%%); "
        "cpu_system_rank_now = %g (%.2f%%, %.2f%%); "
        "cpu_total_rank_now = %g (%.2f%%, %.2f%%); ",
        ps->name, ps->num_proc, ps->num_lwp, ps->num_fd, ps->num_maps,
        ps->vmem_size, ps->vmem_rss, ps->vmem_data, ps->vmem_code,
        ps->vmem_minflt_counter, ps->vmem_majflt_counter, ps->pss, ps->pss_anon,
        ps->pss_file, ps->pss_shmem, ps->cpu_user_counter,
        ps->cpu_system_counter, ps->io_rchar, ps->io_wchar, ps->io_syscr,
        ps->io_syscw, ps->io_diskr, ps->io_diskw, ps->cswitch_vol,
        ps->cswitch_invol, ps->delay_cpu, ps->delay_blkio, ps->delay_swapin,
        ps->delay_freepages, ps->cpu_user_rank_now, ps->cpu_user_percent_now,
        ps->cpu_user_percent_all, ps->cpu_system_rank_now,
        ps->cpu_system_percent_now, ps->cpu_system_percent_all,
        ps->cpu_total_rank_now, ps->cpu_total_percent_now,
        ps->cpu_total_percent_all);
}

#else
// submit info about QNX specific process (e.g.: memory taken, cpu usage, etc..)
static void p2_qnx_submit_proc_list(procstat_t *ps) {
  value_list_t vl = VALUE_LIST_INIT;
  value_t values[2];

  vl.values = values;
  sstrncpy(vl.plugin, "processes", sizeof(vl.plugin));
  sstrncpy(vl.plugin_instance, ps->name, sizeof(vl.plugin_instance));

  sstrncpy(vl.type, "ps_cputime", sizeof(vl.type));
  vl.values[0].derive = (derive_t)ps->cpu_user_time;
  vl.values[1].derive = (derive_t)ps->cpu_system_time;
  vl.values_len = 2;
  plugin_dispatch_values(&vl);

  sstrncpy(vl.type, "ps_stacksize", sizeof(vl.type));
  vl.values[0].gauge = (gauge_t)ps->process_stack;
  vl.values_len = 1;
  plugin_dispatch_values(&vl);

  if (ps->num_fd > 0) {
    sstrncpy(vl.type, "file_handles", sizeof(vl.type));
    vl.values[0].gauge = (gauge_t)ps->num_fd;
    vl.values_len = 1;
    plugin_dispatch_values(&vl);
  }

  DEBUG("name = %s;"
        "base_address = %lu; initial_stack = %lu; process_stack = %lu;"
        "cpu_user_counter = %" PRIi64 "; cpu_system_counter = %" PRIi64 "; "
        "num_fd = %lu; num_thread = %lu; ",
        ps->name, ps->base_address, ps->initial_stack, ps->process_stack,
        ps->cpu_user_time, ps->cpu_system_time, ps->num_fd, ps->num_thread);
}
#endif

//------------------------------------------------------------------------------
#if KERNEL_LINUX
static void p2_submit_fork_rate(derive_t value) {
  value_list_t vl = VALUE_LIST_INIT;

  vl.values = &(value_t){.derive = value};
  vl.values_len = 1;
  sstrncpy(vl.plugin, PLUGIN_NAME, sizeof(vl.plugin));
  sstrncpy(vl.plugin_instance, "", sizeof(vl.plugin_instance));
  sstrncpy(vl.type, "fork_rate", sizeof(vl.type));
  sstrncpy(vl.type_instance, "", sizeof(vl.type_instance));

  plugin_dispatch_values(&vl);
}
#endif

//------------------------------------------------------------------------------
#if KERNEL_LINUX
static int p2_read_tasks_status(process_entry_t *ps) {
  char dirname[64];
  DIR *dh;
  char filename[64];
  FILE *fh;
  struct dirent *ent;
  derive_t cswitch_vol = 0;
  derive_t cswitch_invol = 0;
  char buffer[1024];
  char *fields[8];
  int numfields;

  ssnprintf(dirname, sizeof(dirname), "/proc/%li/task", ps->id);

  if ((dh = opendir(dirname)) == NULL) {
    DEBUG(LOG_KEY "Failed to open directory '%s'", dirname);
    return -1;
  }

  while ((ent = readdir(dh)) != NULL) {
    char *tpid;

    if (!isdigit((int)ent->d_name[0]))
      continue;

    tpid = ent->d_name;

    int r = ssnprintf(filename, sizeof(filename), "/proc/%li/task/%s/status",
                      ps->id, tpid);
    if ((size_t)r >= sizeof(filename)) {
      DEBUG(LOG_KEY "Filename too long: '%s'", filename);
      continue;
    }

    if ((fh = fopen(filename, "r")) == NULL) {
      DEBUG(LOG_KEY "Failed to open file '%s'", filename);
      continue;
    }

    while (fgets(buffer, sizeof(buffer), fh) != NULL) {
      derive_t tmp;
      char *endptr;

      if (strncmp(buffer, "voluntary_ctxt_switches", 23) != 0 &&
          strncmp(buffer, "nonvoluntary_ctxt_switches", 26) != 0)
        continue;

      numfields = strsplit(buffer, fields, STATIC_ARRAY_SIZE(fields));

      if (numfields < 2)
        continue;

      errno = 0;
      endptr = NULL;
      tmp = (derive_t)strtoll(fields[1], &endptr, 10);
      if ((errno == 0) && (endptr != fields[1])) {
        if (strncmp(buffer, "voluntary_ctxt_switches", 23) == 0) {
          cswitch_vol += tmp;
        } else if (strncmp(buffer, "nonvoluntary_ctxt_switches", 26) == 0) {
          cswitch_invol += tmp;
        }
      }
    }

    if (fclose(fh)) {
      WARNING(LOG_KEY "fclose: %s", STRERRNO);
    }
  }
  closedir(dh);

  ps->cswitch_vol = cswitch_vol;
  ps->cswitch_invol = cswitch_invol;

  return 0;
}

//------------------------------------------------------------------------------
// Read data from /proc/pid/status
static int p2_read_status(long pid, process_entry_t *ps) {
  char buffer[2024];
  char filename[64];
  unsigned long uid = 0;
  unsigned long lib = 0;
  unsigned long exe = 0;
  unsigned long data = 0;
  unsigned long swap = 0;

  ssnprintf(filename, sizeof(filename), "/proc/%li/status", pid);

  if (p2_read_text_file_contents(filename, buffer, sizeof(buffer)) <= 0)
    return -1;

  unsigned ready = 0;
  for (const char *cptr = buffer; *cptr; cptr += strcspn(cptr, "\n") + 1) {
    if ((cptr[0] == 'V') && (cptr[1] == 'm')) {
      const char *start = cptr + 2;
      if (strncmp(start, "Data", 4) == 0) {
        data = strtoul(start + 5, NULL, 10);
        ready |= 1 << 0;
      } else if (strncmp(start, "Lib", 3) == 0) {
        lib = strtoul(start + 4, NULL, 10);
        ready |= 1 << 1;
      } else if (strncmp(start, "Exe", 3) == 0) {
        exe = strtoul(start + 4, NULL, 10);
        ready |= 1 << 2;
      } else if (strncmp(start, "Swap", 4) == 0) {
        swap = strtoul(start + 5, NULL, 10);
        ready |= 1 << 3;
      }
    } else if (strncmp(cptr, "Uid", 3) == 0) {
      uid = strtoul(cptr + 4, NULL, 10);
      ready |= 1 << 4;
    } else if (strncmp(cptr, "Threads", 3) == 0) {
      ready = 1 << 5;
    }
    if (ready >= 31)
      break;
  }

  ps->uid = uid;
  ps->vmem_data = data * 1024;
  ps->vmem_code = (exe + lib) * 1024;
  ps->vmem_swap = swap * 1024;

  return 0;
}

//------------------------------------------------------------------------------
static int p2_read_io(process_entry_t *ps) {
  char buffer[1024];
  char filename[64];

  ssnprintf(filename, sizeof(filename), "/proc/%li/io", ps->id);
  if (p2_read_text_file_contents(filename, buffer, sizeof(buffer)) <= 0)
    return -1;

  for (const char *cptr = buffer; *cptr; cptr += strcspn(cptr, "\n") + 1) {
    switch (*cptr) {
    case 'r':
      if (strncasecmp(cptr, "rchar:", 6) == 0) {
        cptr += 6;
        ps->io_rchar = p2_get_derive(&cptr);
      } else if (strncasecmp(cptr, "read_bytes:", 11) == 0) {
        cptr += 11;
        ps->io_diskr = p2_get_derive(&cptr);
      }
      break;
    case 'w':
      if (strncasecmp(cptr, "wchar:", 6) == 0) {
        cptr += 6;
        ps->io_wchar = p2_get_derive(&cptr);
      } else if (strncasecmp(cptr, "write_bytes:", 12) == 0) {
        cptr += 12;
        ps->io_diskw = p2_get_derive(&cptr);
      }
      break;
    case 's':
      if (strncasecmp(cptr, "syscr:", 6) == 0) {
        cptr += 6;
        ps->io_syscr = p2_get_derive(&cptr);
      } else if (strncasecmp(cptr, "syscw:", 6) == 0) {
        cptr += 6;
        ps->io_syscw = p2_get_derive(&cptr);
      }
      break;
    }
  }

  return 0;
}

//------------------------------------------------------------------------------
static int p2_count_maps(long pid) {
  FILE *fh;
  char buffer[1024];
  char filename[64];
  int count = 0;

  ssnprintf(filename, sizeof(filename), "/proc/%li/maps", pid);
  if ((fh = fopen(filename, "r")) == NULL) {
    DEBUG(LOG_KEY "p2_count_maps: Failed to open file '%s'", filename);
    return -1;
  }

  while (fgets(buffer, sizeof(buffer), fh) != NULL) {
    if (strchr(buffer, '\n')) {
      count++;
    }
  }

  if (fclose(fh)) {
    WARNING(LOG_KEY "fclose: %s", STRERRNO);
  }
  return count;
}

//------------------------------------------------------------------------------
static int p2_count_fd(long pid) {
  char dirname[64];
  DIR *dh;
  struct dirent *ent;
  int count = 0;

  ssnprintf(dirname, sizeof(dirname), "/proc/%li/fd", pid);

  if ((dh = opendir(dirname)) == NULL) {
    DEBUG(LOG_KEY "Failed to open directory '%s'", dirname);
    return -1;
  }
  while ((ent = readdir(dh)) != NULL) {
    if (!isdigit((int)ent->d_name[0]))
      continue;
    else
      count++;
  }
  closedir(dh);

  return (count >= 1) ? count : 1;
}

//------------------------------------------------------------------------------
static int p2_read_pss_info(process_entry_t *pse) {
  char buffer[1024];
  char filename[64];

  ssnprintf(filename, sizeof(filename), "/proc/%li/smaps_rollup", pse->id);
  if (p2_read_text_file_contents(filename, buffer, sizeof(buffer)) <= 0)
    return -1;

  size_t found = 0;
  for (const char *cptr = buffer; *cptr; cptr += strcspn(cptr, "\n") + 1) {
    if (*cptr == 'P') {
      if (strncasecmp(cptr, "Pss:", 4) == 0) {
        cptr += 4;
        ++found;
        pse->pss = p2_get_derive_bytes(&cptr);
      } else if (strncasecmp(cptr, "Pss_Anon:", 9) == 0) {
        cptr += 9;
        ++found;
        pse->pss_anon = p2_get_derive_bytes(&cptr);
      } else if (strncasecmp(cptr, "Pss_File:", 9) == 0) {
        cptr += 9;
        ++found;
        pse->pss_file = p2_get_derive_bytes(&cptr);
      } else if (strncasecmp(cptr, "Pss_Shmem:", 10) == 0) {
        cptr += 10;
        ++found;
        pse->pss_shmem = p2_get_derive_bytes(&cptr);
      }
      if (found >= 4)
        break;
    }
  }
  if (found < 4)
    return -1;

  return 0;
}

//------------------------------------------------------------------------------
#if HAVE_LIBTASKSTATS
static int p2_delay(process_entry_t *ps) {
  if (taskstats_handle == NULL) {
    return ENOTCONN;
  }

  int status = ts_delay_by_tgid(taskstats_handle, (uint32_t)ps->id, &ps->delay);
  if (status == EPERM) {
    static c_complain_t c;
#if defined(HAVE_SYS_CAPABILITY_H) && defined(CAP_NET_ADMIN)
    if (check_capability(CAP_NET_ADMIN) != 0) {
      if (getuid() == 0) {
        c_complain(LOG_ERR, &c,
                   LOG_KEY "Reading Delay Accounting metric failed: %s. "
                           "collectd is running as root, but missing the "
                           "CAP_NET_ADMIN "
                           "capability. The most common cause for this is "
                           "that the init "
                           "system is dropping capabilities.",
                   STRERROR(status));
      } else {
        c_complain(LOG_ERR, &c,
                   LOG_KEY
                   "Reading Delay Accounting metric failed: %s. "
                   "collectd is not running as root and missing the "
                   "CAP_NET_ADMIN "
                   "capability. Either run collectd as root or grant it the "
                   "CAP_NET_ADMIN capability using \"setcap "
                   "cap_net_admin=ep " PREFIX "/sbin/collectd\".",
                   STRERROR(status));
      }
    } else {
      ERROR(LOG_KEY "ts_delay_by_tgid failed: %s. The CAP_NET_ADMIN "
                    "capability is available (I checked), so this "
                    "error is utterly "
                    "unexpected.",
            STRERROR(status));
    }
#else
    c_complain(LOG_ERR, &c,
               LOG_KEY
               "Reading Delay Accounting metric failed: %s. "
               "Reading Delay Accounting metrics requires root privileges.",
               STRERROR(status));
#endif
    return status;
  } else if (status != 0) {
    ERROR(LOG_KEY "ts_delay_by_tgid failed: %s", STRERROR(status));
    return status;
  }

  return 0;
}
#endif

//------------------------------------------------------------------------------
static void p2_fill_details(const procstat_t *ps, process_entry_t *entry) {
  if (entry->has_io == false) {
    p2_read_io(entry);
    entry->has_io = true;
  }

  if (ps->report_ctx_switch) {
    if (entry->has_cswitch == false) {
      p2_read_tasks_status(entry);
      entry->has_cswitch = true;
    }
  }

  if (ps->report_maps_num) {
    int num_maps;
    if (entry->has_maps == false && (num_maps = p2_count_maps(entry->id)) > 0) {
      entry->num_maps = num_maps;
    }
    entry->has_maps = true;
  }

  if (ps->report_fd_num) {
    int num_fd;
    if (entry->has_fd == false && (num_fd = p2_count_fd(entry->id)) > 0) {
      entry->num_fd = num_fd;
    }
    entry->has_fd = true;
  }

  if (ps->report_pss_info) {
    if (entry->has_pss == false) {
      p2_read_pss_info(entry);
      entry->has_pss = true;
    }
  }

#if HAVE_LIBTASKSTATS
  if (ps->report_delay && !entry->has_delay) {
    if (p2_delay(entry) == 0) {
      entry->has_delay = true;
    }
  }
#endif
}

//------------------------------------------------------------------------------
// p2_read_process reads process counters on Linux.
static int p2_read_process(long pid, process_entry_t *ps, char *state) {
  char filename[64];
  char buffer[1024];
  ssize_t status;

  ssnprintf(filename, sizeof(filename), "/proc/%li/stat", pid);

  status = p2_read_text_file_contents(filename, buffer, sizeof(buffer));
  if (status <= 0)
    return -1;

  // parse name
  const char *name_begin = buffer + strcspn(buffer, "(");
  if (*name_begin == '\0') {
    WARNING(LOG_KEY "no name found in '%s'", buffer);
    return -1;
  }
  ++name_begin;
  const char *name_end = name_begin;
  size_t name_len = 0;
  int bracket_co = 1;
  while (*name_end) {
    size_t forward = strcspn(name_end, "()");
    name_len += forward;
    name_end += forward;
    if (*name_end) {
      bracket_co += *name_end == '(' ? 1 : -1;
      ++name_end;
    }
    if (bracket_co == 0)
      break;
    ++name_len;
  }
  if ((bracket_co != 0) || (*name_end != ' ')) {
    WARNING(LOG_KEY "no name found, wrong brackets in '%s'", buffer);
    return -1;
  }
  sstrncpy(ps->name, name_begin, name_len + 1);

  const char *cptr = name_end;
  const size_t max_field = 29;
  unsigned long long stack_start = 0;
  unsigned long long stack_end = 0;
  for (size_t field = 3; *cptr && (field <= max_field); ++field) {
    cptr += 1; // strspn(cptr, " ");
    switch (field) {
      // 01 - pid - process id
      // 02 - tcomm - filename of the executable
    case 3: // state - state (R is running, S is sleeping, D is sleeping in an
      //       uninterruptible wait, Z is zombie, T is traced or stopped)
      *state = *cptr;
      if (*cptr == 'Z') {
        ps->num_lwp = 0;
        ps->num_proc = 0;
        DEBUG(LOG_KEY "This is only a zombie: pid = %li; "
                      "name = %s;",
              pid, ps->name);
        return 0;
      } else {
        if ((p2_read_status(pid, ps)) != 0) {
          // No VMem data
          ps->vmem_data = -1;
          ps->vmem_code = -1;
          DEBUG(LOG_KEY "p2_read_process: did not get vmem data for pid %li",
                pid);
        }
        ps->num_proc = 1;
      }
      ++cptr;
      break;
    // 04 - ppid - process id of the parent process
    // 05 - pgrp - pgrp of the process
    // 06 - sid - session id
    // 07 - tty_nr - tty the process uses
    // 08 - tty_pgrp - pgrp of the tty
    // 09 - flags - task flags
    case 10: // min_flt - number of minor faults
      ps->vmem_minflt_counter = p2_get_ul(&cptr);
      break;
    // 11 - cmin_flt - number of minor faults with child’s
    case 12: // maj_flt - number of major faults
      ps->vmem_majflt_counter = p2_get_ul(&cptr);
      break;
    // 13 - cmaj_flt - number of major faults with child’s
    case 14: // utime - user mode jiffies
      ps->cpu_user_counter = p2_get_ul(&cptr);
      ps->cpu_user_counter *= 1000000;
      ps->cpu_user_counter /= CONFIG_HZ;
      break;
    case 15: // stime - kernel mode jiffies
      ps->cpu_system_counter = p2_get_ul(&cptr);
      ps->cpu_system_counter *= 1000000;
      ps->cpu_system_counter /= CONFIG_HZ;
      break;
    // 16 - cutime - user mode jiffies with child’s
    // 17 - cstime - kernel mode jiffies with child’s
    // 18 - priority - priority level
    // 19 - nice - nice level
    case 20: // num_threads - number of threads
      ps->num_lwp = p2_get_ul(&cptr);
      if (ps->num_lwp == 0)
        ps->num_lwp = 1;
      break;
      // 21 - it_real_value - (obsolete, always 0)
    case 22: // start_time - time the process started after system boot
      ps->start_time = TIME_T_TO_CDTIME_T(p2_get_ul(&cptr)) / HERTZ;
      break;
    case 23: // vsize - virtual memory size
      ps->vmem_size = p2_get_derive(&cptr);
      break;
    case 24: // rss - resident set memory size
      ps->vmem_rss = p2_get_derive(&cptr) * pagesize_g;
      break;
      // 25 - rsslim - current limit in bytes on the rss
      // 26 - start_code - address above which program text can run
      // 27 - end_code - address below which program text can run
      // 28 -
    case 28: // start_stack - address of the start of the main process stack
      stack_start = p2_get_ull(&cptr);
      break;
    case 29: // esp - current value of ESP
      stack_end = p2_get_ull(&cptr);
      break;
    // 30 - eip - current value of EIP
    // 31 - pending - bitmap of pending signals
    // 32 - blocked - bitmap of blocked signals
    // 33 - sigign - bitmap of ignored signals
    // 34 - sigcatch - bitmap of caught signals
    // 35 - 0 - (place holder, used to be the wchan address,
    //          use /proc/PID/wchan instead)
    // 36 - 0 - (place holder)
    // 37 - 0 - (place holder)
    // 38 - exit_signal - signal to send to parent thread on exit
    // 39 - task_cpu - which CPU the task is scheduled on
    // 40 - rt_priority - realtime priority
    // 41 - policy - scheduling policy (man sched_setscheduler)
    // 42 - blkio_ticks - time spent waiting for block IO
    // 43 - gtime - guest time of the task in jiffies
    // 44 - cgtime - guest time of the task children in jiffies
    // 45 - start_data - address above which program data+bss is placed
    // 46 - end_data - address below which program data+bss is placed
    // 47 - start_brk - address above which program heap can be expanded
    // with
    //                  brk()
    // 48 - arg_start - address above which program command line is placed
    // 49 - arg_end - address below which program command line is placed
    // 50 - env_start - address above which program environment is placed
    // 51 - env_end - address below which program environment is placed
    // 52 - exit_code - the thread’s exit_code in the form reported by
    //                  the waitpid system call
    default:
      cptr += strcspn(cptr, " ");
    }
  }

  if (stack_end > 0) {
    ps->stack_size = (stack_start > stack_end) ? stack_start - stack_end
                                               : stack_end - stack_start;
  } else {
    ps->stack_size = 0;
  }

  // no data by default. May be filled by p2_fill_details ()
  ps->io_rchar = -1;
  ps->io_wchar = -1;
  ps->io_syscr = -1;
  ps->io_syscw = -1;
  ps->io_diskr = -1;
  ps->io_diskw = -1;

  ps->cswitch_vol = -1;
  ps->cswitch_invol = -1;

  ps->pss = -1;
  ps->pss_anon = -1;
  ps->pss_file = -1;
  ps->pss_shmem = -1;

  return 0;
}

//------------------------------------------------------------------------------
static int p2_procs_running(void) {
  char buffer[65536] = {};
  char id[] = "procs_running ";
  char *running_start;
  char *running_end = NULL;
  long result = 0L;

  if (p2_read_text_file_contents("/proc/stat", buffer, sizeof(buffer)) <= 0)
    return -1;

  running_start = strstr(buffer, id);
  if (!running_start) {
    WARNING(LOG_KEY "procs_running not found");
    return -1;
  }
  running_start += strlen(id);

  result = strtol(running_start, &running_end, 10);
  if ((*running_start != '\0') &&
      ((*running_end == '\0') || (*running_end == '\n'))) {
    return (int)result;
  }

  return -1;
}

//------------------------------------------------------------------------------
static int p2_read_fork_rate(void) {
  char buffer[50 * 1024];
  value_t value;
  const char id[] = "processes ";

  if (p2_read_text_file_contents("/proc/stat", buffer, sizeof(buffer)) <= 0)
    return -1;

  char *processes = strstr(buffer, id);
  if (!processes) {
    WARNING(LOG_KEY "%s not found", id);
    return -1;
  }
  processes += strlen(id);

  // replace next newline by binary zero for correct value parsing
  for (char *cptr = processes + 1; *cptr; ++cptr) {
    if (*cptr == '\n') {
      *cptr = '\0';
      break;
    }
  }

  if (parse_value(processes, &value, DS_TYPE_DERIVE) != 0)
    return -1;

  p2_submit_fork_rate(value.derive);
  return 0;
}
#endif // KERNEL_LINUX

//------------------------------------------------------------------------------
#if defined(__QNX__)
int p2_qnx_dump_process_info(char *ps_name, int ProcFd) {
  int i, j, num;
  int num_threads = 0;

  debug_process_t debug_data;
  procstat_t ps;
  procfs_status thread_status;
  unsigned long long process_stack_size = 0;

  if (ProcFd == -1) {
    fprintf(stderr, "pload: Unable to access procnto: %s\n", strerror(errno));
    fflush(stderr);
    return -1;
  }

  i = fcntl(ProcFd, F_GETFD);
  if (i != -1) {
    i |= FD_CLOEXEC;

    if (fcntl(ProcFd, F_SETFD, i) != -1) {
      devctl(ProcFd, DCMD_PROC_INFO, &debug_data, sizeof(debug_data), NULL);

      sstrncpy(ps.name, ps_name, sizeof(ps.name));
      ps.cpu_system_time = debug_data.stime;
      ps.cpu_user_time = debug_data.utime;
      ps.base_address = debug_data.base_address;
      ps.initial_stack = debug_data.initial_stack;
      ps.num_fd = debug_data.num_fdcons;
      ps.num_thread = debug_data.num_threads;
      num_threads = debug_data.num_threads;
      thread_status.tid = 1;

      for (j = 0; j < num_threads; j++) {
        if (devctl(ProcFd, DCMD_PROC_TIDSTATUS, &thread_status,
                   sizeof(thread_status), &num) == -1) {
          fprintf(stderr, "Can't get thread info %s\n", strerror(errno));
          return -1;
        } else {
          process_stack_size += thread_status.stksize;
          thread_status.tid++;
        }
      }
      ps.process_stack = process_stack_size;

      p2_qnx_submit_proc_list(&ps);

      return (EOK);
    }
  }
  return (-1);
}

static int p2_qnx_iterate_per_process(int pid) {
  char as_file_path[PATH_MAX];
  int fd;
  int status;

  static struct {
    procfs_debuginfo info;
    char buff[PATH_MAX];
  } name;
  sprintf(as_file_path, "/proc/%d/as", pid);

  // Open a connection to proc to talk over.
  fd = open(as_file_path, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "Unable to get proc entry for the process: %s\n",
            strerror(errno));
    close(fd);
    return -ENOENT;
  } else {
    if (devctl(fd, DCMD_PROC_MAPDEBUG_BASE, &name, sizeof(name), 0) != EOK) {
      if (pid == 1)
        strcpy(name.info.path, "(procnto)");
      else
        strcpy(name.info.path, "(n/a)");
    }
    status = p2_qnx_dump_process_info(name.info.path, fd);
    if (status != EOK) {
      fprintf(stderr, "Unable to get process metrics: %s\n", strerror(errno));
      close(fd);
      return -ENOENT;
    }
  }
  close(fd);
  return EOK;
}

static int iterate_processes(void) {
  struct dirent *dirent;
  DIR *dir;
  int pid;
  int status;

  dir = opendir("/proc");
  if (dir != NULL) {
    while ((dirent = readdir(dir))) {
      if (strlen(dirent->d_name) > 0 && isdigit((int)dirent->d_name[0])) {
        pid = atoi(dirent->d_name);
        status = p2_qnx_iterate_per_process(pid);
        if (status != EOK) {
          fprintf(stderr, "Unable to get time metrics for the process: %d\n",
                  pid);
          closedir(dir);
          return -ENOENT;
        }
      }
    }
    closedir(dir);
  }
  return EOK;
}
#endif
//------------------------------------------------------------------------------
// do actual readings from kernel
static cdtime_t read_time_used = 0;

static int p2_read(void) {
  cdtime_t now = cdtime();

#if KERNEL_LINUX
  int running = 0;
  int sleeping = 0;
  int zombies = 0;
  int stopped = 0;
  int paging = 0;
  int blocked = 0;

  struct dirent *ent;
  DIR *proc;
  long pid = 0;

  char cmdline[CMDLINE_BUFFER_SIZE];
  char username[USER_NAME_BUFFER_SIZE];
  char cgroup[CGROUP_BUFFER_SIZE];

  int status;
  process_entry_t pse;
  char state = '\0';

  running = sleeping = zombies = stopped = paging = blocked = 0;
  p2_cluster_reset();

  if ((proc = opendir("/proc")) == NULL) {
    ERROR("Cannot open '/proc': %s", STRERRNO);
    return -1;
  }

  while ((ent = readdir(proc)) != NULL) {
    if (!isdigit(ent->d_name[0]))
      continue;

    if ((pid = atol(ent->d_name)) < 1)
      continue;

    memset(&pse, 0, sizeof(pse));
    pse.id = pid;

    status = p2_read_process(pid, &pse, &state);
    if (status != 0) {
      DEBUG(LOG_KEY "p2_read_process failed: %i", status);
      continue;
    }

    switch (state) {
    case 'R':
      running++;
      break;
    case 'S':
      sleeping++;
      break;
    case 'D':
      blocked++;
      break;
    case 'Z':
      zombies++;
      break;
    case 'T':
      stopped++;
      break;
    case 'W':
      paging++;
      break;
    }

    const char *cmd = NULL;
    const char *usr = NULL;
    const char *cgrp = NULL;

    usr = p2_cluster_get_username(pse.uid, username, sizeof(username));
    p2_process_info_t *pi = p2_process_info_get(pid);
    if (pi == NULL) {
      DEBUG(LOG_KEY "process cache: create %lu", pid);
      cmd = p2_cluster_get_cmdline(pid, pse.name, cmdline, sizeof(cmdline));
      cgrp = p2_cluster_get_cgroup(pid, cgroup, sizeof(cgroup));
      pi = p2_process_info_add(pid, cmd, cgrp);
    } else {
      cmd = pi->cmdline;
      cgrp = pi->cgroup;
    }
    pi->timestamp = now;

    p2_cluster_add(now, pse.name, cmd, usr, cgrp, &pse);
  }
  p2_process_info_cache_cleanup(now);

  closedir(proc);

  /* get p2_procs_running from /proc/stat
   * scanning /proc/stat AND computing other process stats takes too
   * much time. Consequently, the number of running processes based on
   * the occurences of 'R' as character indicating the running state
   * is typically zero. Due to processes are actually changing state
   * during the evaluation of it's stat(s). The 'p2_procs_running' number
   * in /proc/stat on the other hand is more accurate, and can be
   * retrieved in a single 'read' call. */
  running = p2_procs_running();

  p2_submit_state(now, "running", running);
  p2_submit_state(now, "sleeping", sleeping);
  p2_submit_state(now, "zombies", zombies);
  p2_submit_state(now, "stopped", stopped);
  p2_submit_state(now, "paging", paging);
  p2_submit_state(now, "blocked", blocked);

  p2_cluster_submit(now);

  p2_read_fork_rate();

#elif defined(__QNX__)
  iterate_processes();
#endif

#if !defined(__QNX__)
  last_read_time = now;
#endif

  cdtime_t used = cdtime() - now;
  read_time_used += used;

  DEBUG(LOG_KEY "used time %lums (total: %lums)", CDTIME_T_TO_MS(used),
        CDTIME_T_TO_MS(read_time_used));

  return 0;
}

//------------------------------------------------------------------------------
static int p2_shutdown() {
  DEBUG(LOG_KEY "p2_shutdown()");

#if !defined(__QNX__)
  p2_cluster_destroy();
  p2_user_info_cache_destroy();
  p2_process_info_cache_destroy();
#endif

  return 0;
}

//------------------------------------------------------------------------------
void module_register(void) {
  plugin_register_complex_config(PLUGIN_NAME, p2_config);
  plugin_register_init(PLUGIN_NAME, p2_init);
  plugin_register_read(PLUGIN_NAME, p2_read);
  plugin_register_shutdown(PLUGIN_NAME, p2_shutdown);
}
