# Changelog

This changelog contains all internal changes to collectd.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [Unreleased]

### Fixed

## [5.12.1-34] - 2025-07-24

- processes2 plugin:
  - missed return of correct process state
  - process groups defined by exact matches (before: same start)

## [5.12.1-33] - 2025-07-11

### Added

- core:
  - filtering notifications by "Notification" chain
- processes2:
  - new options 'NotifyIOTopReadSingleLine' and 'NotifyIOTopWriteSingleLine'

## [5.12.1-32] - 2025-05-13

### Added

- mbient qnx plugins
  - added netstat plugin for QNX reporting TCP, UDP and ARP statistics.

### Changed

- processes2 plugin:
  - naming of processes, replace all special characters by underscore

## [5.12.1-31] - 2025-04-15

### Fixed

- core:
  - benchmark for value dispatching
  - earlier clock type configuration

### Changed

- mbient plugins:
  - changed license to MIT

## [5.12.1-30] - 2025-03-19

### Added

- core:

  - new global option `ClockType` for flexible time measurement
  - added benchmarks for dispatching values and notifications

- memory2 plugin:

  - swaptotal/swapfree/swapcached in memory total log.
  - orig/compr/used/ratio in memory zram log.

- cgroups2:

  - cgroup memory swap usage log added if swap memory enabled

- processes2:
  - diff_of_VmSwap, VmSwap in top-memdiff-program log
  - swap usage data in mem top processes log

### Fixed

- QNX memory plugin:
  - fixed logging of top shmem users when file names are long
- QNX cpu plugin:
  - do not expect certain VMs to be running but detect them at runtime

### Changed

- cgroups2:
  - more efficient directory filter implemented

## [5.12.1-29] - 2025-01-15

### Added

- QNX NPU plugin:
  - report the utilization of the NPU (neural processing unit)

### Fixed

- vram plugin:
  - double closing a file descriptor
- cgroups2:
  - removing apostrophes in the notification message

## [5.12.1-28] - 2024-12-09

### Added

- cgroups2: more tracing capabilities

### Fixed

- cgroups2: double closing a file descriptor

### Changed

- cgroups2 plugin: optimized file scanning

## [5.12.1-27] - 2024-10-23

### Added

- QNX memory plugin:
  - report shared memory usage
- Linux Video RAM memory plugin:
  - report total vram usage, top vram processes

### Changed

- QNX GPU plugin:
  - wait for two read intervals time before opening slog2 buffer
- disable RRD for richos:
  - plugins disabled - buddyinfo, contextswitch, df, disk, Entropy,
    ethstat, interface, irq, load, vmem

## [5.12.1-26] - 2024-09-24

### Added

- QNX memory plugin:
  - report total, used and per memory region usage
- Linux cgroups2 plugin:
  - new 'cgroup memory pressure' and 'cgroup io pressure' logs

### Changed

- QNX GPU plugin:
  - report context ID for GPU processes
  - disable GPU report logging when collectd is stopped
- QNX CPU plugin:
  - harmonize total cpu usage naming
- QNX configuration:
  - set "Hostname" to "SafeOS"
- Linux memory2 plugin:
  - report below memory details
  - added Data: total,free,available,buffers,cached,shmem,slab
  - applied `clang-format`
- Linux cgroups2 plugin:
  - 'cgroup cpu pressure' metrics retrives from PSI pressue
- Linux cpu plugin:
  - stop 'cpu pressure <hostname>' logging
- Linux memory2 plugin:
  - stop 'memory pressue <hostname>' logging

## [5.12.1-25] - 2024-08-09

### Added

- cpu:
  - new option `ReportCpuPressure` to collect and report total CPU pressure
- groups2:
  - more debug logging

### Fixed

- cgroups2:
  - v1/v2 detection, avoid duplicated reads

## [5.12.1-24] - 2024-07-16

### Added

- cgroup2 plugin:

  - option `MountCache`: read mount information only ones, default `true`

- processes2 plugin:

  - new configuration `NotifyMemTopSingleLine 10` that logs top 10
    memory consumers list to single line

- memory2 plugin:
  - new plugin
  - rewritten of memory plugin + memory pressure

### Changed

- cgroup2 plugin:
  - reduced number of system calls for traversing directories

### Fixed

- QNX GPU plugin:
  - handled cases where frequency of logs is higher than that of plugin
- cgroup2 plugin:
  - counting processes and threads per cgroup
  - optimize deactivation of unsupported metrics

## [5.12.1-23] - 2024-05-23

### Added

- cpu plugin:
  - each core pressure information from schedstat file

### Fixed

