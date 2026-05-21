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
 *
 * OS platform plugin.
 * Features:
 *   - collect UFS statistic
 */
#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

#include <linux/bsg.h>
#include <scsi/scsi_bsg_ufs.h>
#include <scsi/sg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

#include <stdarg.h>

static bool samsung_info = true;
static bool micron_info = true;
#define PLUGIN_NAME "osp_ufs"
#define LOG_KEY PLUGIN_NAME " plugin: "
#define NOTIF_MAX_BUF_LEN 1024

const char *UFSBlockDevicePath = "/run/ufs/secondary-ufs";
const char *UFSBSGDevicePath = "/run/ufs/secondary-ufs-bsg";

struct __attribute__((packed)) micronHealthFields3_1 {
  uint16_t wFactoryBBCnt; // Initial(Factory) Bad Block count
  uint16_t wRunTimeBBCnt; // Run time bad blocks count
  uint16_t wSpareBlkCnt;  // Spare blocks count
  uint8_t bReserved[5];
  uint8_t bExhaustedLife4Normal; // Exhausted life for TLC/MLC(Normal)
  uint16_t wMetadataCorruption;  // Meta data corruption
  uint16_t wReserved;
  uint32_t dMinBlkEraseCntTLC; // Minimum virtual block erase count for TLC
  uint32_t dMaxBlkEraseCntTLC; // Maximum virtual block erase count for TLC
  uint32_t dAvgBlkEraseCntTLC; // Average virtual block erase count for TLC
  uint32_t dReserved[5];
  uint32_t dInitCntWithPON;    // Initialization count (with PON)
  uint32_t dInitCntWithoutPON; // Initialization count (w/o PON)
  uint32_t dReserved1;
  uint32_t dReadReclaimCnt4TLC;     // Read reclaim count for TLC
  uint32_t dReadDataSizeIn100MB;    // Read data size [Unit 100MB]
  uint32_t dWrittenDataSizeIn100MB; // Written data size [Unit 100MB]
  uint32_t dSPORwriteFailCnt;       // SPOR write fail count
  uint32_t dSPORCnt;                // SPOR count
  uint32_t dVDETCnt;                // VDET recovery count
  uint32_t dUECCCnt;                // UECC count
  uint32_t dReadRetryCnt;           // Read retry count
  uint32_t dTemperatureInfo;        // Temperature Infomration
  uint16_t wReserved1;
  uint8_t bExhaustLifeEM1; // Exhausted Life for EM1
  uint8_t bReserved1;
  uint32_t dReserved2;
  uint32_t dReadDataSizeEM1In100MB;    // Read data size for EM1 [Unit 100MB]
  uint32_t dWrittenDataSizeEM1In100MB; // Written data size for EM1 [Unit 100MB]
  uint32_t dMinBlkEraseEM1;            // Minimum Block Erase for EM1
  uint32_t dMaxBlkEraseEM1;            // Maximum Block Erase for EM1
  uint32_t dAvgBlkEraseEM1;            // Average Block Erase for EM1
  uint32_t dReadReclaimCntEM1;         // Read Reclaim Count EM1
  uint16_t EAFNormal;
  uint16_t EAFEM1;
  uint32_t DeviceOnTime;
  uint64_t dReserved3;
  uint32_t UICErrorCount;
  uint32_t SERDEDCount;
  uint32_t SECCount;
  uint32_t dReserved4;
  uint32_t UrgentGCCountNormal;
  uint32_t UrgentGCCountEM1;
  uint64_t qReserved;
  uint8_t LUReconfig;
  uint8_t bReserved2[7];
  uint32_t dReserved5[81];
  uint8_t vendorReserved[260];
};

struct __attribute__((packed)) micronHealthDescFields3_1 {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint8_t bPreEOLInfo;
  uint8_t bDeviceLifeTimeEstA;
  uint8_t bDeviceLifeTimeEstB;
  uint8_t bVendorPropInfo[32];
  uint8_t bVendorPropInfoTLcEC;
  uint8_t bRefreshTotalCount[3];
  uint32_t dRefreshProgress;
  uint8_t devHealthDescReserved[211];
};

struct __attribute__((packed)) samsungHealthDescFields3_1 {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint8_t bPreEOLInfo;
  uint8_t bDeviceLifeTimeEstA;
  uint8_t bDeviceLifeTimeEstB;
  uint8_t bDeviceLifeTimeEstAOnePerRes;
  uint8_t bDeviceLifeTimeEstBOnePerRes;
  uint8_t bMinTempCurrentPowerCycle;
  uint8_t bMaxTempCurrentPowerCycle;
  uint8_t bMinTempFrom1stPowerCycle;
  uint8_t bMaxTempFrom1stPowerCycle;
  uint8_t bROMWriteMigrationStatus;
  uint32_t dTotalHostWrittenSLCDataSize;
  uint32_t dTotalHostReadSLCDataSize;
  uint32_t dTotalHostWrittenTLCDataSize;
  uint32_t dTotalHostReadTLCDataSize;
  uint16_t dLvdfDetectCount;
  uint8_t bReserved[7];
  uint32_t dRefreshTotalCount;
  uint32_t dRefreshProgress;
  uint8_t devHealthDescReserved[211];
};

