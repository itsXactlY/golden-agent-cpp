# Golden-Agent C++26 port — build system
CXX ?= g++
CXXFLAGS ?= -std=c++26 -O2 -Wall -Wextra -Isrc
LDLIBS ?= -lcurl -lz -lpthread

SRC_DIR  := src/golden_agent
BUILD    := build
CORE     := util json http archive
LIB_SRCS := $(addprefix $(SRC_DIR)/,$(addsuffix .cc,$(CORE))) $(wildcard $(SRC_DIR)/*.cc)
LIB_SRCS := $(filter-out $(addprefix $(SRC_DIR)/main.cc,$(LIB_SRCS)),$(LIB_SRCS))
# server_daemon.cc has its own int main — it is a standalone binary, not part of libga.a
LIB_SRCS := $(filter-out $(addprefix $(SRC_DIR)/server_daemon.cc,$(LIB_SRCS)),$(LIB_SRCS))
LIB_OBJS := $(LIB_SRCS:$(SRC_DIR)/%.cc=$(BUILD)/%.o)

TEST_SRCS := $(wildcard tests/test_*.cc)
TEST_BINARIES := $(addprefix $(BUILD)/test_,$(patsubst tests/test_%.cc,%,\
$(TEST_SRCS)))

.PHONY: all lib daemon example tests run-tests clean
all: lib daemon $(TEST_BINARIES)

lib: $(BUILD)/libga.a

# standalone daemon binary (spawned by backend.cc, talks JSON over --control-port)
daemon: $(BUILD)/server_daemon

$(BUILD)/server_daemon: $(SRC_DIR)/server_daemon.cc $(BUILD)/libga.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) $< $(BUILD)/libga.a $(LDLIBS) -o $@

$(BUILD)/libga.a: $(LIB_OBJS)
	ar rcs $@ $^

$(BUILD)/%.o: $(SRC_DIR)/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/test_%: tests/test_%.cc $(BUILD)/libga.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) $< $(BUILD)/libga.a $(LDLIBS) -o $@

tests: all

$(BUILD)/minimal: examples/minimal.cpp $(BUILD)/libga.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) $< $(BUILD)/libga.a $(LDLIBS) -o $@

example: $(BUILD)/minimal

run-tests: all
	@fail=0; \
	for t in $(TEST_BINARIES); do \
	  echo "== $$t"; "$$t" || fail=1; done; \
	if [ $$fail -ne 0 ]; then echo "FAILURES"; exit 1; fi; \
	echo "ALL TESTS PASSED"

clean:
	rm -rf $(BUILD)
