-include config.mk

CC = gcc

WARNINGS := -Wall -Wextra -Wshadow

CPPFLAGS = -I/usr/local/include -D_XOPEN_SOURCE=600
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
	CPPFLAGS += -D_DARWIN_C_SOURCE
endif

CFLAGS = -O2 -std=c11 -MMD -MP $(WARNINGS)
LDFLAGS = -L/usr/local/lib
LDLIBS = -lsoundio -lpthread

ifeq ($(DEBUG),1)
	CFLAGS += -O0 -g3
endif

TARGET := castty
OBJ := audio.o castty.o input.o output.o shell.o xwrap.o audio/writer-raw.o

# Optional dependency libmp3lame (default: yes)
ifneq ("$(WITH_LAME)", "no")
	CPPFLAGS += -DWITH_LAME
	LDLIBS += -lmp3lame
	OBJ += audio/writer-lame.o
endif

all: $(TARGET)
$(TARGET): $(OBJ)

clean:
	$(RM) $(OBJ) $(OBJ:.o=.d) $(TARGET)

-include $(OBJ:.o=.d)

.PHONY: all clean
