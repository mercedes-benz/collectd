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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/neutrino.h>
#include <sys/procfs.h>
#include <sys/time.h>

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))

#define STRING_LEN 32
#define PROCNTO_CTL_FILE "/proc/1/ctl"
#define MAX_QVM_NUM 8

typedef struct {
  uint64_t sutime;       /* total system + user time for the thread */
  uint64_t uptime;       /* total time since last measurement was taken */
  double percent_active; /* percent of the time the thread was active in the
                            *past interval* */
} cpu_info_t;

typedef struct {
  char *name;
  int pid;
  int num_vcpus;
  cpu_info_t *vcpu_info;
  double total_load;
  bool skip_reporting;
} qvm_stats_t;

typedef struct {
  char *name;
  char *display_name;
} internal_to_display_name_map_t;

typedef struct {
  int num_cpus;
  cpu_info_t *cpu_info;
  double total_load;
} host_stats_t;

static int num_running_qvms = 0;
static qvm_stats_t qvm_stats[MAX_QVM_NUM];
static host_stats_t host_stats;
static double safeos_load;

static internal_to_display_name_map_t internal_to_display_name_map[] = {
    {"lv", "RichOS"}, {"tgvm", "TgVM"}};

static inline char *get_display_name(char *internal_name) {
  for (int i = 0; i < ARRAY_SIZE(internal_to_display_name_map); i++) {
    if (strcmp(internal_name, internal_to_display_name_map[i].name) == 0) {
      return internal_to_display_name_map[i].display_name;
    }
  }
  return internal_name;
}

static void submit_value(int cpu_num, const char *cpu_state, const char *type,
                         value_t value) {
  value_list_t vl = VALUE_LIST_INIT;

  vl.values = &value;
  vl.values_len = 1;

  sstrncpy(vl.plugin, "cpu", sizeof(vl.plugin));
  sstrncpy(vl.type, type, sizeof(vl.type));
  sstrncpy(vl.type_instance, cpu_state, sizeof(vl.type_instance));

  if (cpu_num >= 0) {
    snprintf(vl.plugin_instance, sizeof(vl.plugin_instance), "%i", cpu_num);
  }
  plugin_dispatch_values(&vl);
}

static void submit_percent(int cpu_num, const char *cpu_state, gauge_t value) {
  /* This function is called for all known CPU states, but each read
   * method will only report a subset. The remaining states are left as
   * NAN and we ignore them here. */
  if (isnan(value))
    return;

  submit_value(cpu_num, cpu_state, "percent", (value_t){.gauge = value});
}

static void cpu_notify() {
  const cdtime_t now = cdtime();
  notification_t n = {NOTIF_OKAY, now, "", "", "cpu", "", "", "", NULL};

  strncpy(n.message, "CPU per core usage: ", sizeof(n.message));
  n.message[sizeof(n.message) - 1] = '\0';

  size_t current_len = strlen(n.message);
  for (int i = 0; i < host_stats.num_cpus; i++) {
    char temp_string[STRING_LEN] = {0};
    ssnprintf(temp_string, sizeof(temp_string), "core %d: %.2f%%; ", i,
              host_stats.cpu_info[i].percent_active);

    size_t remaining = sizeof(n.message) - current_len - 1;
    if (remaining > 0) {
      size_t len = strlen(temp_string);
      size_t copy_len = (len < remaining) ? len : remaining;
      strncat(n.message, temp_string, remaining);
      current_len += copy_len;
    }
  }
  plugin_dispatch_notification(&n);

  ssnprintf(n.message, sizeof(n.message), "CPU total usage: %.2f%%",
            host_stats.total_load);
  plugin_dispatch_notification(&n);

  const int fdesc = open("/dev/qnx-critical-logging", O_WRONLY);
  if (fdesc != -1) {

    write(fdesc, n.message, strlen(n.message) + 1);
    close(fdesc);
  }

  ssnprintf(n.message, sizeof(n.message), "CPU total SafeOS usage: %.2f%%",
            safeos_load);
  plugin_dispatch_notification(&n);

  for (int i = 0; i < num_running_qvms; i++) {
    qvm_stats_t *qvm = &qvm_stats[i];
    if (qvm->skip_reporting) {
      qvm->skip_reporting = false;
      continue;
    }

    ssnprintf(n.message, sizeof(n.message), "CPU total %s usage: %.2f%%",
              get_display_name(qvm->name), qvm->total_load);
    plugin_dispatch_notification(&n);

    ssnprintf(n.message, sizeof(n.message),
              "CPU per %s vCPU core usage: ", get_display_name(qvm->name));

    current_len = strlen(n.message);
    for (int j = 0; j < qvm->num_vcpus; j++) {
      char temp_string[STRING_LEN] = {0};
      ssnprintf(temp_string, sizeof(temp_string), "core %d: %.2f%%; ", j,
                qvm->vcpu_info[j].percent_active);

      size_t remaining = sizeof(n.message) - current_len - 1;
      if (remaining > 0) {
        size_t len = strlen(temp_string);
        size_t copy_len = (len < remaining) ? len : remaining;
        strncat(n.message, temp_string, remaining);
        current_len += copy_len;
      }
    }
    plugin_dispatch_notification(&n);
  }
}

