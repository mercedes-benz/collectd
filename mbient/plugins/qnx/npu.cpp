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

#include "sysprofiler.h"

extern "C" {
#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"
}

static dsp::DSP_MON* dsp_mon = nullptr;

static void submit_value(value_t value) {
  value_list_t vl = VALUE_LIST_INIT;

  vl.values = &value;
  vl.values_len = 1;

  sstrncpy(vl.plugin, "npu", sizeof(vl.plugin));
  sstrncpy(vl.type, "percent", sizeof(vl.type));
  sstrncpy(vl.type_instance, "busy",
           sizeof(vl.type_instance));

  plugin_dispatch_values(&vl);
}

static void npu_submit(float val) {
  value_t _val = {0};
  _val.gauge = val;
  submit_value(_val);
}

static void npu_notify(float val) {
  cdtime_t now = cdtime();
  notification_t n = {NOTIF_OKAY, now, "", "", "npu", "", "", "", NULL};
  ssnprintf(n.message, sizeof(n.message),
    "NPU total usage: %.2f%%",
    val);
  plugin_dispatch_notification(&n);
  if (n.meta != NULL)
    plugin_notification_meta_free(n.meta);
}

static int npu_read() {
  int ret = dsp_mon->sysprofiler_getDSPSample();
  if (ret != 0) {
    ERROR("Failed to get DSP sample");
    return -1;
  }

  float val = dsp_mon->sysprofiler_getDSPUtilization();
  npu_submit(val);
  npu_notify(val);

  return 0;
}

static int npu_init() {
  dsp_mon = new dsp::DSP_MON(CDSP_ID);
  if (dsp_mon == nullptr) {
    ERROR("Failed to create DSP_MON object");
    return -1;
  }
  return 0;
}

static int npu_shutdown() {
  if (dsp_mon != nullptr) {
    delete dsp_mon;
    dsp_mon = nullptr;
  }
  return 0;
}

void module_register(void) {
  plugin_register_init("npu", npu_init);
  plugin_register_read("npu", npu_read);
  plugin_register_shutdown("npu", npu_shutdown);
} /* void module_register */
