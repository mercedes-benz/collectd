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
 *   Mircea Cernat <mircea dot cernat at mercedes-benz.com>
 */

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

#include <netinet/tcp_var.h>
#include <netinet/udp_var.h>
#include <net/if_arp.h>
#include <sys/sysctl.h>
#include <kvm.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>



/* ************************************************************************* */
/* Private Macros */
/* ************************************************************************* */

/* Name of this plugin */
#define PLUGIN_NAME              "netstat_qnx"

/* Maximum length for a formatted statistics */
#define NETSTAT_NOTIF_STAT_SIZE  (256u)

/* UDP stats constants */
#define NETSTAT_UDP_STATS_COUNT  (sizeof(struct udpstat)/sizeof(uint64_t))

/* TCP stats constants */
#define NETSTAT_TCP_STATS_COUNT  (sizeof(struct tcpstat)/sizeof(uint64_t))

/* ARP stats constants */
#define NETSTAT_ARP_STATS_COUNT  (sizeof(struct arpstat)/sizeof(uint64_t))


/* ************************************************************************* */
/* Private Types */
/* ************************************************************************* */

/* Type for TCP statistics, name of statistic and counter. */
typedef struct netstat_tcp_stats
{
  const char* name[NETSTAT_TCP_STATS_COUNT];      /* Name of the stat. */
  uint64_t    counters[NETSTAT_TCP_STATS_COUNT];  /* Value (count of packets). */
} netstat_tcp_stats_t;

/* Type for UDP statistics, name of statistic and counter. */
typedef struct netstat_udp_stats
{
  const char* name[NETSTAT_UDP_STATS_COUNT];      /* Name of the stat. */
  uint64_t    counters[NETSTAT_UDP_STATS_COUNT];  /* Value (count of packets). */
} netstat_udp_stats_t;

/* Type for ARP statistics, name of statistic and counter. */
typedef struct netstat_arp_stats
{
  const char* name[NETSTAT_ARP_STATS_COUNT];      /* Name of the stat. */
  uint64_t    counters[NETSTAT_ARP_STATS_COUNT];  /* Value (count of packets). */
} netstat_arp_stats_t;


/* ************************************************************************* */
/* Private Variables */
/* ************************************************************************* */

/* ARP statistics - see struct arpstat@if_arp.h*/
static netstat_arp_stats_t netstat_arp_stats = {
  {
   "sndtotal",    "sndreply",    "sndrequest",  "rcvtotal",
   "rcvrequest",  "rcvreply",    "rcvmcast",    "rcvbadproto",
   "rcvbadlen",   "rcvzerotpa",  "rcvzerospa",  "rcvnoint",
   "rcvlocalsha", "rcvbcastsha", "rcvlocalspa", "rcvoverperm",
   "rcvoverint",  "rcvover",     "rcvlenchg",   "dfrtotal",
   "dfrsent",     "dfrdropped",  "allocfail", 
  },
  { 0U }
};

/* UDP statistics - see struct udpstat@udp_var.h */
static netstat_udp_stats_t netstat_udp_stats = {
  {
   "ipackets",     "hdrops",      "badsum",      "badlen",
   "noport",       "noportbcast", "fullsock",    "pcbhashmiss",
   "opackets"
  },
  { 0U }
};

/* TCP statistics - see struct tcpstat@tcp_var.h*/
static netstat_tcp_stats_t netstat_tcp_stats = {
  {
   "connattempt",      "accepts",         "connects",        "drops", 
   "conndrops",        "closed",          "segstimed",       "rttupdated", 
   "delack",           "timeoutdrop",     "rexmttimeo",      "persisttimeo", 
   "keeptimeo",        "keepprobe",       "keepdrops",       "persistdrops", 
   "connsdrained",     "pmtublackhole",   "sndtotal",        "sndpack", 
   "sndbyte",          "sndrexmitpack",   "sndrexmitbyte",   "sndacks", 
   "sndprobe",         "sndurg",          "sndwinup",        "sndctrl", 
   "rcvtotal",         "rcvpack",         "rcvbyte",         "rcvbadsum", 
   "rcvbadoff",        "rcvmemdrop",      "rcvshort",        "rcvduppack", 
   "rcvdupbyte",       "rcvpartduppack",  "rcvpartdupbyte",  "rcvoopack", 
   "rcvoobyte",        "rcvpackafterwin", "rcvbyteafterwin", "rcvafterclose", 
   "rcvwinprobe",      "rcvdupack",       "rcvacktoomuch",   "rcvackpack", 
   "rcvackbyte",       "rcvwinupd",       "pawsdrop",        "predack", 
   "preddat",          "pcbhashmiss",     "noport",          "badsyn", 
   "delayed_free",     "sc_added",        "sc_completed",    "sc_timed_out", 
   "sc_overflowed",    "sc_reset",        "sc_unreach",      "sc_bucketoverflow", 
   "sc_aborted",       "sc_dupesyn",      "sc_dropped",      "sc_collisions", 
   "sc_retransmitted", "sc_delayed_free", "selfquench",      "badsig", 
   "goodsig",          "ecn_shs",         "ecn_ce",          "ecn_ect", 
   "sc_resp_lim"
  },
  { 0U }
};

/* KVM handle to read arp stats */
static kvm_t* kvmd = NULL;


/* ************************************************************************* */
/* Private Function Declarations */
/* ************************************************************************* */

