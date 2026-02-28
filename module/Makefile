# Makefile for func_deepfilter.so - Asterisk DeepFilterNet module
#
# Usage:
#   make              - build the module
#   make install      - copy to Asterisk modules directory
#   make reload       - unload/load module in running Asterisk
#   make clean        - remove built files

MODULES_DIR ?= /usr/lib/asterisk/modules

CC      = gcc
CFLAGS  = -shared -fPIC -Wall -Wextra -O2 \
          -DAST_MODULE_SELF_SYM=__internal_func_deepfilter_self

# Use pkg-config for libdeepfilter if available, otherwise fallback
DEEPFILTER_CFLAGS := $(shell pkg-config --cflags libdeepfilter 2>/dev/null)
DEEPFILTER_LIBS   := $(shell pkg-config --libs libdeepfilter 2>/dev/null)

ifneq ($(DEEPFILTER_CFLAGS),)
  CFLAGS += $(DEEPFILTER_CFLAGS) -DHAVE_DEEP_FILTER_H
  LDFLAGS = $(DEEPFILTER_LIBS) -lspeexdsp -lpthread -lm
else
  CFLAGS += -I/usr/local/include -L/usr/local/lib
  LDFLAGS = -ldeep_filter -lspeexdsp -lpthread -lm
endif

TARGET  = func_deepfilter.so
SOURCES = func_deepfilter.c

.PHONY: all install reload clean

all: $(TARGET)

$(TARGET): $(SOURCES) df_bridge.h
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LDFLAGS)

install: $(TARGET)
	cp $(TARGET) $(MODULES_DIR)/

reload: install
	asterisk -rx "module unload func_deepfilter.so" 2>/dev/null || true
	asterisk -rx "module load func_deepfilter.so"

clean:
	rm -f $(TARGET)
