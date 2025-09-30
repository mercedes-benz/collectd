/**
 * The MIT License
 *
 * Copyright (C) 2023 MBition GmbH
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
 *   Emil Velikov <emil dot velikov at mercedes-benz.com>
 */

#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/slog2.h>
#include <slog2_parse.h>
#include <glob.h>
#include <regex.h>

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

#define MAX(a,b) \
  ({ __typeof__ (a) _a = (a); \
      __typeof__ (b) _b = (b); \
    _a > _b ? _a : _b; })

#define MAX_GPU_PROCESS 16
#define MAX_STRING_LEN    128
#define KGSL_CONTROL_DEV "/dev/kgsl-control"
#define KGSL_SLOG_BUF_PATTERN "/dev/shmem/slogger2/kgsl.[0-9]*"
#define GPU_TOTAL_BUSY_REGEX_NUM_MATCHES 2
#define GPU_TOTAL_BUSY_REGEX "frame.*busy = ([0-9]*\\.[0-9]*)"
#define GPU_PER_PROCESS_BUSY_REGEX_NUM_MATCHES 5
#define GPU_PER_PROCESS_BUSY_REGEX "PID:([0-9]*)\\] = '([[:print:]]*)' the GPU busy = ([0-9]*\\.[0-9]*).* CtxtID = ([0-9]*)"
#define MAX_REGEX_NUM_MATCHES MAX(GPU_TOTAL_BUSY_REGEX_NUM_MATCHES, GPU_PER_PROCESS_BUSY_REGEX_NUM_MATCHES)

typedef struct {
  int pid;
  char process_name[MAX_STRING_LEN];
  double gpu_busy;
  int ctxtid;
} per_process_gpu_busy_t;

typedef struct {
  double total_gpu_busy;
  int num_of_gpu_processes;
  per_process_gpu_busy_t* processes;
} gpu_busy_t;

static slog2_log_t kgsl_slog2_handle;
static regex_t gpu_total_busy_re;
static regex_t gpu_per_process_busy_re;
static gpu_busy_t gpu;

static inline int write_and_verify(int fd, const char* str_to_write) {
  ssize_t bytes_written = 0;
  ssize_t bytes_to_write = 0;

  bytes_to_write = strlen(str_to_write);
  bytes_written = write(fd, str_to_write, bytes_to_write);
  if (bytes_written != bytes_to_write) {
    ERROR("Failed to write string: %s to %s (error %s)",
          str_to_write, KGSL_CONTROL_DEV, strerror(errno));
    return -1;
  }
  return 0;
}

static int find_filename_by_pattern(const char* pattern, char* filename) {
  int ret = 0;
  glob_t paths;

  paths.gl_pathc = 0;
  paths.gl_pathv = NULL;
  paths.gl_offs = 0;

  if ((pattern == NULL) || (filename == 0)) {
    ERROR("Invalid argument");
    return -1;
  }

  ret = glob(pattern, GLOB_NOCHECK | GLOB_NOSORT, NULL, &paths);
  if (ret != 0) {
    ERROR("glob failed with error %d", ret);
    return ret;
  }

  if (paths.gl_pathc != 1) {
    ERROR("Found invalid number of paths: %lu", paths.gl_pathc);
    globfree(&paths);
    return -1;
  }

  strncpy(filename, paths.gl_pathv[0], MAX_STRING_LEN - 1);
  globfree(&paths);
  return 0;
}