/* Notify the selected statistics */
static void netstat_notify_statistics(const char*     which,
                                      const char**    names,
                                      const uint64_t* values,
                                      const uint16_t  count);

/* Read the UDP statistics and call notify */
static void netstat_qnx_read_udp_stats(void);

/* Read the TCP statistics and call notify */
static void netstat_qnx_read_tcp_stats(void);

/* Read the ARP statistics and call notify */
static void netstat_qnx_read_arp_stats(void);


/* ************************************************************************* */
/* Private Function Definitions */
/* ************************************************************************* */

/* ------------------------------------------------------------------------- */
static void netstat_notify_statistics
(
  const char*     which,
  const char**    names,
  const uint64_t* values,
  const uint16_t  count
)
{
  char           stat_buffer[NETSTAT_NOTIF_STAT_SIZE];
  cdtime_t       time_now = cdtime();
  notification_t notif    = {
    NOTIF_OKAY, time_now, "", "", PLUGIN_NAME, "", "", "", NULL
  };

  int ret = snprintf(notif.message, sizeof(notif.message), 
                     "%s statistics", which);

  if (ret < 0) { 
    ERROR("snprintf() failed.");
    return;
  }

  for (uint16_t stat_idx = 0U; stat_idx < count; stat_idx ++) { 
    ret = snprintf(stat_buffer, sizeof(stat_buffer), "%s = %lu",
                   names[stat_idx], values[stat_idx]);
    if (ret < 0) {
      ERROR("snprintf() failed while formatting stat: %s, %lu", 
            names[stat_idx], values[stat_idx]);
      continue;
    }

    ret = plugin_notification_meta_add_string(&notif, "", stat_buffer);
    if (ret != 0) {
      ERROR("plugin_notification_meta_add_string() failed for stat: %s, %lu",
            names[stat_idx], values[stat_idx]);
    }
  }

  plugin_dispatch_notification(&notif);
  if (notif.meta != NULL)
    plugin_notification_meta_free(notif.meta);
}

/* ------------------------------------------------------------------------- */
static void netstat_qnx_read_udp_stats(void)
{
  size_t    len = sizeof(netstat_udp_stats.counters);
  const int ret = sysctlbyname("net.inet.udp.stats", netstat_udp_stats.counters,
                               &len, NULL, 0);

  if (ret != 0) {
    ERROR("sysctlbynname(\"net.inet.udp.stats\") failed, errno: %d", errno);
  } else {
    netstat_notify_statistics("UDP", netstat_udp_stats.name, 
                              netstat_udp_stats.counters,
                              NETSTAT_UDP_STATS_COUNT);
  }
}

/* ------------------------------------------------------------------------- */
static void netstat_qnx_read_tcp_stats(void)
{
  size_t    len = sizeof(netstat_tcp_stats.counters);
  const int ret = sysctlbyname("net.inet.tcp.stats", netstat_tcp_stats.counters,
                               &len, NULL, 0);

  if (ret != 0) {
    ERROR("sysctlbynname(\"net.inet.tcp.stats\") failed, errno: %d", errno);
  } else {
    netstat_notify_statistics("TCP", netstat_tcp_stats.name, 
                              netstat_tcp_stats.counters,
                              NETSTAT_TCP_STATS_COUNT);
  }
}

/* ------------------------------------------------------------------------- */
static void netstat_qnx_read_arp_stats(void)
{
  static struct nlist kvm_nl[] = {
    {"_arpstat", 0, 0, 0},
    {NULL,       0, 0, 0}
  };

  if (kvmd == NULL) { 
    ERROR("kvmd == NULL");
    return;
  }

  if (kvm_nlist(kvmd, kvm_nl) != 0) {
    ERROR("kvm_nlist() failed.\n");
    return;
  }

  const int ret = kvm_read(kvmd, kvm_nl[0].n_value, 
                           netstat_arp_stats.counters,
                           sizeof(netstat_arp_stats.counters));
  
  if (ret == -1) {
    ERROR("kvm_read() failed.\n");
    return;
  }

  netstat_notify_statistics("ARP", netstat_arp_stats.name, 
                            netstat_arp_stats.counters,
                            NETSTAT_ARP_STATS_COUNT);
} 

/* ------------------------------------------------------------------------- */
static int netstat_qnx_init(void)
{
  static char error_message[] = "netstat_qnx";
  if (kvmd == NULL) {
    kvmd = kvm_openfiles(NULL, NULL, NULL, O_RDONLY, error_message);
    if (kvmd == NULL) {
      ERROR("kvm_openfiles() returned NULL.");
      return -1;
    }
  }

  return 0;
}

/* ------------------------------------------------------------------------- */
static int netstat_qnx_shutdown(void)
{
  if (kvmd != NULL) {
    kvm_close(kvmd);
    kvmd = NULL;
  }

  return 0;
}

/* ------------------------------------------------------------------------- */
static int netstat_qnx_read(void)
{
  netstat_qnx_read_udp_stats();
  netstat_qnx_read_tcp_stats();
  netstat_qnx_read_arp_stats();

  return 0;
}

/* ------------------------------------------------------------------------- */
void module_register(void) {
  plugin_register_init(PLUGIN_NAME, netstat_qnx_init);
  plugin_register_shutdown(PLUGIN_NAME, netstat_qnx_shutdown);
  plugin_register_read(PLUGIN_NAME, netstat_qnx_read);
} /* void module_register */

