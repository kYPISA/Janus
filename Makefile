CC = gcc
CXX = g++
CFLAGS = -O3 -DNDEBUG -std=c11 -I. -IC -Ibrotli-1.1.0/c/include
CXXFLAGS = -O3 -DNDEBUG -std=c++17 -I. -IC -Ibrotli-1.1.0/c/include
LDFLAGS = -static -static-libgcc -static-libstdc++
7Z_OBJS = C/7zCrc.o C/7zCrcOpt.o C/7zStream.o C/Alloc.o C/CpuArch.o C/LzFind.o C/LzFindMt.o C/LzmaDec.o C/LzmaEnc.o C/LzFindOpt.o C/Threads.o

BROTLI_COMMON_OBJS = brotli-1.1.0/c/common/dictionary.o brotli-1.1.0/c/common/context.o brotli-1.1.0/c/common/constants.o brotli-1.1.0/c/common/transform.o brotli-1.1.0/c/common/platform.o brotli-1.1.0/c/common/shared_dictionary.o

BROTLI_ENC_OBJS = brotli-1.1.0/c/enc/bit_cost.o brotli-1.1.0/c/enc/backward_references_hq.o brotli-1.1.0/c/enc/compress_fragment_two_pass.o brotli-1.1.0/c/enc/command.o brotli-1.1.0/c/enc/memory.o brotli-1.1.0/c/enc/block_splitter.o brotli-1.1.0/c/enc/metablock.o brotli-1.1.0/c/enc/static_dict.o brotli-1.1.0/c/enc/dictionary_hash.o brotli-1.1.0/c/enc/entropy_encode.o brotli-1.1.0/c/enc/encode.o brotli-1.1.0/c/enc/backward_references.o brotli-1.1.0/c/enc/compress_fragment.o brotli-1.1.0/c/enc/cluster.o brotli-1.1.0/c/enc/compound_dictionary.o brotli-1.1.0/c/enc/histogram.o brotli-1.1.0/c/enc/utf8_util.o brotli-1.1.0/c/enc/fast_log.o brotli-1.1.0/c/enc/brotli_bit_stream.o brotli-1.1.0/c/enc/encoder_dict.o brotli-1.1.0/c/enc/literal_cost.o

BROTLI_DEC_OBJS = brotli-1.1.0/c/dec/bit_reader.o brotli-1.1.0/c/dec/decode.o brotli-1.1.0/c/dec/huffman.o brotli-1.1.0/c/dec/state.o

OBJS = $(7Z_OBJS) $(BROTLI_COMMON_OBJS) $(BROTLI_ENC_OBJS) $(BROTLI_DEC_OBJS) main.o

TARGET = janus

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
ifeq ($(OS),Windows_NT)
	-del /Q $(subst /,\,$(OBJS)) $(TARGET).exe 2>nul
else
	$(RM) $(OBJS) $(TARGET)
endif