- QNX GPU plugin:
  - fixed issue where when a GPU process exits it is still
    reported with its last GPU load value

## [5.12.1-22] - 2024-05-08

### Added

- cpu plugin:
  - new option `ReportActiveState`

### Changed

- cpu plugin:
  - reverted all changes related to QNX, QNX has its own plugin in
    `mbient/plugins/qnx`
  - always calculate and dispatch global cpu metrics

### Fixed

- cgroup2 plugin:
  - added missing include statements
- QNX GPU plugin:
  - fixed regex for total GPU load string parsing

## [5.12.1-21] - 2024-05-02

### Changed

- cgroups2 plugin:
  - different directory traversal strategy
  - output of 'UNKOWN' in case of missing size information
- QNX GPU plugin:
  - changed name of the buffer containing GPU load stats
- QNX config:
  - interval for CPU and GPU plugin is set to 5 seconds
- QNX CPU plugin:
  - per core and overall load for VMs is now reported

### Fixed

- processes2 plugin:
  - main loop for listing top processes
- build env:
  - clean.sh: added `classnoinst.stamp` for cleaning
- write_dlt plugin:
  - support empty meta name in wdlt_notify

### Removed

- cgroups2 plugin:
  - option 'ProcessCountUnique' has no effect any more
  - option 'ThreadCountUnique' has no effect any more
  - removed unused function `cmpl`

## [5.12.1-20] - 2024-01-18

### Added

- processes2 plugin:
  - new configuration switch `NotifyCpuTopSingleLine` (ON/OFF) that switches
    cpu consumer list to single
  - added workaround for unicode character `\x2d` in cgroup names
- cgroups2 plugin:
  - new configuration switch `CGroupVersion` (`0`,`1`,`2`).
    Default is `0` for auto detection. While 1 only checks for cgroup v1
    configuration, 2 checks for cgroup v2.
  - cgroup version in notifications
  - optional number of processes and threads (APRICOT-403282)
  - added workaround for unicode character `\x2d` in cgroup names
- write_dlt plugin:
  - avoid loss of information due to long messages (APRICOT-41158)
- collectd:
  - print benchmark results aligned and in ms (APRICOT-394752)
  - some reformatting (follow clang-format rules given by collectd)
    (APRICOT-428761)
  - using thread names inside collectd to support performance analysis
    (APRICOT-421710)

### Changed

- Configuration files: Multiple partial configuration files are replaced by
  a single mbient (richos) configuration file `collectd.conf`. File's content
  can directly be copied to various mbient configuration variants.

### Fixed

- cgroups2 plugin:
  - automatic mode for each controller
  - endless loop in `cg2_handle_files_recurse_fully()`

## [5.12.1-19] - 2023-11-23

### Added

- cgroups2 plugin:
  New configuration switches `ProcessCount` & `ThreadCount` (ON/OFF).
  When ON, cgroup cpu log is extended with '/prc' heading and process count,
  and '/thr' heading and thread count, respectively. The process count is
  determined by reading 'cgroup.procs' files of all leaf sub-cgroups in the
  cgroup pseudo file system. The thread count is determined by reading
  `cgroup.threads` files, respectively.
  By default, duplicate PIDs are removed. For speed, this
  can be disabled for speed by setting `ProcessCountUnique` and
  `ThreadCountUnique` to OFF. (APRICOT-403282).
- main executable: Print benchmark results aligned and in ms (APRICOT-394752)
- sdnotify plugin: more robustness in case of missing watchdog
  configuration (APRICOT-359437)

### Fixed

- main executable: Fix segfault on WritePluginProfiling. WritePluginProfiling
  can now be used (MBIENT-50429)
- cgroups2 plugin: Interpret unset memory limit with cgroup2 as NO_LIMIT

### Changed

- Switch main development platform from Ubuntu 20.04 to Ubuntu 22.04
  (APRICOT-357158).

## [5.12.1-18] - 2023-10-06

### Added

- write_dlt plugin:
  - Improve regex implementation
  - Add configuration to output daemon log to DLT: Option 'LogLevel' takes
    the same arguments as the logfile plugin. (APRICOT-343632)
  - Add optional configuration 'LogTarget' which can set the target DLT
    context for service logs. Default value is "SERV". (APRICOT-343632)
  - Improve descriptions for DLT items (APRICOT-343631)
- sdnotify plugin:
  - Added as prospective upstream module. Further info in README.
- threads plugin:
  - Introduce as experimental plugin to benchmark BPF-based implementation
- mbient/tools/thread_demo showing thread data is equivalent to
  process data in terms of complexity (APRICOT-296105)
- QNX support:
  - thermal plugin
  - gpu plugin
  - cpu plugin