static const uint8_t INQ_CMD_LEN = 6;
static const uint8_t INQ_CMD_CODE = 0x12;
static const uint8_t WB_CMD_CODE = 0x3b;
static const uint8_t RB_CMD_CODE = 0x3c;
static const uint8_t INQ_REPLY_LEN = 36;
static const uint32_t RB_REPLY_LEN = 0x200;

static const int SENSE_BUFFER_SIZE = 32;
static const uint8_t MAX_ATTR_FLAG_SIZE_BYTES = 4;

enum desc_idn {
  QUERY_DESC_IDN_DEVICE = 0x0,
  QUERY_DESC_IDN_CONFIGURATION = 0x1,
  QUERY_DESC_IDN_UNIT = 0x2,
  QUERY_DESC_IDN_RFU_0 = 0x3,
  QUERY_DESC_IDN_INTERCONNECT = 0x4,
  QUERY_DESC_IDN_STRING = 0x5,
  QUERY_DESC_IDN_RFU_1 = 0x6,
  QUERY_DESC_IDN_GEOMETRY = 0x7,
  QUERY_DESC_IDN_POWER = 0x8,
  QUERY_DESC_IDN_HEALTH = 0x9,
  QUERY_DESC_IDN_MAX,
};

enum query_opcode {
  UPIU_QUERY_OPCODE_NOP = 0x0,
  UPIU_QUERY_OPCODE_READ_DESC = 0x1,
  UPIU_QUERY_OPCODE_WRITE_DESC = 0x2,
  UPIU_QUERY_OPCODE_READ_ATTR = 0x3,
  UPIU_QUERY_OPCODE_WRITE_ATTR = 0x4,
  UPIU_QUERY_OPCODE_READ_FLAG = 0x5,
  UPIU_QUERY_OPCODE_SET_FLAG = 0x6,
  UPIU_QUERY_OPCODE_CLEAR_FLAG = 0x7,
  UPIU_QUERY_OPCODE_TOGGLE_FLAG = 0x8,
  UPIU_QUERY_OPCODE_MAX,
};

/* UPIU Query request function */
enum {
  UPIU_QUERY_FUNC_STANDARD_READ_REQUEST = 0x01,
  UPIU_QUERY_FUNC_STANDARD_WRITE_REQUEST = 0x81,
};

/* UTP UPIU Transaction Codes Initiator to Target */
enum {
  UPIU_TRANSACTION_NOP_OUT = 0x00,
  UPIU_TRANSACTION_COMMAND = 0x01,
  UPIU_TRANSACTION_DATA_OUT = 0x02,
  UPIU_TRANSACTION_TASK_REQ = 0x04,
  UPIU_TRANSACTION_QUERY_REQ = 0x16,
};

struct query_err_res {
  char *name;
  __u8 opcode;
};

struct query_err_res query_err_status[] = {{"Success", 0xF0},
                                           {"Reserved1", 0xF1},
                                           {"Reserved2", 0xF2},
                                           {"Reserved3", 0xF3},
                                           {"Reserved4", 0xF4},
                                           {"Reserved5", 0xF5},
                                           {"Parameter not readable", 0xF6},
                                           {"Parameter not written", 0xF7},
                                           {"Parameter already written", 0xF8},
                                           {"Invalid LENGTH", 0xF9},
                                           {"Invalid value", 0xFA},
                                           {"Invalid SELECTOR", 0xFB},
                                           {"Invalid INDEX", 0xFC},
                                           {"Invalid IDN", 0xFD},
                                           {"Invalid OPCODE", 0xFE},
                                           {"General failure", 0xFF}};

#define UPIU_HEADER_DWORD(byte3, byte2, byte1, byte0)                          \
  htobe32(((uint32_t)byte3 << 24) | ((uint32_t)byte2 << 16) |                  \
          ((uint32_t)byte1 << 8) | ((uint32_t)byte0))

// Function pointers - need for tests
static int (*notify)(char *, int);

static const char *eol[5] = {
    "bPreEOLInfo = 00 : Not Defined", "bPreEOLInfo = 01 : Normal",
    "bPreEOLInfo = 02 : Warning (Consumed 80% of Reserved blocks)",
    "bPreEOLInfo = 03 : Critical (Consumed 90% of Reserved blocks)",
    "Reserved"};
