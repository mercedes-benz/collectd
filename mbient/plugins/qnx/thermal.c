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

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

#include "amss/thermal_client.h"

#define THERMAL_DEV "/dev/thermalmgr"
#define THERMAL_DEV_WAIT_RETRIES    5

typedef struct {
  int fd;
  int num_sensors;
  char** sensor_names;
  int* sensor_data;
} thermal_dev_t;

static thermal_dev_t thermal_dev = {
  .fd = -1,
  .num_sensors = -1,
  .sensor_data = NULL,
  .sensor_names = NULL
};

static inline int wait_for_thermal_dev()
{
  int retries = THERMAL_DEV_WAIT_RETRIES;
  while (retries--) {
    if (access(THERMAL_DEV, F_OK) != 0) {
      INFO("%s not yet present. Retrying... (%d)", THERMAL_DEV, THERMAL_DEV_WAIT_RETRIES - retries);
      sleep(1);
    } else {
      INFO("%s found.", THERMAL_DEV);
      break;
    }
  }

  return retries == 0 ? -1 : 0;
}

static void cleanup() {
  if (thermal_dev.fd != -1) {
    thermal_close(thermal_dev.fd);
    thermal_dev.fd = -1;
  }

  if (thermal_dev.sensor_names) {
    for (int i = 0; i < thermal_dev.num_sensors; i++) {
      if (thermal_dev.sensor_names[i]) {
        free(thermal_dev.sensor_names[i]);
        thermal_dev.sensor_names[i] = NULL;
      }
    }
    free(thermal_dev.sensor_names);
    thermal_dev.sensor_names = NULL;
  }

  if (thermal_dev.sensor_data) {
    free(thermal_dev.sensor_data);
    thermal_dev.sensor_data = NULL;
  }
}

static int init() {
  thermal_sts_type ret;

  if (wait_for_thermal_dev() != 0) {
    ERROR("%s in not present.", THERMAL_DEV);
    return -1;
  }

  ret = thermal_open(THERMAL_DEV, &thermal_dev.fd);
  if (ret != THERMAL_STATUS_SUCCESS) {
    ERROR("Failed to open thermal device: %d\n", ret);
    return -1;
  }

  ret = thermal_get_num_sensors(thermal_dev.fd, &thermal_dev.num_sensors);
  if (ret != THERMAL_STATUS_SUCCESS) {
    ERROR("thermal_get_num_sensors failed with error: %d\n", ret);
    cleanup();
    return -2;
  }

  thermal_dev.sensor_names = calloc(thermal_dev.num_sensors, sizeof(char*));
  if (thermal_dev.sensor_names == NULL) {
    ERROR("Out of memory!\n");
    cleanup();
    return -5;
  }

  for (int i = 0; i < thermal_dev.num_sensors; i++) {
    thermal_dev.sensor_names[i] = calloc(1, MAX_SENSOR_NAME_LEN);
    if (thermal_dev.sensor_names[i] == NULL) {
      ERROR("Out of memory!\n");
      cleanup();
      return -5;
    }
  }

  thermal_dev.sensor_data = calloc(thermal_dev.num_sensors, sizeof(int));
  if (thermal_dev.sensor_data == NULL) {
    ERROR("Out of memory!\n");
    cleanup();
    return -5;
  }

  ret = thermal_get_sensor_names(thermal_dev.fd, thermal_dev.sensor_names);
  if (ret != THERMAL_STATUS_SUCCESS) {
    ERROR("Could not read sensor names: %d\n", ret);
    cleanup();
    return -3;
  }

  return 0;
}

static void submit_value(const char* sensor_name, value_t value) {
  value_list_t vl = VALUE_LIST_INIT;

  vl.values = &value;
  vl.values_len = 1;

  sstrncpy(vl.plugin, "thermal", sizeof(vl.plugin));
  sstrncpy(vl.plugin_instance, sensor_name, sizeof(vl.plugin_instance));
  sstrncpy(vl.type, "temperature", sizeof(vl.type));

  plugin_dispatch_values(&vl);
}

static void submit_temperature(const char* sensor_name, gauge_t value) {
  submit_value(sensor_name, (value_t){.gauge = value});
}

static int thermal_read() {
  thermal_sts_type ret;
  int num_samples = 1;
  int max_temp_sensor_index = -1;
  int max_temp = 0;

  for (int i = 0; i < thermal_dev.num_sensors; i++) {
    ret = thermal_get_sensor_temp(thermal_dev.fd, thermal_dev.sensor_names[i],
                                  &num_samples, &thermal_dev.sensor_data[i]);
    if (ret != THERMAL_STATUS_SUCCESS) {
      ERROR("Failed to read data for sensor %s (error %d)\n", thermal_dev.sensor_names[i], ret);
      return -1;
    }
    if (max_temp < thermal_dev.sensor_data[i]) {
      max_temp_sensor_index = i;
      max_temp = thermal_dev.sensor_data[i];
    }
  }
  submit_temperature(thermal_dev.sensor_names[max_temp_sensor_index], (gauge_t)thermal_dev.sensor_data[max_temp_sensor_index]);
  return 0;
}

static int thermal_shutdown() {
  cleanup();
  return 0;
}

void module_register(void) {
  plugin_register_init("thermal", init);
  plugin_register_read("thermal", thermal_read);
  plugin_register_shutdown("thermal", thermal_shutdown);
}