static void cpu_submit() {
  for (int i = 0; i < host_stats.num_cpus; i++) {
    submit_percent(i, "active", (gauge_t)host_stats.cpu_info[i].percent_active);
  }
  submit_percent(-1, "active", host_stats.total_load);
}

static void
calculate_cpu_percent_active(const procfs_status *const thread_status,
                             cpu_info_t *const cpu_info) {
  uint64_t sutime, uptime;
  uint64_t delta_sutime, delta_uptime;
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  sutime = thread_status->sutime / (1000 * 1000); // in ms
  uptime = ts.tv_sec * 1000 + ts.tv_nsec / (1000 * 1000);

  delta_sutime = sutime - cpu_info->sutime;
  delta_uptime = uptime - cpu_info->uptime;

  cpu_info->sutime = sutime;
  cpu_info->uptime = uptime;

  if (delta_uptime > 0) {
    cpu_info->percent_active = ((double)delta_sutime / delta_uptime) * 100.0;
  } else {
    cpu_info->percent_active = 0.0;
  }
}

static int get_cpus_load() {
  int fd = -1;
  procfs_status thread_status;

  /* Open connection to procnto stats */
  fd = open(PROCNTO_CTL_FILE, O_RDONLY);
  if (fd < 0) {
    ERROR("Failed to open %s (error %s)", PROCNTO_CTL_FILE, strerror(errno));
    return -1;
  }

  /* Get the amount of time when idle threads were running */
  for (int i = 0; i < host_stats.num_cpus; i++) {
    thread_status.tid = i + 1;
    if (devctl(fd, DCMD_PROC_TIDSTATUS, &thread_status, sizeof(thread_status),
               NULL) == -1) {
      ERROR("Can't get info for thread %d (error %s)", thread_status.tid,
            strerror(errno));
      close(fd);
      return -1;
    }
    calculate_cpu_percent_active(&thread_status, &host_stats.cpu_info[i]);
    /* To get the active percentage for a CPU core we need to subtract the
     * active time of idle threads from 100 */
    host_stats.cpu_info[i].percent_active =
        100.0 - host_stats.cpu_info[i].percent_active;
  }
  close(fd);
  return 0;
}

static int get_host_cpu_load() {
  if (get_cpus_load() != 0) {
    return -1;
  }

  host_stats.total_load = 0;
  for (int i = 0; i < host_stats.num_cpus; i++) {
    host_stats.total_load += host_stats.cpu_info[i].percent_active;
  }
  host_stats.total_load /= host_stats.num_cpus;
  return 0;
}

/* We get the number and the names of the QVMs from the
 * /dev/qvm/ directory. The names are the same as the
 * directories in /dev/qvm/ and the number of the QVMs
 * is the number of the directories in /dev/qvm/
 */
static int detect_qvms() {
  DIR *dir;
  struct dirent *ent;
  struct stat statbuf;
  const char *qvm_dir = "/dev/qvm";
  int qvm_num = 0;

  if ((dir = opendir(qvm_dir)) == NULL) {
    ERROR("Failed to open %s: %s", qvm_dir, strerror(errno));
    return -1;
  }

  while ((ent = readdir(dir)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }

    if (qvm_num >= MAX_QVM_NUM) {
      WARNING("Maximum number of QVMs (%d) reached, ignoring additional QVMs", MAX_QVM_NUM);
      break;
    }

    char fullpath[1024];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", qvm_dir, ent->d_name);

    if (stat(fullpath, &statbuf) == 0 && S_ISDIR(statbuf.st_mode)) {
      qvm_stats[qvm_num].name = strdup(ent->d_name);
      if (qvm_stats[qvm_num].name == NULL) {
        ERROR("Out of memory!");
        for (int i = 0; i < qvm_num; i++) {
          free(qvm_stats[i].name);
          qvm_stats[i].name = NULL;
        }
        closedir(dir);
        return -1;
      }
      qvm_num++;
    }
  }
  closedir(dir);
  return qvm_num;
}