static const char *est_desc[13] = {"00h : Information not available",
                                   "01h : 0% - 10% device life time used",
                                   "02h : 10% - 20% device life time used",
                                   "03h : 20% - 30% device life time used",
                                   "04h : 30% - 40% device life time used",
                                   "05h : 40% - 50% device life time used",
                                   "06h : 50% - 60% device life time used",
                                   "07h : 60% - 70% device life time used",
                                   "08h : 70% - 80% device life time used",
                                   "09h : 80% - 90% device life time used",
                                   "0Ah : 90% - 100% device life time used",
                                   "0Bh: Exceeded its device life time",
                                   "Reserved"};
// helper functions
static int8_t memstr(const uint8_t *data, const char *key, size_t size) {
  int8_t result = -1;
  size_t key_length = strlen(key);
  assert(size > key_length && "size should be greater than key length");
  if (size < key_length) {
    return result;
  }

  for (size_t i = 0; i < size - key_length + 1; i++) {
    if (memcmp(&data[i], key, key_length) == 0) {
      result = 0;
      break;
    }
  }
  return result;
}
// helper functions end

// Parser
static void
parseUfsMicronHealthReport(uint8_t *u8InqBuff,
                           struct micronHealthFields3_1 *micronData) {
  memcpy(micronData, u8InqBuff, sizeof(struct micronHealthFields3_1));
  micronData->wFactoryBBCnt = htobe16(micronData->wFactoryBBCnt);
  micronData->wRunTimeBBCnt = htobe16(micronData->wRunTimeBBCnt);
  micronData->wSpareBlkCnt = htobe16(micronData->wSpareBlkCnt);
  micronData->wMetadataCorruption = htobe16(micronData->wMetadataCorruption);
  micronData->dMinBlkEraseCntTLC = htobe32(micronData->dMinBlkEraseCntTLC);
  micronData->dMaxBlkEraseCntTLC = htobe32(micronData->dMaxBlkEraseCntTLC);
  micronData->dAvgBlkEraseCntTLC = htobe32(micronData->dAvgBlkEraseCntTLC);
  micronData->dInitCntWithPON = htobe32(micronData->dInitCntWithPON);
  micronData->dInitCntWithoutPON = htobe32(micronData->dInitCntWithoutPON);
  micronData->dReadReclaimCnt4TLC = htobe32(micronData->dReadReclaimCnt4TLC);
  micronData->dReadDataSizeIn100MB = htobe32(micronData->dReadDataSizeIn100MB);
  micronData->dWrittenDataSizeIn100MB =
      htobe32(micronData->dWrittenDataSizeIn100MB);
  micronData->dSPORwriteFailCnt = htobe32(micronData->dSPORwriteFailCnt);
  micronData->dSPORCnt = htobe32(micronData->dSPORCnt);
  micronData->dVDETCnt = htobe32(micronData->dVDETCnt);
  micronData->dUECCCnt = htobe32(micronData->dUECCCnt);
  micronData->dReadRetryCnt = htobe32(micronData->dReadRetryCnt);
  micronData->dTemperatureInfo = htobe32(micronData->dTemperatureInfo);
  micronData->dReadDataSizeEM1In100MB =
      htobe32(micronData->dReadDataSizeEM1In100MB);
  micronData->dWrittenDataSizeEM1In100MB =
      htobe32(micronData->dWrittenDataSizeEM1In100MB);
  micronData->dMinBlkEraseEM1 = htobe32(micronData->dMinBlkEraseEM1);
  micronData->dMaxBlkEraseEM1 = htobe32(micronData->dMaxBlkEraseEM1);
  micronData->dAvgBlkEraseEM1 = htobe32(micronData->dAvgBlkEraseEM1);
  micronData->dReadReclaimCntEM1 = htobe32(micronData->dReadReclaimCntEM1);
  micronData->EAFNormal = htobe16(micronData->EAFNormal);
  micronData->EAFEM1 = htobe16(micronData->EAFEM1);
  micronData->DeviceOnTime = htobe32(micronData->DeviceOnTime);
  micronData->UICErrorCount = htobe32(micronData->UICErrorCount);
  micronData->SERDEDCount = htobe32(micronData->SERDEDCount);
  micronData->SECCount = htobe32(micronData->SECCount);
  micronData->UrgentGCCountNormal = htobe32(micronData->UrgentGCCountNormal);
  micronData->UrgentGCCountEM1 = htobe32(micronData->UrgentGCCountEM1);
}

static void
parseUfsMicronHealthDescr(uint8_t *u8InqBuff,
                          struct micronHealthDescFields3_1 *micronData) {
  memcpy(micronData, u8InqBuff, sizeof(struct micronHealthDescFields3_1));
  micronData->dRefreshProgress = htobe32(micronData->dRefreshProgress);
}

