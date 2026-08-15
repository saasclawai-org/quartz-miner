# Quartz firmware host-side test Makefile
# Requires: libssl-dev (for SHA-256), Unity test framework

CC = gcc
CFLAGS = -Wall -Wextra -O0 -g -I. -I../main -Iunity/src
LDFLAGS = -lcrypto

UNITY_SRC = unity/src/unity.c

test: test_quartz_firmware

test_quartz_firmware: test_quartz_firmware.c $(UNITY_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(UNITY_SRC) $(LDFLAGS)
	./$@

$(UNITY_SRC):
	@echo "Downloading Unity test framework..."
	git clone --depth 1 https://github.com/ThrowTheSwitch/Unity.git unity

clean:
	rm -f test_quartz_firmware
	rm -rf unity

.PHONY: test clean
