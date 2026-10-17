# fldigi-rtty-core -- plain make, C99, no dependencies besides libm.
#   make            static + shared library into $(BUILD)
#   make test       build and run the C tests (receiver and transmitter)
#   make sanitize   the C tests under AddressSanitizer + UndefinedBehaviorSanitizer
#   make BUILD=/abs/dir shared   out-of-tree build (used by wave-owl)
BUILD ?= build
CC ?= cc
# -ffp-contract=off: no fused multiply-add, the arithmetic follows the reference order.
CFLAGS ?= -O3
CORE_FLAGS = -std=c99 -Wall -Wextra -Werror -pedantic -ffp-contract=off -fPIC -Iinclude
LDLIBS = -lm

UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
  SHLIB = libfldigi_rtty.dylib
  SHFLAGS = -dynamiclib -install_name @rpath/libfldigi_rtty.dylib
else
  SHLIB = libfldigi_rtty.so
  SHFLAGS = -shared -Wl,-soname,libfldigi_rtty.so
endif

OBJ = $(BUILD)/rtty_rx.o $(BUILD)/rtty_tx.o

all: static shared
static: $(BUILD)/libfldigi_rtty.a
shared: $(BUILD)/$(SHLIB)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c include/fldigi_rtty.h | $(BUILD)
	$(CC) $(CORE_FLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/libfldigi_rtty.a: $(OBJ)
	ar rcs $@ $^

$(BUILD)/$(SHLIB): $(OBJ)
	$(CC) $(SHFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_rtty: tests/test_rtty.c $(BUILD)/libfldigi_rtty.a
	$(CC) $(CORE_FLAGS) $(CFLAGS) $< $(BUILD)/libfldigi_rtty.a -o $@ $(LDLIBS)

$(BUILD)/test_rtty_tx: tests/test_rtty_tx.c $(BUILD)/libfldigi_rtty.a
	$(CC) $(CORE_FLAGS) $(CFLAGS) $< $(BUILD)/libfldigi_rtty.a -o $@ $(LDLIBS)

test: $(BUILD)/test_rtty $(BUILD)/test_rtty_tx
	$(BUILD)/test_rtty tests/golden
	$(BUILD)/test_rtty_tx tests/golden

SAN_FLAGS = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

sanitize:
	$(MAKE) --no-print-directory BUILD=$(BUILD)/san CFLAGS="$(SAN_FLAGS)" \
		LDLIBS="$(LDLIBS) $(SAN_FLAGS)" test

clean:
	rm -rf $(BUILD)

.PHONY: all static shared test sanitize clean
