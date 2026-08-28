/* MtDec.h -- Multi-threading Decoder
2023-04-02 : Igor Pavlov : Public domain */

#ifndef ZIP7_INC_MT_DEC_H
#define ZIP7_INC_MT_DEC_H

#include "7zTypes.h"

EXTERN_C_BEGIN

typedef struct CMtDec_ CMtDec;

struct CMtDec_
{
  ISzAllocPtr alloc;
  Byte *crossBlock;
  size_t inBufSize;
  UInt32 numThreadsMax;

  ISeqInStream *inStream;
  ICompressProgress *progress;

  size_t crossStart;
  size_t crossEnd;
  unsigned numFilledThreads;
};

void MtDec_Construct(CMtDec *p);
void MtDec_Free(CMtDec *p);

EXTERN_C_END

#endif