CFLAGS ?= -O3 -DNDEBUG

SRCS := \
	src/later.c \
	src/util.c \
	src/exec.c \
	src/store.c \
	src/action.c \
	src/strvec.c \
	src/daemon.c \
	src/timefmt.c \
	src/3rdparty/argparse/argparse.c \
	src/3rdparty/linenoise/linenoise.c
OBJS := $(SRCS:%.c=obj/%.o)

WARN = -Wall -Wextra -Wpedantic
obj/src/3rdparty/%.o: WARN = -w

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
else ifeq ($(UNAME_S),Darwin)
CPPFLAGS += -D_DARWIN_C_SOURCE
endif

later: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

obj/%.o: %.c
	@mkdir -p $(@D)
	$(CC) -std=c11 $(WARN) -Isrc $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -rf obj later

.PHONY: clean

-include $(OBJS:.o=.d)
