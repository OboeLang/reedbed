# SPDX-License-Identifier: GPL-2.0-only
# © 2026 Sushii64
# © 2026 robinpie
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.

CC = gcc
CFLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
	 -D_DARWIN_C_SOURCE -Wall -Wextra -Wno-unused-parameter -g \
	 -Isrc -I../common

# ../common is the canonical copy of the wire-format code; the Oboe compiler
# vendors the same files. Edit them there, not in a vendored copy.
COMMON = ../common/sha256.c ../common/izim.c ../common/record.c \
	 ../common/kabuk.c ../common/projectjson.c
COMMON_OBJ = $(COMMON:.c=.o)

SRV = src/config.c src/conn.c src/proto.c src/ratelimit.c \
      src/session.c src/store.c
SRV_OBJ = $(SRV:.c=.o)

HDR = ../common/sha256.h ../common/izim.h ../common/record.h \
      ../common/kabuk.h ../common/projectjson.h src/config.h src/conn.h \
      src/proto.h src/ratelimit.h src/session.h src/store.h

# Every object depends on every header. The header set here is genuinely
# coupled -- conn's types flow into proto's, which flow into session's -- and a
# missed dependency shows up as a stale object that links but misbehaves.

BINS = bin/reedbed bin/reedbed-admin bin/katarecat bin/sema bin/t_common

all: $(BINS)

bin:
	mkdir -p bin

bin/reedbed: bin $(COMMON_OBJ) $(SRV_OBJ) src/main.o
	$(CC) $(CFLAGS) $(COMMON_OBJ) $(SRV_OBJ) src/main.o -o $@

bin/reedbed-admin: bin $(COMMON_OBJ) $(SRV_OBJ) src/admin_main.o
	$(CC) $(CFLAGS) $(COMMON_OBJ) $(SRV_OBJ) src/admin_main.o -o $@

# katarecat speaks the protocol badly on purpose, so it shares nothing with the
# server's framing -- a bug in conn.c must not hide itself from the tests.
bin/katarecat: bin src/katarecat_main.o
	$(CC) $(CFLAGS) src/katarecat_main.o -o $@

bin/sema: bin $(COMMON_OBJ) src/sema_main.o
	$(CC) $(CFLAGS) $(COMMON_OBJ) src/sema_main.o -o $@

bin/t_common: bin $(COMMON_OBJ) tests/t_common.o
	$(CC) $(CFLAGS) $(COMMON_OBJ) tests/t_common.o -o $@

%.o: %.c $(HDR)
	$(CC) $(CFLAGS) -c $< -o $@

test: $(BINS)
	./tests/run_tests.sh

# Regenerate the checksums the vendored copies are checked against. Run this
# after deliberately changing anything in ../common, then re-vendor into the
# Oboe compiler's src/.
MANIFEST_SRC = sha256.c sha256.h izim.c izim.h record.c record.h kabuk.c \
	       kabuk.h projectjson.c projectjson.h

manifest: bin/sema
	cd ../common && $(CURDIR)/bin/sema $(MANIFEST_SRC) > MANIFEST.sha256
	@echo "wrote common/MANIFEST.sha256"

clean:
	rm -f $(COMMON_OBJ) $(SRV_OBJ) src/*.o tests/*.o $(BINS)
	rmdir bin 2>/dev/null || true

.PHONY: all test clean manifest