/* We get the PID by reading the /tmp/<qvm_name> file. First number is the PID.
 */
static int get_qvm_pid(char *qvm_name) {
  char path[STRING_LEN] = {0};
  int pid = -1;

  snprintf(path, sizeof(path), "/tmp/%s", qvm_name);
  FILE *pid_file = fopen(path, "r");
  if (pid_file == NULL) {
    ERROR("Failed to open %s: %s", path, strerror(errno));
    return -1;
  }

  if (fscanf(pid_file, "%d", &pid) != 1) {
    ERROR("Failed to read PID from %s", path);
    fclose(pid_file);
    return -1;
  }
  fclose(pid_file);
  return pid;
}

static int get_num_qvm_vcpus(int qvm_index) {
  int num_vcpus = 0;
  int qvm_pid = qvm_stats[qvm_index].pid;

  if (qvm_pid <= 0) {
    ERROR("Invalid PID for %s VM", qvm_stats[qvm_index].name);
    return -1;
  }

  char threadname[_NTO_THREAD_NAME_MAX] = {0};
  procfs_status thread_status;

  /* virtual CPU thread IDs are sequential and start from TID 2 */
  const int MAX_VCPUS = 256;
  for (int i = 0; i < MAX_VCPUS; i++) {
    thread_status.tid = i + 2;
    if (__getset_thread_name(qvm_pid, thread_status.tid, NULL, -1, threadname,
                             sizeof(threadname)) == EOK) {
      if (strstr(threadname, "virtual cpu")) {
        num_vcpus++;
      } else {
        break;
      }
    }
  }

  return num_vcpus;
}

static int handle_qvm_reset(int qvm_index) {
  qvm_stats_t *qvm = &qvm_stats[qvm_index];
  qvm->skip_reporting = true;
  int qvm_pid = get_qvm_pid(qvm->name);
  if (qvm_pid == -1) {
    ERROR("Failed to get PID for %s VM", get_display_name(qvm->name));
    return -1;
  }

  qvm->pid = qvm_pid;
  WARNING("New PID for %s VM: %d", get_display_name(qvm->name), qvm->pid);
  int num_vcpus = get_num_qvm_vcpus(qvm_index);
  if (num_vcpus == -1) {
    ERROR("Failed to get number of vCPUs for %s VM",
          get_display_name(qvm->name));
    return -1;
  }

  /* It is very unlikely that number of vCPUs change between QVM resets.
   * However, it is still possible that someone stops the QVM, changes its
   * configuration and starts it again with a different number of vCPUs.
   */
  if (num_vcpus != qvm->num_vcpus) {
    INFO("Number of vCPUs for %s VM changed from %d to %d",
         get_display_name(qvm->name), qvm->num_vcpus, num_vcpus);
    if (qvm->vcpu_info) {
      free(qvm->vcpu_info);
      qvm->vcpu_info = NULL;
    }
    qvm->vcpu_info = calloc(num_vcpus, sizeof(cpu_info_t));
    if (qvm->vcpu_info == NULL) {
      ERROR("Out of memory!");
      return -1;
    }
    qvm->num_vcpus = num_vcpus;
  } else {
    memset(qvm->vcpu_info, 0, qvm->num_vcpus * sizeof(cpu_info_t));
  }
  return 0;
}

static int get_vcpus_load(int qvm_index) {
  char path[STRING_LEN] = {0};
  procfs_status thread_status;
  qvm_stats_t *qvm = &qvm_stats[qvm_index];

  snprintf(path, sizeof(path), "/proc/%d/ctl", qvm->pid);
  int fd = open(path, O_RDONLY);
  if (fd == -1) {
    ERROR("Failed to open %s: %s", path, strerror(errno));
    if (errno == ENOENT) {
      WARNING("%s VM is not running. Most probably crashed and was reset. "
              "Current reading will be incorrect!",
              get_display_name(qvm->name));
      return handle_qvm_reset(qvm_index);
    }
    return -1;
  }

  for (int i = 0; i < qvm->num_vcpus; i++) {
    thread_status.tid = i + 2;
    if (devctl(fd, DCMD_PROC_TIDSTATUS, &thread_status, sizeof(thread_status),
               NULL) == -1) {
      ERROR("Can't get info for thread %d (error %s)", thread_status.tid,
            strerror(errno));
      close(fd);
      return -1;
    }
    calculate_cpu_percent_active(&thread_status, &qvm->vcpu_info[i]);
  }
  close(fd);
  return 0;
}

