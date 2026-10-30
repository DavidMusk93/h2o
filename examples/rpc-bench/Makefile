CXX ?= c++
H2O_ROOT ?= ../..
H2O_BUILD ?= $(H2O_ROOT)/build
OUT ?= .
CXXFLAGS ?= -O3 -g -std=c++17 -Wall -Wextra -Werror
CPPFLAGS += -DH2O_USE_LIBUV=0 -I. \
	-isystem $(H2O_ROOT)/include -isystem $(H2O_ROOT)/deps/klib \
	-isystem $(H2O_ROOT)/deps/picotls/include -isystem $(H2O_ROOT)/deps/quicly/include \
	-isystem $(H2O_BUILD)
LDLIBS ?= -lssl -lcrypto -lz -lm -ldl -lpthread

.PHONY: all
all: $(OUT)/h2o-native-rpc-client $(OUT)/h2o-native-rpc-server

$(OUT)/h2o-native-rpc-client: client.cpp alloc_counter.cpp benchmark.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) client.cpp alloc_counter.cpp $(H2O_BUILD)/libh2o-evloop.a $(LDFLAGS) $(LDLIBS) -o $@

$(OUT)/h2o-native-rpc-server: server.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) server.cpp $(H2O_BUILD)/libh2o-evloop.a $(LDFLAGS) $(LDLIBS) -o $@
