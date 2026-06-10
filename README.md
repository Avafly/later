# later

[![Coverity Scan](https://img.shields.io/coverity/scan/32880.svg)](https://scan.coverity.com/projects/later)

A background/delayed command execution tool for Linux and macOS.

I wrote `later` because I often need to run things in the background or schedule them for later, and `at` didn't quite fit my workflow. I wanted a tool that doesn't require a system daemon, and still lets me see and manage tasks throughout their lifecycle.

## Demos

- Delay a task (e.g., pushing code after work hours so I don't forget)

  ```bash
  echo "git push origin main" | later 18:15
  ```

- Run commands in the background as I don't want to open a new terminal (check progress with `later --log`):

  ```bash
  $ later +0m # run commands immediately
  Execute at:  2026-02-13 18:55:56 (0s)
  Working dir: /Users/user/whatever/build
  later> cmake ..
  later> make -j`nproc`
  later> make install PREFIX=./install
  later>
  Task 1770976574_95743_a3f1 created
  ```

## How it works

1. Each task's state is written only by its own daemon, a single-writer model.
2. There is no background daemon managing tasks; state is represented by files, and transitions are made atomic with `open(O_EXCL)` and `rename()`, which resists races and crashes.
3. A file lock (`flock`) tracks daemon liveness, and marker files represent a task's state.
4. Each task runs as a daemon (double fork, `setsid`, `execl`) and is managed through its process group, so cancelling reaches the whole command tree.
5. stdout/stderr are redirected to a log file, viewable anytime with `--log`.
6. Daemons are controlled through signals (cancel, pause/resume, purge).

### Task lifecycle

```
     create
       │
       ▼
    ┌─────────┐   exec_at    ┌─────────┐    all ok     ┌───────────┐
    │ PENDING │ ───────────► │ RUNNING │ ────────────► │ COMPLETED │
    └─────────┘              └─────────┘               └───────────┘
       │   ▲                  │   ▲   │  a cmd fails   ┌────────┐
       │   │                  │   │   └──────────────► │ FAILED │
  pause│   │resume       pause│   │resume              └────────┘
       │   │                  │   │                        ▲
       ▼   │                  ▼   │                        │ daemon dies
    ┌────────┐               ┌────────┐                    │ (crash/OOM/kill)
    │ PAUSED │               │ PAUSED │
    └────────┘               └────────┘

  From PENDING/RUNNING/PAUSED:
       later --cancel ──► CANCELLED
       daemon dies    ──► FAILED (crash/OOM/kill)
```

## Differences from `at`

| | `later` | `at` |
|---|---|---|
| Architecture | No background service, tasks stored as files | Centralized `atd` daemon |
| Output | `later --log` | Sent via system mail |
| Crash handling | Task file status and file lock | Lost if `atd` crashes |
| Cancellation | Cancel pending and running tasks | Cancel pending tasks only |
| Task visibility | Full lifecycle status (Running, Paused, Failed, ...) | Pending tasks only |
| Input | Pipe or interactive | Pipe or interactive |
| Extras | `--retry`, `--pause`/`--resume`, `--clean`, `--purge` | — |

## Examples

**Background build**

```bash
$ later +1m
Execute at:  2026-02-12 22:20:56 (1m)
Working dir: /Users/user/Downloads/build
later> cmake ../opencv-4.x
later> make -j4
later> make install DESTDIR=./install
later>
Task 1770902509_74290_b1c2 created
```

**Check task**

```bash
$ later -l
    Status     Created at           Execute at           Cmds
1   running    2026-02-12 22:19:56  2026-02-12 22:20:56  3
```

**View progress**

```bash
$ later --log 1 | tail -3
[  6%] Linking C static library ../lib/libzlib.a
[  6%] Building C object 3rdparty/openjpeg/openjp2/CMakeFiles/libopenjp2.dir/mqc.c.o
[  6%] Built target zlib
```

**Pause / resume a running task**

```bash
$ later --pause 1
Task 1770902509_74290_b1c2 paused
$ later --resume 1
Task 1770902509_74290_b1c2 resumed
```

**Cancel task**

```bash
$ later --cancel 1
Task 1770902509_74290_b1c2 cancelled
$ later --log 1 | tail -3
make[2]: *** Waiting for unfinished jobs....
make: *** [all] Terminated: 15
Task cancelled by user.
```

**Retry task**

```bash
$ later --retry 1 +0s
Execute at:  2026-02-17 22:26:43 (0s)
Working dir: /Users/user/projects/build
Commands:
  1. cmake ../opencv-4.x
  2. make -j4
  3. make install DESTDIR=./install
Task 1771334803_35103_c9d0 created
```

## Dependencies

All third-party libraries are bundled in `src/3rdparty`. You only need a C11 compiler.

| Library | Use |
|---|---|
| [cofyc/argparse](https://github.com/cofyc/argparse) | command-line option parsing |
| [antirez/linenoise](https://github.com/antirez/linenoise) | interactive line editing & completion |

## Uninstall

```bash
later --purge         # cancel every task and erase the data directory
rm ~/.local/bin/later # remove executable binary
```