static int get_qvm_load(int qvm_index) {
  qvm_stats_t *qvm = &qvm_stats[qvm_index];

  if (get_vcpus_load(qvm_index) != 0) {
    return -1;
  }

  qvm->total_load = 0;
  for (int i = 0; i < qvm->num_vcpus; i++) {
    qvm->total_load += qvm->vcpu_info[i].percent_active;
  }
  qvm->total_load /= host_stats.num_cpus;
  return 0;
}

static int cpu_init() {
  host_stats.num_cpus = (size_t)sysconf(_SC_NPROCESSORS_CONF);
  if (host_stats.num_cpus < 1) {
    ERROR("Failed to get the number of cores!");
    return -1;
  }

  host_stats.cpu_info = calloc(host_stats.num_cpus, sizeof(cpu_info_t));
  if (host_stats.cpu_info == NULL) {
    ERROR("Out of memory!");
    return -1;
  }

  num_running_qvms = detect_qvms();
  if (num_running_qvms < 0) {
    ERROR("Failed to detect running QVMs");
    goto cleanup;
  }

  for (int i = 0; i < num_running_qvms; i++) {
    qvm_stats_t *qvm = &qvm_stats[i];
    qvm->pid = get_qvm_pid(qvm->name);
    if (qvm->pid == -1) {
      ERROR("Failed to get PID for %s", get_display_name(qvm->name));
      num_running_qvms = i;  // Only cleanup what was allocated
      goto cleanup;
    }
    qvm->num_vcpus = get_num_qvm_vcpus(i);
    if (qvm->num_vcpus == -1) {
      ERROR("Failed to get number of vCPUs for %s VM",
            get_display_name(qvm->name));
      num_running_qvms = i;
      goto cleanup;
    }
    qvm->vcpu_info = calloc(qvm->num_vcpus, sizeof(cpu_info_t));
    if (qvm->vcpu_info == NULL) {
      ERROR("Out of memory!");
      num_running_qvms = i;
      goto cleanup;
    }
  }
  return 0;

cleanup:
  if (host_stats.cpu_info) {
    free(host_stats.cpu_info);
    host_stats.cpu_info = NULL;
  }
  for (int i = 0; i < num_running_qvms; i++) {
    if (qvm_stats[i].vcpu_info) {
      free(qvm_stats[i].vcpu_info);
      qvm_stats[i].vcpu_info = NULL;
    }
    if (qvm_stats[i].name) {
      free(qvm_stats[i].name);
      qvm_stats[i].name = NULL;
    }
  }
  num_running_qvms = 0;
  return -1;
}

static int cpu_read() {
  int ret = 0;

  ret = get_host_cpu_load();
  if (ret != 0) {
    ERROR("Failed to get total CPU load: %d", ret);
    return ret;
  }

  double total_qvm_load = 0;
  for (int i = 0; i < num_running_qvms; i++) {
    ret = get_qvm_load(i);
    if (ret < 0) {
      qvm_stats_t *qvm = &qvm_stats[i];
      ERROR("Failed to get load for %s", get_display_name(qvm->name));
      continue;
      /* Even if we fail to read the load for a VM,
       * do not return error here since it will suspend the plugin.
       * Normally, both VMs should be running.
       */
    }
    total_qvm_load += qvm_stats[i].total_load;
  }
  safeos_load = host_stats.total_load - total_qvm_load;

  /* Ensure it is not negative */
  if (safeos_load < 0) {
    safeos_load = 0;
  }

  cpu_notify();
  cpu_submit();

  return 0;
}

static int cpu_shutdown() {
  if (host_stats.cpu_info) {
    free(host_stats.cpu_info);
    host_stats.cpu_info = NULL;
  }

  for (int i = 0; i < num_running_qvms; i++) {
    if (qvm_stats[i].vcpu_info) {
      free(qvm_stats[i].vcpu_info);
      qvm_stats[i].vcpu_info = NULL;
    }
    if (qvm_stats[i].name) {
      free(qvm_stats[i].name);
      qvm_stats[i].name = NULL;
    }
  }
  return 0;
}

void module_register(void) {
  plugin_register_init("cpu", cpu_init);
  plugin_register_read("cpu", cpu_read);
  plugin_register_shutdown("cpu", cpu_shutdown);
} /* void module_register */