static void
parseUfsSamsungHealthDescr(uint8_t *u8InqBuff,
                           struct samsungHealthDescFields3_1 *samsungData) {
  memcpy(samsungData, u8InqBuff, sizeof(struct samsungHealthDescFields3_1));
  samsungData->dTotalHostWrittenSLCDataSize =
      htobe32(samsungData->dTotalHostWrittenSLCDataSize);
  samsungData->dTotalHostReadSLCDataSize =
      htobe32(samsungData->dTotalHostReadSLCDataSize);
  samsungData->dTotalHostWrittenTLCDataSize =
      htobe32(samsungData->dTotalHostWrittenTLCDataSize);
  samsungData->dTotalHostReadTLCDataSize =
      htobe32(samsungData->dTotalHostReadTLCDataSize);
  samsungData->dRefreshTotalCount = htobe32(samsungData->dRefreshTotalCount);
  samsungData->dRefreshProgress = htobe32(samsungData->dRefreshProgress);
}
//------Parser end

// UFS
static void print_error(const char *msg, ...) {
  va_list args;

  fprintf(stderr, "\nErr: ");
  va_start(args, msg);
  vfprintf(stderr, msg, args);
  va_end(args);
  fprintf(stderr, "\n");
}

static void query_response_error(__u8 opcode, __u8 idn) {
  __u8 query_response_inx = opcode & 0x0F;

  print_error("%s, for idn 0x%02x", query_err_status[query_response_inx].name,
              idn);
}

static int openUFSDevice(const char *filePath, int openMode) {
  int fd;

  fd = open(filePath, openMode);
  if (fd < 0) {
    ERROR(LOG_KEY "Failed to open the UFS device: %s error: %s", filePath,
          STRERRNO);
  }
  return fd;
}

static int32_t send_scsi_cmd_upiu(uint8_t *cdb, uint8_t cdb_len,
                                  uint8_t *data_ptr, int32_t data_len,
                                  int8_t direction)

{
  struct sg_io_hdr io_hdr;
  int32_t status = 0;
  uint8_t sense_buffer[SENSE_BUFFER_SIZE];
  int defTimeout = 10000;

  int fd_sd = openUFSDevice(UFSBlockDevicePath, O_RDONLY | O_SYNC);

  memset(sense_buffer, 0, sizeof(sense_buffer));

  memset(&io_hdr, 0, sizeof(struct sg_io_hdr));

  io_hdr.interface_id = 'S';
  io_hdr.cmd_len = cdb_len;
  io_hdr.mx_sb_len = sizeof(sense_buffer);
  io_hdr.dxfer_direction =
      (direction == 0) ? SG_DXFER_FROM_DEV : SG_DXFER_TO_DEV;

  if (data_len && data_ptr != NULL) {
    io_hdr.dxfer_len = data_len;
    io_hdr.dxferp = data_ptr;
  }

  if (cdb != NULL) {
    io_hdr.cmdp = cdb;
  }

  io_hdr.sbp = sense_buffer;
  io_hdr.timeout = defTimeout;

  if (fd_sd >= 0) {
    status = ioctl(fd_sd, SG_IO, &io_hdr);
  } else {
    status = -1;
  }

  if (status != 0) {
    ERROR(LOG_KEY "SCSI inquiry failed! Error: %d", status);
  }

  if (fd_sd >= 0) {
    close(fd_sd);
  }
  return status;
}

static uint8_t readUFSProductInfo(uint8_t *arr, uint16_t len) {
  uint8_t u8InqQueryCmdBlk[] = {INQ_CMD_CODE, 0, 0, 0, INQ_REPLY_LEN, 0};
  uint8_t u8InqBuff[INQ_REPLY_LEN];
  int status;
  status = send_scsi_cmd_upiu(u8InqQueryCmdBlk, sizeof(u8InqQueryCmdBlk),
                              u8InqBuff, sizeof(u8InqBuff), 0);
  if (status == 0) {
    memcpy(arr, u8InqBuff, INQ_REPLY_LEN);
    return INQ_REPLY_LEN;
  }
  return 0;
}

static void put_unaligned_be24(__u32 val, void *p) {
  ((__u8 *)p)[0] = (val >> 16) & 0xff;
  ((__u8 *)p)[1] = (val >> 8) & 0xff;
  ((__u8 *)p)[2] = val & 0xff;
}