### Fixed

- processes2 plugin:
  - sorting function (APRICOT-344155)
- cgroups2 plugin:
  - naming of cgroups with underscore (MBIENT-44472)
  - debug compile flag
- write_dlt plugin:
  - sorting function (APRICOT-344155)
- client:
  - Use same maximal length for value identifiers as server
- curl plugin :
  - Replace deprecated symbols like CURLINFO_SIZE_UPLOAD

## [5.12.1-17] - 2023-09-12

### Fixed

- write_dlt: Fix shutdown race condition

## [5.12.1-16] - 2023-07-11

### Added

- cgroups2 plugin:
  - optional configuration variable "MaxLevel" (>=1) that limits the maximum
    level of the cgroup hierarchy that is shown (MBIENT-41247)
  - optional configuration switch "SortAlphabetical" which can enable
    alphabetical sorting of cgroups by name (MBIENT-41247)
  - optional configuration variable "MemoryUnit", which can be "kB",
    "MB", or "GB" (MBIENT-41247)

### Changed

- cgroups2 plugin:
  - Automatic memory unit: prefer greater unit if magnitude would be equal to
    one, that is, prefer 1MB to 1024kB.
  - When no cgroup memory limit is in place, display special string NO_LIMIT
    (MBIENT-41247)
  - When no cgroup memory limit is in place, display special string NO_LIMIT
    (MBIENT-41247)

### Fixed

- sdbus plugin: Guard against premature shutdown
- cgroups2 plugin:
  - Automatic memory unit: prefer greater unit if magnitude would be equal to
    one, that is, prefer 1MB to 1024kB.

## [5.12.1-15] - 2023-05-17

### Added

- write_dlt plugin: Log daemon version at startup (APRICOT-266783)
- cgroups2, cpu, processes2, sdbus, write_dlt plugins: Log collectd version at
  startup in dlt (APRICOT-266783)

## [5.12.1-14] - 2023-05-05

### Added

- cgroups2 plugin: Added cpu pressure which is disabled by default and enabled
  with the configuration switch "CpuPressure"(APRICOT-204640)

## [5.12.1-13] - 2023-04-26

### Added

- processes2 plugin:
  - add pss based statistics (APRICOT-221442)

### Changed

- types.db:
  - Install mbient type database separately (APRICOT-266816)

### Fixed

- sdbus plugin:
  - Join threads at exit to free memory, and enable the test in the CI
    (MBIENT-41406)

## [5.12.1-12] - 2023-03-07

### Added

- memory plugin:
  - new option `ExtraStats` added
  - extra data points: available, anon_pages, mapped, shmem
- cpu plugin:
  - cpu: add notification for CPU logline

## [5.12.1-11] - 2023-01-10

### Added

- cgroup2 plugin:
  - new plugin
- write_dlt:
  - output of notification attributes added

### Changed

- processes2:
  - reduced logging

## [5.12.1-10] - 2022-11-15

### Added

- processes2 plugin:
  - added more statistics to notify

## [5.12.1-9] - 2022-11-15

### Changed

- processes2 plugin:
  - avoid using real-time clock
  - code cleanup (removed unsupported platforms)

## [5.12.1-8] - 2022-11-03

### Added

- processes2 plugin:
  - support of top memory consumer notifications added

## [5.12.1-7] - 2022-10-22

### Changed

- processes2 plugin:
  - long-term cpu information (percent, ranking) changed from "all" to "avg"

## [5.12.1-6] - 2022-09-26

### Changed

- processes2 plugin:
  - caching of command line and cgroup information
  - more precise measurement of latest cpu usage

## [5.12.1-5] - 2022-08-04

### Fixed

- processes2 plugin: consistent timestamp for all submitted values

## [5.12.1-4] - 2022-07-07

### Fixed

- processes2 plugin: determination of the user id has been corrected

## [5.12.1-3] - 2022-07-01

### Fixed

- processes2 plugin: significant performance improvements
- fixes for QNX support
- python plugin: disabled deprecated function for >= 3.9

## [5.12.1-2] - 2022-06-02

### Added

- test configuration:
  - synched test with production
  - single read/write thread for testing
- processes2 plugin:
  - correct return value in shutdown
  - improved notification message format
  - performance optimization
- tooling: helper script for debugging added

## [5.12.1-1] - 2022-05-30

### Added

- processes2 plugin: First version
- sdbus plugin: First version
- write_dlt: First version
- match_last_value: First version
- threshold plugin: Fix fixed for deactivated plugin
- logfile plugin: Better support of notification severity
- plugin: added general performance measurement capabilities
