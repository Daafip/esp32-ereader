// bb_epaper compiled for the PC as a RAM-only display (no SPI, no GPIO), for
// rendering UI previews. Skips the Raspberry Pi I/O layer and supplies the
// Linux initIO hook that mem_io.inl lacks in 2.1.11.
#define __LINUX__
#define __MEM_ONLY__
#define __BB_RPI_IO__
#include "bb_epaper.h"
void bbepInitIO(BBEPDISP *, uint32_t) {}
#include "bb_epaper.cpp"