static uint32_t readUFSHealthReportMicron(uint8_t *arr, uint16_t len) {
  uint8_t u8InqQueryCmdWrite[10];
  uint8_t u8InqQueryCmdRead[10];
  uint8_t u8InqBuff[RB_REPLY_LEN];
  memset(u8InqQueryCmdWrite, 0, 10);
  memset(u8InqQueryCmdRead, 0, 10);
  memset(u8InqBuff, 0, RB_REPLY_LEN);
  u8InqQueryCmdWrite[0] = WB_CMD_CODE;
  u8InqQueryCmdWrite[1] = 0xe1;
  u8InqQueryCmdWrite[2] = '\0';
  put_unaligned_be24(0x2c, u8InqQueryCmdWrite + 6);
  u8InqBuff[0] = 0xfe;
  u8InqBuff[1] = '@';
  u8InqBuff[2] = '\0';
  u8InqBuff[3] = '\x10';
  u8InqBuff[4] = '\x01';

  int32_t status =
      send_scsi_cmd_upiu(u8InqQueryCmdWrite, 10, u8InqBuff, 0x2c, 1);
  if (status == 0) {
    memset(u8InqBuff, 0, RB_REPLY_LEN);
    u8InqQueryCmdRead[0] = RB_CMD_CODE;
    u8InqQueryCmdRead[1] = 0xc1;
    u8InqQueryCmdRead[2] = '\0';

    put_unaligned_be24(0x200, u8InqQueryCmdRead + 6);
    status = send_scsi_cmd_upiu(u8InqQueryCmdRead, 10, u8InqBuff, 0x200, 0);
    if (status == 0) {
      if (len < RB_REPLY_LEN) {
        memcpy(arr, u8InqBuff, len);
      } else {
        memcpy(arr, u8InqBuff, RB_REPLY_LEN);
      }
      return len;
    }
  }
  return 0;
}

static int send_bsg_scsi_trs(int fd, struct ufs_bsg_request *request_buff,
                             struct ufs_bsg_reply *reply_buff,
                             uint32_t req_buf_len, uint32_t reply_buf_len,
                             uint8_t *data_buf) {
  int ret;
  struct sg_io_v4 io_hdr_v4 = {0};

  io_hdr_v4.guard = 'Q';
  io_hdr_v4.protocol = BSG_PROTOCOL_SCSI;
  io_hdr_v4.subprotocol = BSG_SUB_PROTOCOL_SCSI_TRANSPORT;
  io_hdr_v4.response = (__u64)reply_buff;
  io_hdr_v4.max_response_len = sizeof(struct ufs_bsg_reply);
  io_hdr_v4.request_len = sizeof(struct ufs_bsg_request);
  io_hdr_v4.request = (__u64)request_buff;

  if (req_buf_len > 0) {
    /* write descriptor */
    io_hdr_v4.dout_xferp = (__u64)(data_buf);
    io_hdr_v4.dout_xfer_len = req_buf_len;
  } else if (reply_buf_len > 0) {
    /* read descriptor */
    io_hdr_v4.din_xferp = (__u64)(data_buf);
    io_hdr_v4.din_xfer_len = reply_buf_len;
  }

  while (((ret = ioctl(fd, SG_IO, &io_hdr_v4)) < 0) &&
         ((errno == EINTR) || (errno == EAGAIN)))
    ;

  if (io_hdr_v4.info != 0) {
    ERROR(LOG_KEY "send_bsg_scsi_trs: IOCTL Command failed with status:%d",
          io_hdr_v4.info);
    ret = -1;
  }
  INFO(LOG_KEY "Query Response Length: %d", reply_buff->reply_payload_rcv_len);
  return ret;
}

// prepare_upiu function formulates the payload for the BSG query
static void prepare_upiu(struct ufs_bsg_request *bsg_req,
                         uint8_t query_req_func, uint16_t data_len,
                         uint8_t opcode, uint8_t idn, uint8_t index,
                         uint8_t sel) {
  bsg_req->msgcode = UPIU_TRANSACTION_QUERY_REQ;

  bsg_req->upiu_req.header.dword_0 =
      UPIU_HEADER_DWORD(UPIU_TRANSACTION_QUERY_REQ, 0, 0, 0);
  bsg_req->upiu_req.header.dword_1 = UPIU_HEADER_DWORD(0, query_req_func, 0, 0);
  bsg_req->upiu_req.header.dword_2 =
      UPIU_HEADER_DWORD(0, 0, data_len >> 8, (uint8_t)data_len);

  bsg_req->upiu_req.qr.opcode = opcode;
  bsg_req->upiu_req.qr.idn = idn;
  bsg_req->upiu_req.qr.index = index;
  bsg_req->upiu_req.qr.selector = sel;
  bsg_req->upiu_req.qr.length = htobe16(data_len);
}

