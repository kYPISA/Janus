CC = gcc
CXX = g++
CFLAGS = -O3 -DNDEBUG -std=c11 -I. -IC -Ibrotli-1.1.0/c/include
CXXFLAGS = -O3 -DNDEBUG -std=c++17 -I. -IC -Ibrotli-1.1.0/c/include -Ilib
LDFLAGS = -static -static-libgcc -static-libstdc++

ifeq ($(OS),Windows_NT)
LIBS = -lpsapi
else
LIBS =
endif
7Z_OBJS = C/7zCrc.o C/7zCrcOpt.o C/7zStream.o C/Alloc.o C/CpuArch.o C/LzFind.o C/LzFindMt.o C/LzmaDec.o C/LzmaEnc.o C/LzFindOpt.o C/Threads.o

BROTLI_COMMON_OBJS = brotli-1.1.0/c/common/dictionary.o brotli-1.1.0/c/common/context.o brotli-1.1.0/c/common/constants.o brotli-1.1.0/c/common/transform.o brotli-1.1.0/c/common/platform.o brotli-1.1.0/c/common/shared_dictionary.o

BROTLI_ENC_OBJS = brotli-1.1.0/c/enc/bit_cost.o brotli-1.1.0/c/enc/backward_references_hq.o brotli-1.1.0/c/enc/compress_fragment_two_pass.o brotli-1.1.0/c/enc/command.o brotli-1.1.0/c/enc/memory.o brotli-1.1.0/c/enc/block_splitter.o brotli-1.1.0/c/enc/metablock.o brotli-1.1.0/c/enc/static_dict.o brotli-1.1.0/c/enc/dictionary_hash.o brotli-1.1.0/c/enc/entropy_encode.o brotli-1.1.0/c/enc/encode.o brotli-1.1.0/c/enc/backward_references.o brotli-1.1.0/c/enc/compress_fragment.o brotli-1.1.0/c/enc/cluster.o brotli-1.1.0/c/enc/compound_dictionary.o brotli-1.1.0/c/enc/histogram.o brotli-1.1.0/c/enc/utf8_util.o brotli-1.1.0/c/enc/fast_log.o brotli-1.1.0/c/enc/brotli_bit_stream.o brotli-1.1.0/c/enc/encoder_dict.o brotli-1.1.0/c/enc/literal_cost.o

BROTLI_DEC_OBJS = brotli-1.1.0/c/dec/bit_reader.o brotli-1.1.0/c/dec/decode.o brotli-1.1.0/c/dec/huffman.o brotli-1.1.0/c/dec/state.o

OBJS = $(7Z_OBJS) $(BROTLI_COMMON_OBJS) $(BROTLI_ENC_OBJS) $(BROTLI_DEC_OBJS) main.o

NOTATION_SRC = lib/janus_notation.cpp lib/janus_notation_encoder.cpp lib/janus_notation_decoder.cpp
NOTATION_OBJS = lib/janus_notation.o lib/janus_notation_encoder.o lib/janus_notation_decoder.o

TARGET = janus
LIB_TARGET = lib/libjanus_notation.a

.PHONY: all clean libjanus_notation test_notation

all: $(TARGET)

$(TARGET): $(OBJS) $(NOTATION_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

libjanus_notation: $(LIB_TARGET)

$(LIB_TARGET): $(NOTATION_OBJS)
	ar rcs $@ $^

lib/janus_notation.o: lib/janus_notation.cpp lib/janus_notation.hpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

lib/janus_notation_encoder.o: lib/janus_notation_encoder.cpp lib/janus_notation_encoder.hpp lib/janus_notation.hpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

lib/janus_notation_decoder.o: lib/janus_notation_decoder.cpp lib/janus_notation_decoder.hpp lib/janus_notation.hpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test_notation: $(LIB_TARGET) tests/test_notation.cpp
	$(CXX) $(CXXFLAGS) -o tests/test_notation tests/test_notation.cpp $(LIB_TARGET)
	./tests/test_notation

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
ifeq ($(OS),Windows_NT)
	-del /Q $(subst /,\\,$(OBJS)) $(TARGET).exe 2>nul
	-del /Q $(subst /,\\,$(NOTATION_OBJS)) $(subst /,\\,$(LIB_TARGET)) 2>nul
	-del /Q tests\\test_notation.exe 2>nul
else
	$(RM) $(OBJS) $(TARGET)
	$(RM) $(NOTATION_OBJS) $(LIB_TARGET)
	$(RM) tests/test_notation
endif