static int control_gpu_stats(bool enable) {
  int fd = -1;
  const char* log_level_string = "gpu_set_log_level 4";
  char per_process_busy_string[MAX_STRING_LEN] = {0};
  char busystats_string[MAX_STRING_LEN] = {0};

  /* Setting -1 as interval disables the reporting */
  long interval = enable ? CDTIME_T_TO_MS(plugin_get_interval()) : -1;
  snprintf(per_process_busy_string, MAX_STRING_LEN, "gpu_per_process_busy %ld", interval);
  snprintf(busystats_string, MAX_STRING_LEN, "gpubusystats %ld", interval);

  fd = open(KGSL_CONTROL_DEV, O_WRONLY);
  if (fd < 0) {
    ERROR("Failed to open %s (error %s)", KGSL_CONTROL_DEV, strerror(errno));
    return -1;
  }

  /* echo gpu_set_log_level 4 > /dev/kgsl-control */
  if (write_and_verify(fd, log_level_string) < 0) {
    close(fd);
    return -1;
  }

  /* echo gpu_per_process_busy 1000 > /dev/kgsl-control */
  if (write_and_verify(fd, per_process_busy_string) < 0) {
    close(fd);
    return -1;
  }

  /* echo gpubusystats 1000 > /dev/kgsl-control */
  if (write_and_verify(fd, busystats_string) < 0) {
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

static int enable_gpu_stats() {
  return control_gpu_stats(true);
}

static int disable_gpu_stats() {
  return control_gpu_stats(false);
}

static int init_ksgl_slog2_handle() {
  int ret = 0;
  char kgsl_slog2_filename[MAX_STRING_LEN] = {0};

  ret = find_filename_by_pattern(KGSL_SLOG_BUF_PATTERN, kgsl_slog2_filename);
  if (ret != 0) {
    ERROR("Could not get filename for pattern %s", KGSL_SLOG_BUF_PATTERN);
    return ret;
  }

  kgsl_slog2_handle = slog2_open_log(kgsl_slog2_filename);
  if (kgsl_slog2_handle == 0) {
    ERROR("Failed to open slog2 handle for %s", kgsl_slog2_filename);
    return -1;
  }

  return 0;
}

static per_process_gpu_busy_t* find_process_by_ctxtid(gpu_busy_t* gpu, int ctxtid) {
  for (int i = 0; i < gpu->num_of_gpu_processes; i++) {
    if (gpu->processes[i].ctxtid == ctxtid) {
      return &gpu->processes[i];
    }
  }
  return NULL;
}

static void cleanup() {
  if (kgsl_slog2_handle != 0) {
    slog2_close_log(kgsl_slog2_handle);
    kgsl_slog2_handle = 0;
  }

  if (gpu.processes != NULL) {
    free(gpu.processes);
    gpu.processes = NULL;
  }

  regfree(&gpu_total_busy_re);
  regfree(&gpu_per_process_busy_re);
}

static int sort_processes_per_gpu_usage(const void* this, const void* other) {
  per_process_gpu_busy_t* t = (per_process_gpu_busy_t*)this;
  per_process_gpu_busy_t* o = (per_process_gpu_busy_t*)other;

  if (t->gpu_busy < o->gpu_busy) return 1;
  else if (t->gpu_busy > o->gpu_busy) return -1;
  return 0;
}

static per_process_gpu_busy_t* get_new_process_entry(gpu_busy_t* gpu) {
  per_process_gpu_busy_t* process = NULL;
  if (gpu->num_of_gpu_processes < MAX_GPU_PROCESS) {
    process = &gpu->processes[gpu->num_of_gpu_processes++];
  } else {
    ERROR("Too many processes to track GPU usage. Increase MAX_GPU_PROCESS (%d)!", MAX_GPU_PROCESS);
  }
  return process;
}

static void gpu_total_busy_slog2_callback(void* payload, regmatch_t* matches, void* param) {
  char buffer[MAX_STRING_LEN] = {0};
  gpu_busy_t* gpu = (gpu_busy_t*)param;

  memcpy(buffer, (char*)(payload) + matches[1].rm_so, matches[1].rm_eo - matches[1].rm_so);
  gpu->total_gpu_busy = atof(buffer);
}

static void gpu_per_process_busy_slog2_callback(void* payload, regmatch_t* matches, void* param) {
  char pid[MAX_STRING_LEN] = {0};
  char process_name[MAX_STRING_LEN] = {0};
  char gpu_busy[MAX_STRING_LEN] = {0};
  char ctxtid[MAX_STRING_LEN] = {0};
  gpu_busy_t* gpu = (gpu_busy_t*)param;

  if (gpu == NULL) {
    ERROR("Argument is NULL! GPU per process load will be invalid!");
    return;
  }

  memcpy(pid, (char*)(payload) + matches[1].rm_so, matches[1].rm_eo - matches[1].rm_so);
  memcpy(process_name, (char*)(payload) + matches[2].rm_so, matches[2].rm_eo - matches[2].rm_so);
  memcpy(gpu_busy, (char*)(payload) + matches[3].rm_so, matches[3].rm_eo - matches[3].rm_so);
  memcpy(ctxtid, (char*)(payload) + matches[4].rm_so, matches[4].rm_eo - matches[4].rm_so);

  /* Handle cases there we have multiple reports for the same PID in one interval.
   * Otherwise, if the process restarts it will spam the logs with "Too many processes to track ..."
   * In this case, we will overwrite the previous entry with the new one
   */
  per_process_gpu_busy_t* process = find_process_by_ctxtid(gpu, atoi(ctxtid));
  if (!process) {
    process = get_new_process_entry(gpu);
    if (!process) {
      ERROR("Failed to get new process entry!");
      return;
    }
  }

  strncpy(process->process_name, process_name, MAX_STRING_LEN);
  process->pid = atoi(pid);
  process->gpu_busy = atof(gpu_busy);
  process->ctxtid = atoi(ctxtid);
}

static int slog2_callback(slog2_packet_info_t* info, void* payload, void* param) {
  int ret;
  regmatch_t match[MAX_REGEX_NUM_MATCHES];

  static uint64_t last_timestamp;
  static uint16_t last_sequence_number;
  /* There are two ways to parse slog2 buffers - slog2_parse_static_buffer and
   * slog2_parse_dynamic_buffer. The difference is that when called the former
   * calls the callback for each entry in the given slog2 buffer since its
   * beginning (does not store the offset) and returns. On the other hand,
   * when the latter is called it blocks forever and calls the callback when
   * new entry is added to the given buffer. We use the former to comply with
   * the collectd architecture (otherwise, we would have to create a separate thread
   * which will call slog2_parse_dynamic_buffer and block forever). However, we add
   * the check below to avoid processing the same entry multiple times.
   * "info->timestamp" is uint64 so we don't bother with overflow.
   */
  if ((last_timestamp > info->timestamp) ||
      ((last_timestamp == info->timestamp) && (last_sequence_number >= info->sequence_number))) {
    return 0;
  }
  last_timestamp = info->timestamp;
  last_sequence_number = info->sequence_number;

  ret = regexec(&gpu_total_busy_re, payload, MAX_REGEX_NUM_MATCHES, match, 0);
  if (ret == 0) {
    gpu_total_busy_slog2_callback(payload, match, param);
  } else {
    ret = regexec(&gpu_per_process_busy_re, payload, MAX_REGEX_NUM_MATCHES, match, 0);
    if (ret == 0) {
      gpu_per_process_busy_slog2_callback(payload, match, param);
    } else {
      /* do nothing */
    }
  }

  return 0;
}

static int get_gpu_load(slog2_log_t handle, void* param) {
  slog2_log_info_t log_info = SLOG2_LOG_INFO_INIT;
  slog2_packet_info_t packet_info = SLOG2_PACKET_INFO_INIT;

  if (slog2_get_log_info(handle, &log_info) == -1) {
    ERROR("Couldn't get information about the log.");
    return -1;
  }
  for (int buffer_index = 0; buffer_index < log_info.num_buffers; buffer_index++) {
    if (slog2_parse_static_buffer(handle, buffer_index, &packet_info,
                                    slog2_callback, param) == -1) {
      ERROR("slog2_parse_static_buffer() failed.");
      return -1;
    }
  }
  return 0;
}

static inline void sort_gpu_processes(gpu_busy_t* gpu) {
  qsort(gpu->processes, gpu->num_of_gpu_processes, sizeof(per_process_gpu_busy_t), sort_processes_per_gpu_usage);
}

static void reset_gpu_stats(gpu_busy_t* gpu) {
  gpu->num_of_gpu_processes = 0;
  /* No need to memset the array as we traverse up to num_of_gpu_processes */
}

static void submit_value(value_t value) {
  value_list_t vl = VALUE_LIST_INIT;

  vl.values = &value;
  vl.values_len = 1;

  sstrncpy(vl.plugin, "gpu", sizeof(vl.plugin));
  sstrncpy(vl.type, "percent", sizeof(vl.type));
  sstrncpy(vl.type_instance, "busy",
           sizeof(vl.type_instance));

  plugin_dispatch_values(&vl);
}

static void gpu_submit(gpu_busy_t* gpu) {
  submit_value((value_t)gpu->total_gpu_busy);
}

static void gpu_notify(gpu_busy_t* gpu) {
  cdtime_t now = cdtime();
  notification_t n = {NOTIF_OKAY, now, "", "", "gpu", "", "", "", NULL};
  ssnprintf(n.message, sizeof(n.message),
          "GPU total usage: %.2f%%",
          gpu->total_gpu_busy);

  plugin_dispatch_notification(&n);

  strncpy(n.message, "GPU top 10 processes - process/GPU usage/CtxtID: ", sizeof(n.message));
  for (int i = 0; i < gpu->num_of_gpu_processes; i++) {
    char temp_string[32] = {0};
    per_process_gpu_busy_t *process = &gpu->processes[i];
    ssnprintf(temp_string, sizeof(temp_string),
          "%s/%.2f%%/%d; ",
          process->process_name, process->gpu_busy, process->ctxtid);

    strncat(n.message, temp_string, sizeof(n.message) - 1);
  }

  plugin_dispatch_notification(&n);
  if (n.meta != NULL)
    plugin_notification_meta_free(n.meta);
}

static int gpu_init() {
  int ret = 0;

  gpu.processes = calloc(MAX_GPU_PROCESS, sizeof(per_process_gpu_busy_t));
  if (gpu.processes == NULL) {
    ERROR("Failed to allocate memory for GPU processes");
    return -1;
  }

  ret = enable_gpu_stats();
  if (ret != 0) {
    ERROR("Failed to enable GPU stats");
    return ret;
  }

  ret = regcomp(&gpu_total_busy_re, GPU_TOTAL_BUSY_REGEX, REG_EXTENDED);
  if (ret != 0) {
    char error_buf[MAX_STRING_LEN] = {0};
    regerror(ret, &gpu_total_busy_re, error_buf, sizeof(error_buf));
    ERROR("%s", error_buf);
    return ret;
  }

  ret = regcomp(&gpu_per_process_busy_re, GPU_PER_PROCESS_BUSY_REGEX, REG_EXTENDED);
  if (ret != 0) {
    char error_buf[MAX_STRING_LEN] = {0};
    regerror(ret, &gpu_per_process_busy_re, error_buf, sizeof(error_buf));
    ERROR("%s", error_buf);
    return ret;
  }

  return 0;
}

static int gpu_read() {
  /* init_ksgl_slog2_handle() is called in the read callback because:
   * GPU stats start to be reported after one interval.
   * This is the point when the kgsl slog2 buffer appears.
   * We skip two intervals to make sure that the buffer is created.
   * If we call "sleep()" in the init callback, the startup of all plugins will be delayed.
   */
  static const int num_readings_to_skip = 2;
  static int readings_skipped = 0;
  if (readings_skipped < num_readings_to_skip) {
    readings_skipped++;
    return 0;
  }

  if (kgsl_slog2_handle == 0) {
    int ret = init_ksgl_slog2_handle();
    if (ret != 0) {
      ERROR("Failed to create slog2 handles");
      return ret;
    }
  }
  reset_gpu_stats(&gpu);
  get_gpu_load(kgsl_slog2_handle, &gpu);
  sort_gpu_processes(&gpu);
  gpu_notify(&gpu);
  gpu_submit(&gpu);

  return 0;
}

static int gpu_shutdown() {
  disable_gpu_stats();
  cleanup();
  return 0;
}

void module_register(void) {
  plugin_register_init("gpu", gpu_init);
  plugin_register_read("gpu", gpu_read);
  plugin_register_shutdown("gpu", gpu_shutdown);
} /* void module_register */