static int bsg_query(int fd, struct ufs_bsg_request *bsg_req,
                     struct ufs_bsg_reply *bsg_rsp, uint8_t query_req_func,
                     uint8_t opcode, uint8_t idn, uint8_t index, uint8_t sel,
                     uint8_t *data_buf, uint32_t req_buf_len,
                     uint32_t res_buf_len) {
  int status = 0;
  uint8_t res_code = 0;
  uint8_t shift_in_bits = 0;
  uint16_t len = res_buf_len;
  uint32_t value = 0;

  if (req_buf_len > 0)
    len = req_buf_len;

  if ((bsg_req != NULL) && (bsg_rsp != NULL)) {

    prepare_upiu(bsg_req, query_req_func, len, opcode, idn, index, sel);

    status = send_bsg_scsi_trs(fd, bsg_req, bsg_rsp, req_buf_len, res_buf_len,
                               data_buf);
    if (status) {
      print_error("%s: query failed, status %d idn: %d, i: %d, s: %d", __func__,
                  status, idn, index, sel);
      return -1;
    }
    res_code = (be32toh(bsg_rsp->upiu_rsp.header.dword_1) >> 8) & 0xff;
    if (res_code) {
      query_response_error(res_code, idn);
      return -1;
    }
    if ((opcode == UPIU_QUERY_OPCODE_READ_ATTR) ||
        (opcode == UPIU_QUERY_OPCODE_READ_FLAG)) {
      value = bsg_rsp->upiu_rsp.qr.value;

      for (int i = 0; i < MAX_ATTR_FLAG_SIZE_BYTES; i++) {
        shift_in_bits = 8 * (MAX_ATTR_FLAG_SIZE_BYTES - i - 1);
        data_buf[i] = (value >> shift_in_bits) & 0xFF;
      }
    }

    return status;
  }
  return status;
}

static int ufsQuery(int fd, uint8_t opcode, uint8_t idn, uint8_t index,
                    uint8_t selector, uint8_t *desc, uint32_t *dlen,
                    uint32_t *data) {

  struct ufs_bsg_request bsg_req = {0};
  struct ufs_bsg_reply bsg_rsp = {0};
  uint32_t res_buf_len = 0;
  uint32_t req_buf_len = 0;
  uint8_t query_req_func = 0;
  int status = 0;

  if (opcode == UPIU_QUERY_OPCODE_READ_DESC) {
    query_req_func = UPIU_QUERY_FUNC_STANDARD_READ_REQUEST;
    res_buf_len = *dlen;
    req_buf_len = 0;
  } else if ((opcode == UPIU_QUERY_OPCODE_READ_ATTR) ||
             (opcode == UPIU_QUERY_OPCODE_READ_FLAG)) {
    query_req_func = UPIU_QUERY_FUNC_STANDARD_READ_REQUEST;
    res_buf_len = 0;
    req_buf_len = 0;
  }

  status = bsg_query(fd, &bsg_req, &bsg_rsp, query_req_func, opcode, idn, index,
                     selector, desc, req_buf_len, res_buf_len);

  return status;
}

static int ufsReadDesc(uint8_t idn, int idx, uint8_t *dataBuf, uint32_t size) {
  int status = 0;
  int fd_bsg = openUFSDevice(UFSBSGDevicePath, O_RDWR | O_SYNC);
  if (fd_bsg >= 0) {
    status = ufsQuery(fd_bsg, UPIU_QUERY_OPCODE_READ_DESC, idn, idx, 0, dataBuf,
                      &size, NULL);
  } else {
    status = -1;
  }
  if (fd_bsg >= 0) {
    close(fd_bsg);
  }
  return status;
}

static int readUFSHealthInfo(uint8_t *arr, uint16_t len) {
  int status = 0;
  uint32_t desc_len = 255;
  uint8_t desc[255] = {0};
  status = ufsReadDesc(QUERY_DESC_IDN_HEALTH, 0, desc, desc_len);
  if (status == 0) {
    memcpy(arr, desc, len);
  }

  return status;
}
// UFS end

/**************************************************************
 *
 * Function osp_ufs_send_data accumulate buffer for send in the DLT.
 * The max length of DLT message is 1024 char. If the data size exceeds 1024
 * characters, the data is distributed across multiple messages.
 * buf - buffer  wich will be send
 * tmpbuf - data which will be passed to the buffer
 * r - size of data
 * info - Shows what the data refers to
 * Example:
 * ufs info: eolInfo:bPreEOLInfo = 01 : Normal;
 * info => "ufs info:"
 * tmpbuf => "eolInfo:bPreEOLInfo = 01 : Normal"
 *
 **************************************************************/
