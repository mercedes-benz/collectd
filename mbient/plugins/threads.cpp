/**
 * SPDX-FileCopyrightText: 2023 MBition GmbH
 **/

extern "C" {
#include "plugin.h"
#include "utils/common/common.h"
#include "collectd.h"
}

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>

#define PLUGIN_NAME "threads"
#define LOG_KEY PLUGIN_NAME " plugin: "

static const std::string BPF_PATH = "/sys/fs/bpf/threads";

struct task_data {
  bool hot = false;
  std::string name = {};
};

static std::unordered_map<pid_t, task_data> tdata_cache;

static int threads_init(void) {
  try {
    NOTICE(LOG_KEY
           "Experimental implementation for testing. NOT FOR PRODUCTION.");

    if (!std::filesystem::exists(BPF_PATH)) {
      ERROR(LOG_KEY "Can't find %s", BPF_PATH.c_str());
      return -1;
    }
    if (!std::ifstream(BPF_PATH)) {
      ERROR(LOG_KEY "Can't open %s", BPF_PATH.c_str());
      return -1;
    }
  } catch (std::exception &e) {
    ERROR(LOG_KEY "%s", e.what());
    return -1;
  } catch (...) {
    ERROR(LOG_KEY "Unknown error");
    return -1;
  }

  return 0;
}

static void replace(std::string &s, std::map<char, char> R) {
  char r;
  std::replace_if(
      s.begin(), s.end(),
      [&](char c) {
        if (auto i = R.find(c); i != R.end()) {
          r = i->second;
          return true;
        }
        return false;
      },
      r);
}

static int threads_read(void) {
  try {
    cdtime_t now = cdtime();

    std::ifstream is(BPF_PATH);
    if (!is) {
      ERROR(LOG_KEY "Can't open %s", BPF_PATH.c_str());
      return -1;
    }

    std::string line;
    while (std::getline(is, line)) {
      pid_t pid, tid;
      long unsigned utime, stime;
      std::istringstream line_input(line);
      line_input >> pid >> tid >> utime >> stime;
      if (!line_input) {
        ERROR(LOG_KEY "Can't parse line '%s'", line.c_str());
        continue;
      }

      value_t values[3];
      value_list_t vl = {.values = values, .time = now, .meta = NULL};

      auto &tdata = tdata_cache[tid];
      tdata.hot = true;

      std::string &tname = tdata.name;
      if (tname.empty()) {
        const std::string cmdlinep =
            "/proc/" + std::to_string(tid) + "/cmdline";
        std::ifstream cmdlineis(cmdlinep);
        if (!cmdlineis) {
          ERROR(LOG_KEY "Can't open %s", cmdlinep.c_str());
          continue;
        }
        std::string cmdline;
        cmdlineis >> cmdline;
        const std::map<char, char> TNAME_XCHARS = {{'/', '!'}, {'-', '_'}};
        if (!cmdline.empty() && std::filesystem::path(cmdline).is_absolute()) {
          tname = cmdline.substr(0, cmdline.find_first_of('\0'));
          replace(tname, TNAME_XCHARS);
          tname += "." + std::to_string(tid);
          INFO(LOG_KEY "Set thread name: %d is %s.", tid, tname.c_str());
        } else {
          std::error_code ec;
          const std::string exeln = "/proc/" + std::to_string(tid) + "/exe";
          const std::string exep = std::filesystem::read_symlink(exeln, ec);
          if (!ec) {
            tname = std::filesystem::absolute(exep, ec).string();
            replace(tname, TNAME_XCHARS);
            tname += "." + std::to_string(tid);
            INFO(LOG_KEY "Set thread name (exe): %d is %s", tid, tname.c_str());
          }
        }

        if (tname.empty()) {
          const std::string commp = "/proc/" + std::to_string(tid) + "/comm";
          std::ifstream commis(commp);
          if (!commis) {
            ERROR(LOG_KEY "Can't open %s", commp.c_str());
            continue;
          }
          commis >> tname;
          tname += "." + std::to_string(tid);
          INFO(LOG_KEY "Set thread name (comm): %d is %s", tid, tname.c_str());
        }
      }

      sstrncpy(vl.plugin, PLUGIN_NAME, sizeof(vl.plugin));
      sstrncpy(vl.plugin_instance, tname.c_str(), sizeof(vl.plugin_instance));
      sstrncpy(vl.type, "ps_cputime", sizeof(vl.type));
      vl.values[0].derive = utime / 1e3;
      vl.values[1].derive = stime / 1e3;
      vl.values_len = 2;

      plugin_dispatch_values(&vl);
    }

    for (auto it = tdata_cache.begin(); it != tdata_cache.end();) {
      if (!it->second.hot) {
        it = tdata_cache.erase(it);
      } else {
        it++->second.hot = false;
      }
    }

  } catch (const std::exception &e) {
    ERROR(LOG_KEY "%s", e.what());
    return -1;
  } catch (...) {
    ERROR(LOG_KEY "Unknown error");
    return -1;
  }

  return 0;
}

static int threads_shutdown() { return 0; }

void module_register(void) {
  plugin_register_init(PLUGIN_NAME, threads_init);
  plugin_register_read(PLUGIN_NAME, threads_read);
  plugin_register_shutdown(PLUGIN_NAME, threads_shutdown);
}
