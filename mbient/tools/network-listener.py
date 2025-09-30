#!/usr/bin/env python3
# Copyright (C) 2025 Frank Bielig

# Listen to output of network plugin
#
# https://github-wiki-see.page/m/collectd/collectd/wiki/Binary-protocol

import socket
import struct
from datetime import datetime

# Value data source types
DS_TYPE_COUNTER = 0
DS_TYPE_GAUGE = 1
DS_TYPE_DERIVE = 2
DS_TYPE_ABSOLUTE = 3

TYPE_NAMES = {
  DS_TYPE_COUNTER: 'counter',
  DS_TYPE_GAUGE: 'gauge',
  DS_TYPE_DERIVE: 'derive',
  DS_TYPE_ABSOLUTE: 'absolute',
}

# WARN and LOG levels
LOG_ERR = 3
LOG_WARNING = 4
LOG_NOTICE = 5
LOG_INFO = 6
LOG_DEBUG = 7

NOTIF_FAILURE = 1
NOTIF_WARNING = 2
NOTIF_OKAY = 4

SEVERITY_NAMES = {
  NOTIF_FAILURE: "FAILURE",
  NOTIF_WARNING: "WARNING",
  NOTIF_OKAY: "OK",
  LOG_ERR: "ERROR",
  LOG_NOTICE: "NOTICE",
  LOG_INFO: "INFO",
  LOG_DEBUG: "DEBUG",
}


def parse_collectd_packet(parts_data):

    offset = 0
    context = {}

    while offset < len(parts_data):
        if offset + 4 > len(parts_data):
            break

        part_type, part_length = struct.unpack('!HH', parts_data[offset:offset+4])
        offset += 4
        part_value = parts_data[offset:offset+(part_length-4)]
        offset += part_length - 4

        # Handle different part types
        try:
            if part_type == 0x0000:   # Hostname
                context['host'] = part_value.split(b'\x00')[0].decode('utf-8')
            elif part_type == 0x0001: # Time
                ns = struct.unpack('>Q', part_value)[0]
                context['time'] = datetime.fromtimestamp(ns / 1e9).strftime('%Y-%m-%d %H:%M:%S')
            elif part_type == 0x0008: # Time (high resolution)
                t = struct.unpack('>Q', part_value)[0] / pow(2, 30)
                context['time'] = datetime.fromtimestamp(t).strftime('%Y-%m-%d %H:%M:%S')
            elif part_type == 0x0009: # Time Interval (high resolution)
                interval = struct.unpack('>Q', part_value)[0] / pow(2, 30)
            elif part_type == 0x0002: # Plugin
                context['plugin'] = part_value.split(b'\x00')[0].decode('utf-8')
            elif part_type == 0x0003: # Plugin Instance
                context['plugin_instance'] = part_value.split(b'\x00')[0].decode('utf-8')
            elif part_type == 0x0004: # Type
                context['type'] = part_value.split(b'\x00')[0].decode('utf-8')
            elif part_type == 0x0005: # Type Instance
                context['type_instance'] = part_value.split(b'\x00')[0].decode('utf-8')
            elif part_type == 0x0006: # Values
                if len(part_value) < 2:
                    continue

                values = []
                num_values = struct.unpack('>H', part_value[:2])[0]

                # Get all value types
                types = struct.unpack(f'>{num_values}B', part_value[2:2+num_values])

                # Get all values
                value_offset = 2 + num_values
                for data_type in types:
                    val_bytes = part_value[value_offset:value_offset + 8]
                    if data_type == DS_TYPE_DERIVE:
                        values.append(struct.unpack('>q', val_bytes)[0])
                    elif data_type == DS_TYPE_GAUGE:
                        values.append(struct.unpack('<d', val_bytes)[0])
                    else:
                        values.append(struct.unpack('>Q', val_bytes)[0])
                    value_offset += 8

                return {
                    **context,
                    'values': values,
                    'value_type': TYPE_NAMES.get(data_type, 'unknown')
                }
            elif part_type == 0x0100: # Message (notifications)
                return {
                  **context,
                  'message': part_value.decode('utf-8')
                }
            elif part_type == 0x0101: # Severity
                severity = struct.unpack('>q', part_value)[0]
                context['severity'] = SEVERITY_NAMES.get(severity, f"LEVEL {str(severity)}")
            else:
              print(f"unknown part type {part_type}")
        except (UnicodeDecodeError, struct.error, IndexError) as e:
            print(f"Error parsing part: {e}")
            continue

    return None

def main(host, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((host, port))
    print(f"Listening for collectd packets on {host}:{port}")

    try:
        while True:
            data, addr = sock.recvfrom(4096)
            result = parse_collectd_packet(data)
            if result:
                host = result.get('host', 'N/A')
                time = result.get('time', 'N/A')
                plugin = result.get('plugin', 'N/A')
                plugin_inst = result.get('plugin_instance', '')
                type = result.get('type', 'N/A')
                type_inst = result.get('type_instance', '')
                if 'values' in result:
                    value = ",".join(map(str, result['values']))
                elif 'message' in result:
                    value = result['message']
                else:
                    value = "???"
                severity = result.get('severity', 'UKNOWN')
                print(f"{time} | {severity:6} | {host}:{plugin}:{plugin_inst}:{type}:{type_inst} | {value}")
    except KeyboardInterrupt:
        print("\nExiting...")
    finally:
        sock.close()

if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description='Collectd network client')
    parser.add_argument('--host', default='localhost', help='Host to listen on')
    parser.add_argument('--port', type=int, default=25826, help='Port to listen on')

    args = parser.parse_args()

    main(args.host, args.port)
