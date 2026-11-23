-include ../config.mk

CC = gcc

WARNINGS := -Wall -Wextra -Wshadow

CPPFLAGS = -I/usr/local/include -D_XOPEN_SOURCE=600
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
	CPPFLAGS += -D_DARWIN_C_SOURCE
endif

CFLAGS = -O2 -I../include -std=c11 -MMD -MP $(WARNINGS)
LDFLAGS = -O2 -L/usr/local/lib
LDLIBS = -lsoundio -lmp3lame -lsndfile -lavformat -lavcodec -lavutil -lvterm -lpthread -lm

TARGET := castty
OBJ := audio.o cast.o cast_doc.o cast_player.o castty.o input.o json.o output.o record.o shell.o signals.o term.o xwrap.o audio/pcm.o audio/pcm_track.o audio/writer-lame.o audio/writer-m4a.o audio/writer-raw.o audio/writer-sndfile.o tui/hud.o tui/screen.o

all: $(TARGET)
$(TARGET): $(OBJ)

debug: all
debug: CFLAGS += -Og -ggdb3 -fno-omit-frame-pointer
debug: LDFLAGS += -Og -ggdb3

clean:
	$(RM) $(OBJ) $(OBJ:.o=.d) $(TARGET)

install: $(TARGET)
	install -Dm755 -s $(TARGET) -t $(DESTDIR)$(PREFIX)/bin

-include $(OBJ:.o=.d)

.PHONY: all clean
