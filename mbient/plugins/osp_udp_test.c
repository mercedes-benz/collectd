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
#include <stdio.h>
#include <stdlib.h>
#include "liboconfig/oconfig.h"
#include "osp_udp.c"
#include "testing.h"

#define MAX_MSG_LEN 256
static int notify_counter = 0;
static int fclose_counter = 0;
const char test_data_bufer_with_drops[] =
    "sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt "
    "  uid  timeout inode ref pointer drops\n"
    "7: 00000000:0E89 00000000:0000 07 00000000:00000000 00:00000000 00000000  "
    "6100        0 45558 2 0000000000000000 5\n"
    "142: 01DCD20A:2710 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  6014        0 27185 2 0000000000000000 8\n"
    "152: 00000000:771A 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  3013        0 21352 2 0000000000000000 3\n"
    "152: 01DCD20A:771A 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  3013        0 26219 2 0000000000000000 9\n"
    "171: 01DCD20A:772D 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  3013        0 32103 2 0000000000000000 0\n"
    "174: 01DCD20A:7730 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  3013        0 37426 2 0000000000000000 3\n"
    "367: 01DC960A:9FF1 02DC960A:0035 01 00000000:00000000 00:00000000 "
    "00000000   993        0 143317 2 0000000000000000 6\n"
    "435: 0100010A:0035 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  6040        0 56660 2 0000000000000000 9\n"
    "435: 0102000A:0035 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  6040        0 56621 2 0000000000000000 15\n"
    "435: 0101000A:0035 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  6040        0 56566 2 0000000000000000 4\n"
    "435: 01E6A8C0:0035 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  3210        0 45417 2 0000000000000000 4\n"
    "435: 0103000A:0035 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000   993        0 44388 2 0000000000000000 1\n"
    "435: 3600007F:0035 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000   993        0 44386 2 0000000000000000 8\n"
    "435: 3500007F:0035 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000   993        0 44384 2 0000000000000000 3\n"
    "449: 00000000:0043 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  6040        0 56657 2 0000000000000000 7\n"
    "449: 00000000:0043 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  6040        0 56618 2 0000000000000000 9\n"
    "449: 00000000:0043 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  6040        0 56563 2 0000000000000000 1\n"
    "449: 00000000:0043 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  3210        0 45414 2 0000000000000000 5\n"
    "464: 00000000:A852 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  6040        0 71380 2 0000000000000000 8\n"
    "483: 00000000:E865 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  6040        0 95498 2 0000000000000000 2\n"
    "908: 0100007F:FA0E 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  6053        0 99429 2 0000000000000000 6\n"
    "908: 00000000:FA0E 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  6053        0 99428 2 0000000000000000 9\n"
    "972: 00000000:DA4E 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  6040        0 91478 2 0000000000000000 4\n"
    "979: 00000000:E255 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  6040        0 89801 2 0000000000000000 4\n"
    "990: 01DCD20A:EA60 04DCD20A:772D 01 00000000:00000000 00:00000000 "
    "00000000  3013        0 43367 2 0000000000000000 2\n"
    "990: 01DCD20A:EA60 04DCD20A:7918 01 00000000:00000000 00:00000000 "
    "00000000  3013        0 38631 2 0000000000000000 1\n"
    "990: 01DCD20A:EA60 07DCD20A:76C1 01 00000000:00000000 00:00000000 "
    "00000000  3013        0 34670 2 0000000000000000 7\n";

const char test_data_bufer_without_drops[] =
    "sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt "
    "  uid  timeout inode ref pointer drops\n"
    "7: 00000000:0E89 00000000:0000 07 00000000:00000000 00:00000000 00000000  "
    "6100        0 45558 2 0000000000000000 0\n"
    "142: 01DCD20A:2710 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  6014        0 27185 2 0000000000000000 0\n"
    "152: 00000000:771A 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  3013        0 21352 2 0000000000000000 0\n"
    "152: 01DCD20A:771A 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  3013        0 26219 2 0000000000000000 0\n"
    "171: 01DCD20A:772D 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  3013        0 32103 2 0000000000000000 0\n";

static void testReadUDPInfoNoDrops(char *arr, uint16_t len) {
  // 45 bytes
  char test_arr[] =
      "367: 01DC960A:9FF1 02DC960A:0035 01 00000000:00000000 00:00000000 "
      "00000000   993        0 143317 2 0000000000000000 0\n";
  memcpy(arr, test_arr, len);
}