static void osp_ufs_send_data(char *buf, char *tmpbuf, int r, char *info) {
  int len_buf = strlen(buf);
  if (len_buf == 0) {
    if ((r + strlen(info)) < NOTIF_MAX_BUF_LEN) {
      snprintf(buf, NOTIF_MAX_BUF_LEN, "%s%s", info, tmpbuf);
      strncat(buf, ";", 2);
      memset(tmpbuf, '\0', NOTIF_MAX_MSG_LEN);
    }
    // check if enough space in the buffer for data and ";\0"
  } else if ((1022 - len_buf) > r) {
    strncat(buf, tmpbuf, r);
    strncat(buf, ";", 2);
    memset(tmpbuf, '\0', NOTIF_MAX_MSG_LEN);
  } else {
    assert(notify != NULL && "notify does not set");
    notify(buf, strlen(buf));
    memset(buf, '\0', NOTIF_MAX_BUF_LEN);
    if (strlen(tmpbuf) > 0) {
      if ((r + strlen(info)) < NOTIF_MAX_BUF_LEN) {
        snprintf(buf, NOTIF_MAX_BUF_LEN, "%s%s", info, tmpbuf);
      }
      strncat(buf, ";", 2);
      memset(tmpbuf, '\0', NOTIF_MAX_MSG_LEN);
    }
  }
}

static int osp_ufs_statistic() {
  uint8_t u8InqBuff[36] = {0};
  uint8_t uHealthReportBuf[512] = {0};
  uint8_t uDescrHealthReportBuf[45] = {0};
  int res = 0;
  char buf[NOTIF_MAX_BUF_LEN] = {0};
  char tmpbuf[NOTIF_MAX_MSG_LEN] = {0};

  res = readUFSProductInfo(u8InqBuff, 36);
  if (res > 0) {
    res = memstr(u8InqBuff, "SAMSUNG", sizeof(u8InqBuff));
    if (res == 0 && samsung_info) {
      res = readUFSHealthInfo(uDescrHealthReportBuf, 45);
      if (res == 0) {
        struct samsungHealthDescFields3_1 samsungData;
        parseUfsSamsungHealthDescr(uDescrHealthReportBuf, &samsungData);
        int r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN, "eolInfo:%s",
                         eol[samsungData.bPreEOLInfo]);
        if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
          ERROR(LOG_KEY "Error while adding Samsung info to buffer");
          return -1;
        }
        osp_ufs_send_data(buf, tmpbuf, r, "ufs info: ");

        r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN,
                     "LifetimeA:bDeviceLifeTimeEstA = %s",
                     est_desc[samsungData.bDeviceLifeTimeEstA]);
        if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
          ERROR(LOG_KEY "Error while adding Samsung info to buffer");
          return -1;
        }
        osp_ufs_send_data(buf, tmpbuf, r, "ufs info: ");

        r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN,
                     "LifetimeB:bDeviceLifeTimeEstB = %s",
                     est_desc[samsungData.bDeviceLifeTimeEstB]);
        if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
          ERROR(LOG_KEY "Error while adding Samsung info to buffer");
          return -1;
        }
        osp_ufs_send_data(buf, tmpbuf, r, "ufs info: ");
        if (strlen(buf) > 0) {
          notify(buf, strlen(buf));
        }
      } else {
        ERROR(LOG_KEY "Error during receiving samsung health info %s",
              STRERRNO);
      }
    }
    res = memstr(u8InqBuff, "MICRON", sizeof(u8InqBuff));
    if (res == 0 && micron_info) {
      struct micronHealthDescFields3_1 micronHealthDescr;
      res = readUFSHealthInfo(uDescrHealthReportBuf, 45);
      if (res == 0) {
        parseUfsMicronHealthDescr(uDescrHealthReportBuf, &micronHealthDescr);
        int r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN, "eolInfo:%s",
                         eol[micronHealthDescr.bPreEOLInfo]);
        if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
          ERROR(LOG_KEY "Error while adding Micron info to buffer");
          return -1;
        }
        osp_ufs_send_data(buf, tmpbuf, r, "ufs info: ");

        r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN,
                     "LifetimeA:bDeviceLifeTimeEstA = %s",
                     est_desc[micronHealthDescr.bDeviceLifeTimeEstA]);
        if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
          ERROR(LOG_KEY "Error while adding Micron info to buffer");
          return -1;
        }
        osp_ufs_send_data(buf, tmpbuf, r, "ufs info: ");

        r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN,
                     "LifetimeB:bDeviceLifeTimeEstB = %s",
                     est_desc[micronHealthDescr.bDeviceLifeTimeEstB]);
        if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
          ERROR(LOG_KEY "Error while adding Micron info to buffer");
          return -1;
        }
        osp_ufs_send_data(buf, tmpbuf, r, "ufs info: ");
      } else {
        ERROR(LOG_KEY "Error during receiving micron health info %s", STRERRNO);
      }

      res = readUFSHealthReportMicron(uHealthReportBuf, 512);
      if (res > 0) {
        struct micronHealthFields3_1 micronData;
        parseUfsMicronHealthReport(uHealthReportBuf, &micronData);
        int r =
            snprintf(tmpbuf, NOTIF_MAX_MSG_LEN, "UECC:%d", micronData.dUECCCnt);
        if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
          ERROR(LOG_KEY "Error while adding Micron info to buffer");
          return -1;
        }
        osp_ufs_send_data(buf, tmpbuf, r, "ufs info: ");

        r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN, "UICError:%d",
                     micronData.UICErrorCount);
        if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
          ERROR(LOG_KEY "Error while adding Micron info to buffer");
          return -1;
        }
        osp_ufs_send_data(buf, tmpbuf, r, "ufs info: ");

        r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN, "SERDED:%d",
                     micronData.SERDEDCount);
        if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
          ERROR(LOG_KEY "Error while adding Micron info to buffer");
          return -1;
        }
        osp_ufs_send_data(buf, tmpbuf, r, "ufs info: ");

        r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN, "RunTimeBBCnt:%d",
                     micronData.wRunTimeBBCnt);
        if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
          ERROR(LOG_KEY "Error while adding Micron info to buffer");
          return -1;
        }
        osp_ufs_send_data(buf, tmpbuf, r, "ufs info: ");

        r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN, "MetadataCorruption:%d",
                     micronData.wMetadataCorruption);
        if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
          ERROR(LOG_KEY "Error while adding Micron info to buffer");
          return -1;
        }
        osp_ufs_send_data(buf, tmpbuf, r, "ufs info: ");

        r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN, "ReadRetryCnt:%d",
                     micronData.dReadRetryCnt);
        if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
          ERROR(LOG_KEY "Error while adding Micron info to buffer");
          return -1;
        }
        osp_ufs_send_data(buf, tmpbuf, r, "ufs info: ");

        r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN, "SPORwriteFailCnt:%d",
                     micronData.dSPORwriteFailCnt);
        if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
          ERROR(LOG_KEY "Error while adding Micron info to buffer");
          return -1;
        }
        osp_ufs_send_data(buf, tmpbuf, r, "ufs info: ");

        r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN, "SPORCnt:%d",
                     micronData.dSPORCnt);
        if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
          ERROR(LOG_KEY "Error while adding Micron info to buffer");
          return -1;
        }
        osp_ufs_send_data(buf, tmpbuf, r, "ufs info: ");

        r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN, "InitCntWithPON:%d",
                     micronData.dInitCntWithPON);
        if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
          ERROR(LOG_KEY "Error while adding Micron info to buffer");
          return -1;
        }
        osp_ufs_send_data(buf, tmpbuf, r, "ufs info: ");

        r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN, "InitCntWithoutPON:%d",
                     micronData.dInitCntWithoutPON);
        if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
          ERROR(LOG_KEY "Error while adding Micron info to buffer");
          return -1;
        }
        osp_ufs_send_data(buf, tmpbuf, r, "ufs info: ");

        if (strlen(buf) > 0) {
          notify(buf, strlen(buf));
        }
      } else {
        ERROR(LOG_KEY "Error during receiving micron health report %s",
              STRERRNO);
      }
    }
  } else {
    ERROR(LOG_KEY "Error during receiving vender info %s", STRERRNO);
  }

  return 0;
}

