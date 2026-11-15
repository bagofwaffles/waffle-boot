# SPDX-License-Identifier: GPL-2.0-or-later
#
# libFuzzer targets for the parsers of untrusted input (tests/fuzz/README.md).
# Included by the top-level Makefile after src/third_party/third_party.mk.
#
#   make fuzz                      build them (clang with libFuzzer)
#   make fuzz-run [FUZZ_TIME=60]   run each for FUZZ_TIME seconds from seeds

FUZZ_CC   ?= clang
FUZZ_TIME ?= 60

FUZZERS := fatfs yaffs2 inflate unzip p224
FUZZ_LIBS_fatfs   := fatfs blockdev
FUZZ_LIBS_yaffs2  := yaffs2 blockdev
FUZZ_LIBS_inflate := zlib
FUZZ_LIBS_unzip   := minizip zlib
FUZZ_LIBS_p224    := microecc

FUZZ_SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined

# yaffs2's list_entry() is the offsetof idiom &((type *)NULL)->member, which
# UBSan's null and object-size checks report on every list walk. ASan still
# catches a real null dereference and every out-of-bounds access.
FUZZ_SAN_yaffs2 := -fno-sanitize=null,object-size
FUZZ_COMMON := -g -O1 $(FUZZ_SAN) -fdebug-prefix-map=$(CURDIR)=. $(TP_CPPFLAGS)

fuzz_objs = $(patsubst %.c,$(BUILD)/fuzz/obj/%.o,$(TP_$(1)_SRCS) $(TP_$(1)_GLUE))

# The components, instrumented: upstream code with its relaxed warnings,
# glue with the strict ones, as in third_party.mk.
define FUZZ_COMPONENT
$$(patsubst %.c,$(BUILD)/fuzz/obj/%.o,$$(TP_$(1)_SRCS)): FUZZ_CFLAGS := -std=gnu11 $$(FUZZ_COMMON) $$(FUZZ_SAN_$(1)) -fsanitize=fuzzer-no-link $$(TP_$(1)_FLAGS) $$(TP_WARN) $$(TP_$(1)_WARN)
$$(patsubst %.c,$(BUILD)/fuzz/obj/%.o,$$(TP_$(1)_GLUE)): FUZZ_CFLAGS := $$(COMMON) $$(FUZZ_COMMON) -fsanitize=fuzzer-no-link $$(TP_$(1)_FLAGS)
endef
$(foreach c,$(sort $(foreach f,$(FUZZERS),$(FUZZ_LIBS_$(f)))),$(eval $(call FUZZ_COMPONENT,$(c))))

$(BUILD)/fuzz/obj/%.o: %.c
	@mkdir -p $(@D)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -MD -MP -c $< -o $@

define FUZZER
$(BUILD)/fuzz/fuzz_$(1): tests/fuzz/fuzz_$(1).c $$(foreach c,$$(FUZZ_LIBS_$(1)),$$(call fuzz_objs,$$(c)))
	@mkdir -p $$(@D)
	$$(FUZZ_CC) $$(COMMON) $$(FUZZ_COMMON) -fsanitize=fuzzer $$^ -o $$@
endef
$(foreach f,$(FUZZERS),$(eval $(call FUZZER,$(f))))

FUZZ_SEEDS := $(BUILD)/fuzz/seeds

$(FUZZ_SEEDS)/.stamp: tests/fuzz/seeds.py $(shell find tests/oracle -name '*.py' -o -name '*.json' -o -name '*.bin') \
		$(shell find tests/fuzz/regressions -type f)
	@rm -rf $(FUZZ_SEEDS)
	$(PYTHON) -I -B tests/fuzz/seeds.py $(FUZZ_SEEDS)
	@touch $@

FUZZ_RUNS := $(FUZZERS:%=fuzz-run/%)

.PHONY: fuzz fuzz-run $(FUZZ_RUNS)

fuzz: $(FUZZERS:%=$(BUILD)/fuzz/fuzz_%)

# Each fuzzer is a target of its own, so that -j runs them side by side,
# each for FUZZ_TIME seconds. New inputs go to $(BUILD)/fuzz/corpus/<fuzzer>;
# a crash leaves $(BUILD)/fuzz/<fuzzer>-crash-* and fails the run. The
# output goes to $(BUILD)/fuzz/<fuzzer>.log and is printed when it ends.
fuzz-run: $(FUZZ_RUNS)

$(FUZZ_RUNS): fuzz-run/%: $(BUILD)/fuzz/fuzz_% $(FUZZ_SEEDS)/.stamp
	@mkdir -p $(BUILD)/fuzz/corpus/$*
	@echo "==> fuzz_$* for $(FUZZ_TIME) s"
	@$< -max_total_time=$(FUZZ_TIME) \
		-rss_limit_mb=4096 -timeout=30 -print_final_stats=1 \
		-artifact_prefix=$(BUILD)/fuzz/$*- \
		$(BUILD)/fuzz/corpus/$* $(FUZZ_SEEDS)/$* > $(BUILD)/fuzz/$*.log 2>&1; \
	s=$$?; echo "--- fuzz_$* ($(BUILD)/fuzz/$*.log)"; cat $(BUILD)/fuzz/$*.log; exit $$s

-include $(patsubst %.o,%.d,$(foreach f,$(FUZZERS),$(foreach c,$(FUZZ_LIBS_$(f)),$(call fuzz_objs,$(c)))))
