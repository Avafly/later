# later

[![Coverity Scan](https://img.shields.io/coverity/scan/32880.svg)](https://scan.coverity.com/projects/later)

A background/delayed command execution tool for Linux and macOS.

I wrote `later` because I often need to run things in the background or schedule them for later, and `at` didn't quite fit my workflow. I wanted a tool that doesn't require a system daemon, and still lets me see and manage tasks throughout their lifecycle.

## Demos

- Delay a task (e.g., pushing code after work hours so I don't forget)

  ```bash
  echo "git push origin main" | later 18:15
  ```

- Run commands in the background as I don't want to open a new terminal:

  ```bash
  $ later +0m # run commands immediately
  Execute at:   2026-02-13 18:55:56 (0s)
  Working dir:  /Users/user/whatever/build
  later> cmake ..
  later> make -j`nproc`
  later> make install PREFIX=./install
  later>
  Task 1770976574_95743 created
  ```

## How it works

1. Run each task as a daemon (double fork, setsid, execl). Commands within a task share a process group, so cancelling kills everything cleanly.
2. Redirect stdout/stderr to a log file, so the task's output is viewable anytime with `--logs`.
3. Track daemon liveness via file locks (`flock`).
4. Store and manage tasks as files. No system service.

## Differences from `at`

| | `later` | `at` |
|---|---|---|
| Architecture | No background service, tasks stored as files | Centralized `atd` daemon |
| Output | `later --logs` | Sent via system mail |
| Crash handling | Task file status and file lock | Lost if `atd` crashes |
| Cancellation | Cancel pending and running tasks | Cancel pending tasks only |
| Task visibility | Full lifecycle status (Running, Failed, etc.) | Pending tasks only |
| Input | Pipe or interactive | Pipe or interactive |

## Examples

**Background build**

```bash
$ later +0s
Execute at:   2026-02-12 22:20:56 (0s)
Working dir:  /Users/user/Downloads/build
later> cmake ../opencv-4.x
later> make -j4
later> make install DESTDIR=./install
later>
Task 1770902509_74290 created
```

**Check task**

```bash
$ later -l
    Status     Created at           Execute at           Cmds
1   running    2026-02-12 22:20:56  2026-02-12 22:20:56  3
```

**View progress**

```bash
$ later --logs 1 | tail -3
[  6%] Linking C static library ../lib/libzlib.a
[  6%] Building C object 3rdparty/openjpeg/openjp2/CMakeFiles/libopenjp2.dir/mqc.c.o
[  6%] Built target zlib
```

**Cancel task**

```bash
$ later -c 1
Task 1770902509_74290 cancelled
$ later --logs 1 | tail -4
make[2]: *** Waiting for unfinished jobs....
make[2]: *** [3rdparty/libwebp/CMakeFiles/libwebp.dir/all] Terminated: 15
make: *** [all] Terminated: 15
Task cancelled by user
```

## Dependencies

All 3rdparty libraries are included in `src/3rdparty`. You only need a compiler that supports C++20.

| Library | Version |
|---|---|
| [fmtlib/fmt](https://github.com/fmtlib/fmt) | 12.1.0 |
| [TartanLlama/expected](https://github.com/TartanLlama/expected) | 1.3.1 |
| [CLIUtils/CLI11](https://github.com/CLIUtils/CLI11) | 2.6.1 |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 |
| [antirez/linenoise](https://github.com/antirez/linenoise) | Commit c12b66d |

## Tested on

- macOS 15.7.3
- Debian 12.13
- Ubuntu 24.04.3