//------------------------------------------------------------------------------
static int osp_ufs_config(oconfig_item_t *ci) /* {{{ */
{
  INFO(LOG_KEY "configuration");
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;
    if (strcasecmp("SamsungInfo", child->key) == 0)
      cf_util_get_boolean(child, &samsung_info);
    else if (strcasecmp("MicronInfo", child->key) == 0)
      cf_util_get_boolean(child, &micron_info);
    else
      ERROR("osp plugin: Invalid configuration option: "
            "\"%s\".",
            child->key);
  }
  return 0;
}

static int osp_ufs_init(void) { return 0; }

//------------------------------------------------------------------------------
static int osp_ufs_notify(char *buf, int buf_len) {
  notification_t n = {NOTIF_OKAY, cdtime(),   "", "",  PLUGIN_NAME,
                      "",         "ufs-info", "", NULL};

  if (plugin_notification_meta_add_string(&n, "", buf) != 0) {
    ERROR(LOG_KEY "Error adding meta information to notification.");
    return -1;
  }

  plugin_dispatch_notification(&n);
  if (n.meta != NULL)
    plugin_notification_meta_free(n.meta);

  return 0;
}

//------------------------------------------------------------------------------
static int osp_ufs_read(void) {
  notify = osp_ufs_notify;
  osp_ufs_statistic();

  return 0;
}

//------------------------------------------------------------------------------
void module_register(void) {
  plugin_register_complex_config("osp_ufs", osp_ufs_config);
  plugin_register_init("osp_ufs", osp_ufs_init);
  plugin_register_read("osp_ufs", osp_ufs_read);
} /* void module_register */