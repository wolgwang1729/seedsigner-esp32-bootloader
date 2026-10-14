# host_test/Makefile — Host unit tests (no hardware, no ESP-IDF).
#
# Ported from specter-bootloader-mike/test/Makefile (Catch2 + BL_NO_FATFS +
# UNIT_TEST). Builds two runners because main/esp_image.c selects memory
# windows with CONFIG_IDF_TARGET_ESP32P4 / ESP32S3:
#   build/host_p4/test_runner  — P4 windows (0x48000000, 0x4FF00000, ...)
#   build/host_s3/test_runner  — S3 windows (0x42000000, JMP-zone checks, ...)
#
# Usage:
#   make -C host_test test        # build + run both targets
#   make -C host_test test-p4     # P4 only
#   make -C host_test test-s3     # S3 only

MKDIR_P = mkdir -p

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
HOST_DIR := $(REPO_ROOT)/host_test
CORE_DIR := $(REPO_ROOT)/components/specter_crypto/src
LIB_DIR := $(REPO_ROOT)/components/specter_crypto
MAIN_DIR := $(REPO_ROOT)/main
BUILD_ROOT := $(REPO_ROOT)/build/host_test

# --- Crypto + loader C sources (shared by both targets) ---
C_COMMON := \
	$(CORE_DIR)/bl_section.c \
	$(CORE_DIR)/bl_signature.c \
	$(CORE_DIR)/bl_util.c \
	$(CORE_DIR)/bl_integrity_check.c \
	$(LIB_DIR)/crypto/sha2.c \
	$(LIB_DIR)/crypto/memzero.c \
	$(LIB_DIR)/crc32/crc32.c \
	$(LIB_DIR)/bech32/segwit_addr.c \
	$(LIB_DIR)/secp256k1/src/secp256k1.c \
	$(LIB_DIR)/secp256k1_add/ext_callbacks.c \
	$(HOST_DIR)/bl_syscalls_host.c \
	$(HOST_DIR)/stubs/host_idf_stubs.c \
	$(MAIN_DIR)/bundle_validate.c \
	$(MAIN_DIR)/anti_phish.c \
	$(MAIN_DIR)/bip39_wordlist.c

# esp_image.c is target-specific (compiled per-runner below).
C_P4 := $(C_COMMON) $(MAIN_DIR)/esp_image.c
C_S3 := $(C_COMMON) $(MAIN_DIR)/esp_image.c

CPP_COMMON := \
	$(HOST_DIR)/test_main.cpp \
	$(HOST_DIR)/tests/test_bech32.cpp \
	$(HOST_DIR)/tests/test_bl_section.cpp \
	$(HOST_DIR)/tests/test_bl_signature.cpp \
	$(HOST_DIR)/tests/test_bl_integrity_check.cpp \
	$(HOST_DIR)/tests/test_esp_image.cpp \
	$(HOST_DIR)/tests/test_anti_phish.cpp \
	$(HOST_DIR)/tests/test_bundle_bounds.cpp

INCLUDES := \
	-I$(HOST_DIR) \
	-I$(HOST_DIR)/stubs \
	-I$(MAIN_DIR) \
	-I$(CORE_DIR) \
	-I$(LIB_DIR)/include \
	-I$(LIB_DIR)/crc32 \
	-I$(LIB_DIR)/crypto \
	-I$(LIB_DIR)/bech32 \
	-I$(LIB_DIR)/secp256k1 \
	-I$(LIB_DIR)/secp256k1/include \
	-I$(LIB_DIR)/secp256k1/src \
	-I$(LIB_DIR)/secp256k1_add

DEFS_COMMON := UNIT_TEST BL_NO_FATFS HAVE_CONFIG_H SECP256K1_BUILD \
	__BYTE_ORDER=1234 CRC32_USE_LOOKUP_TABLE_SLICING_BY_8 HOST_TEST

CFLAGS := $(INCLUDES) -MMD -MP -g -O1 -Wall -Wextra \
	-Wno-unused-function -Wno-unused-parameter -Wno-missing-field-initializers \
	$(addprefix -D,$(DEFS_COMMON))
CPPFLAGS := -std=c++14
LDFLAGS += -lstdc++ -lm

P4_DIR := $(BUILD_ROOT)/p4
S3_DIR := $(BUILD_ROOT)/s3

P4_OBJS := $(addprefix $(P4_DIR)/,$(notdir $(C_P4:.c=.o)))
P4_OBJS += $(addprefix $(P4_DIR)/,$(notdir $(CPP_COMMON:.cpp=.o)))
P4_DEPS := $(P4_OBJS:.o=.d)
vpath %.c $(sort $(dir $(C_P4)))
vpath %.cpp $(sort $(dir $(CPP_COMMON)))

S3_OBJS := $(addprefix $(S3_DIR)/,$(notdir $(C_S3:.c=.o)))
S3_OBJS += $(addprefix $(S3_DIR)/,$(notdir $(CPP_COMMON:.cpp=.o)))
S3_DEPS := $(S3_OBJS:.o=.d)

P4_RUNNER := $(P4_DIR)/test_runner.out
S3_RUNNER := $(S3_DIR)/test_runner.out

.PHONY: all test test-p4 test-s3 clean

all: $(P4_RUNNER) $(S3_RUNNER)

test: test-p4 test-s3

test-p4: $(P4_RUNNER)
	@$(P4_RUNNER)

test-s3: $(S3_RUNNER)
	@$(S3_RUNNER)

$(P4_RUNNER): $(P4_OBJS)
	$(CC) $(P4_OBJS) -o $@ $(LDFLAGS)

$(S3_RUNNER): $(S3_OBJS)
	$(CC) $(S3_OBJS) -o $@ $(LDFLAGS)

$(P4_DIR)/%.o: %.c Makefile
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -DCONFIG_IDF_TARGET_ESP32P4=1 -c $< -o $@

$(P4_DIR)/%.o: %.cpp Makefile
	@$(MKDIR_P) $(dir $@)
	$(CXX) $(CFLAGS) $(CPPFLAGS) -DCONFIG_IDF_TARGET_ESP32P4=1 -c $< -o $@

$(S3_DIR)/%.o: %.c Makefile
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -DCONFIG_IDF_TARGET_ESP32S3=1 -c $< -o $@

$(S3_DIR)/%.o: %.cpp Makefile
	@$(MKDIR_P) $(dir $@)
	$(CXX) $(CFLAGS) $(CPPFLAGS) -DCONFIG_IDF_TARGET_ESP32S3=1 -c $< -o $@

clean:
	$(RM) -r $(P4_DIR) $(S3_DIR)

-include $(P4_DEPS) $(S3_DEPS)