static void testReadUDPInfoOneDrop(char *arr, uint16_t len) {
  // 45 bytes
  char test_arr[] =
      "990: 01DCD20A:EA60 04DCD20A:772D 01 00000000:00000000 00:00000000 "
      "00000000  3013        0 43367 2 0000000000000000 1";
  memcpy(arr, test_arr, len);
}

int plugin_dispatch_multivalue(value_list_t const *vl, bool store_percentage,
                               int store_type, ...) {
  return 0;
}

static int osp_notify_mock(char *buf, int buf_len) { return ++notify_counter; }
static FILE *fopen_osp_mock(const char *buf, const char *mode) {
  return fmemopen((char *)buf, strlen(buf), "r");
}
static int fclose_osp_mock(FILE *file) {
  fclose(file);
  return ++fclose_counter;
}

static Node *osp_udp_searchNode(Node *root, uint32_t uid) {
  Node *result = NULL;
  if (root->m_key > uid) {
    if (root->m_left)
      result = osp_udp_searchNode(root->m_left, uid);
  } else if (root->m_key < uid) {
    if (root->m_right)
      result = osp_udp_searchNode(root->m_right, uid);
  } else {
    result = root;
  }
  return result;
}
/* ************************************************************************** */
/* tests */
/* ************************************************************************** */

/* -------------------------------------------------------------------------- */
DEF_TEST(ParseUDPInfoNoDrop) {
  char tmpbuf[NOTIF_MAX_MSG_LEN] = {0};
  int result = 0;
  struct osp_udp_proc_udp udp = {0};
  testReadUDPInfoNoDrops(tmpbuf, NOTIF_MAX_MSG_LEN);
  result = osp_udp_parse_udp(&udp, tmpbuf);
  EXPECT_IN_STR("10.150.22", udp.local_address);
  EXPECT_IN_STR("61855", udp.local_port);
  EXPECT_EQ_INT(993, udp.uid);
  EXPECT_EQ_INT(0, udp.drop);
  return result;
}

/* -------------------------------------------------------------------------- */

DEF_TEST(ParseUDPInfoOneDrop) {
  char tmpbuf[NOTIF_MAX_MSG_LEN] = {0};
  int result = 0;
  struct osp_udp_proc_udp udp = {0};
  testReadUDPInfoOneDrop(tmpbuf, NOTIF_MAX_MSG_LEN);
  result = osp_udp_parse_udp(&udp, tmpbuf);
  EXPECT_IN_STR("10.210.22", udp.rem_address);
  EXPECT_IN_STR("11639", udp.rem_port);
  EXPECT_EQ_INT(3013, udp.uid);
  EXPECT_EQ_INT(1, udp.drop);
  return result;
}

/* -------------------------------------------------------------------------- */

DEF_TEST(TreeTest) {
  int arr[] = {25, 21, 14, 13, 12, 11, 10, 15, 16, 17, 18, 19, 20, 27};
  int arrSize = sizeof(arr) / sizeof(*arr);
  Node *m_root = NULL;

  for (int i = 0; i < arrSize; ++i) {
    m_root = osp_udp_insert(m_root, arr[i], 1);
  }
  EXPECT_EQ_INT(17, m_root->m_key);
  osp_udp_removeTree(m_root);

  return 0;
}

/* -------------------------------------------------------------------------- */

DEF_TEST(OneSendDataTest) {
  char tmpbuf[NOTIF_MAX_MSG_LEN] = {0};
  char buf[NOTIF_MAX_BUF_LEN] = {0};
  memset(tmpbuf, '\0', NOTIF_MAX_MSG_LEN);
  int r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN, "uid %d dropped %d", 123, 321);
  if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
    ERROR(LOG_KEY "Error while adding udp info to buffer");
    return -1;
  }
  osp_udp_send_data(buf, tmpbuf, r, "udp info: ");
  EXPECT_EQ_INT(30, strlen(buf));
  EXPECT_EQ_INT(0, strlen(tmpbuf));
  return 0;
}

/* -------------------------------------------------------------------------- */

DEF_TEST(SendFullBufferTest) {
  char tmpbuf[NOTIF_MAX_MSG_LEN] = {0};
  char buf[NOTIF_MAX_BUF_LEN] = {0};
  memset(tmpbuf, '\0', NOTIF_MAX_MSG_LEN);
  for (int i = 0; i < 50; i++) {
    int r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN, "uid %d dropped %d", 123, 321);
    if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
      ERROR(LOG_KEY "Error while adding udp info to buffer");
      return -1;
    }
    osp_udp_send_data(buf, tmpbuf, r, "udp info: ");
  }

  EXPECT_EQ_INT(1010, strlen(buf));
  EXPECT_EQ_INT(0, strlen(tmpbuf));
  return 0;
}

