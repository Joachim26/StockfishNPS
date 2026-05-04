// Standalone NNUE embedding for universal binary builds

#include "../evaluate.h"

extern const unsigned char gEmbeddedNNUEData[] = {
#embed EvalFileDefaultName
};
extern const unsigned int gEmbeddedNNUESize = sizeof(gEmbeddedNNUEData);