/* -------------------------------------------------------------------------- */

DEF_TEST(NotifyTest) {
  char tmpbuf[NOTIF_MAX_MSG_LEN] = {0};
  char buf[NOTIF_MAX_BUF_LEN] = {0};
  memset(tmpbuf, '\0', NOTIF_MAX_MSG_LEN);
  notify = osp_notify_mock;
  for (int i = 0; i < 51; i++) {
    int r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN, "uid %d dropped %d", 123, 321);
    if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
      ERROR(LOG_KEY "Error while adding udp info to buffer");
      return -1;
    }
    osp_udp_send_data(buf, tmpbuf, r, "udp info: ");
  }

  EXPECT_EQ_INT(30, strlen(buf));
  EXPECT_EQ_INT(0, strlen(tmpbuf));
  EXPECT_EQ_INT(1, notify_counter);
  return 0;
}

/* -------------------------------------------------------------------------- */

DEF_TEST(OspUdpCollectdNoDropTest) {
  notify = osp_notify_mock;
  fopen_osp = fopen_osp_mock;
  fclose_osp = fclose_osp_mock;
  Node *m_root = NULL;
  value_t v[1];
  value_list_t vl = VALUE_LIST_INIT;

  vl.values = v;
  vl.values_len = STATIC_ARRAY_SIZE(v);
  sstrncpy(vl.host, hostname_g, sizeof(vl.host));
  sstrncpy(vl.plugin, "osp_udp", sizeof(vl.plugin));
  sstrncpy(vl.type, "packets", sizeof(vl.type));
  vl.time = cdtime();
  osp_udp_collectd(&vl, &m_root, test_data_bufer_without_drops);
  EXPECT_EQ_PTR(NULL, m_root);
  EXPECT_EQ_INT(1, fclose_counter);
  fclose_counter = 0;
  notify_counter = 0;
  return 0;
}

/* -------------------------------------------------------------------------- */
// Uid in test data: 6100, 6014, 3013, 993, 6040, 3210, 6053
DEF_TEST(OspUdpCollectdDropTest) {
  notify = osp_notify_mock;
  fopen_osp = fopen_osp_mock;
  fclose_osp = fclose_osp_mock;
  Node *m_root = NULL;
  Node *result = NULL;
  value_t v[1];
  value_list_t vl = VALUE_LIST_INIT;

  vl.values = v;
  vl.values_len = STATIC_ARRAY_SIZE(v);
  sstrncpy(vl.host, hostname_g, sizeof(vl.host));
  sstrncpy(vl.plugin, "osp_udp", sizeof(vl.plugin));
  sstrncpy(vl.type, "packets", sizeof(vl.type));
  vl.time = cdtime();
  osp_udp_collectd(&vl, &m_root, test_data_bufer_with_drops);
  CHECK_NOT_NULL(m_root);
  EXPECT_EQ_INT(1, fclose_counter);
  result = osp_udp_searchNode(m_root, 6100);
  EXPECT_EQ_INT(5, result->m_val);
  result = osp_udp_searchNode(m_root, 6014);
  EXPECT_EQ_INT(8, result->m_val);
  result = osp_udp_searchNode(m_root, 3013);
  EXPECT_EQ_INT(25, result->m_val);
  result = osp_udp_searchNode(m_root, 993);
  EXPECT_EQ_INT(18, result->m_val);
  result = osp_udp_searchNode(m_root, 6040);
  EXPECT_EQ_INT(63, result->m_val);
  result = osp_udp_searchNode(m_root, 3210);
  EXPECT_EQ_INT(9, result->m_val);
  result = osp_udp_searchNode(m_root, 6053);
  EXPECT_EQ_INT(15, result->m_val);
  osp_udp_removeTree(m_root);

  return 0;
}

/* -------------------------------------------------------------------------- */

int main(void) {
  RUN_TEST(ParseUDPInfoNoDrop);
  RUN_TEST(ParseUDPInfoOneDrop);
  RUN_TEST(TreeTest);
  RUN_TEST(OneSendDataTest);
  RUN_TEST(SendFullBufferTest);
  RUN_TEST(NotifyTest);
  RUN_TEST(OspUdpCollectdNoDropTest);
  RUN_TEST(OspUdpCollectdDropTest);
  END_TEST;
}