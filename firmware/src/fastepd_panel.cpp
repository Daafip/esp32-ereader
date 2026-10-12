#include "board.h"

#ifdef BOARD_PANEL_FASTEPD

#include "fastepd_panel.h"

#include <FastEPD.h>
#include <string.h>

// FastEPD 2.2.0 functions with external linkage that its public header doesn't
// declare (FastEPD.inl). FASTEPD::initCustomPanel() only runs the I/O callback;
// the parallel bus and the buffers need these, as in bbepInitPanel().
int bbepIOInit(FASTEPDSTATE *pState);
int bbepSetPanelSize(FASTEPDSTATE *pState, int width, int height, int flags, int iVCOM);
void bbepSendShiftData(FASTEPDSTATE *pState);
// The LilyGo T5 Pro uses the same 74HCT4094 control-line scheme.
int LilyGoEinkPower(void *pBBEP, int bOn);
int LilyGoIOInit(void *pBBEP);
void LilyGoRowControl(void *pBBEP, int iMode);

namespace panel {
namespace {

// FastEPD's u8M5Matrix (Apache-2.0, (c) BitBank Software): the 16-level waveform
// for ED047TC1 panels, used for buffer setup; 1-bpp updates don't use the gray levels.
const uint8_t kWaveform[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 1, 1, 2, 1, 1, 1, 2, 2, 1, 1, 1, 1, 2, 1, 2, 2, 1, 1,
    2, 2, 1, 1, 2, 2, 2, 2, 1, 1, 2, 1, 2, 2, 1, 1, 1, 2, 2, 1, 2, 2, 1, 1, 2, 1, 1, 2,
    2, 2, 2, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 1, 1,
    1, 1, 2, 2, 1, 1, 1, 1, 2, 1, 2, 2, 2, 2, 1, 1, 2, 1, 2, 2, 2, 1, 1, 2, 2, 1, 2, 2,
    2, 2, 1, 2, 2, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
};
static_assert(sizeof(kWaveform) == 16 * 8, "16 gray levels x 8 passes");

class T547 : public FASTEPD {
 public:
  int init() {
    BBPANELDEF &d = _state.panelDef;
    memset(&d, 0, sizeof(d));
    d.width = kEpdNativeW;
    d.height = kEpdNativeH;
    d.bus_speed = kEpdBusHz;
    d.flags = BB_PANEL_FLAG_SLOW_SPH;
    memcpy(d.data, kEpdData, sizeof(kEpdData));
    d.bus_width = 8;
    d.ioPWR = d.ioSPV = BB_NOT_USED;  // power and STV go through the shift register
    d.ioCKV = kEpdCkv;
    d.ioSPH = kEpdSth;
    d.ioCL = kEpdCkh;
    d.ioSDA = kEpdCfgData;
    d.ioSCL = kEpdCfgClk;
    d.ioShiftSTR = kEpdCfgStr;
    d.ioDCDummy = kEpdDummyDc;
    d.pGrayMatrix = kWaveform;
    d.iMatrixSize = sizeof(kWaveform);
    d.iLinePadding = 16;
    d.iVCOM = kEpdVcomMv;

    _state.iPanelType = BB_PANEL_CUSTOM;
    _state.mode = BB_MODE_1BPP;
    _state.iFG = BBEP_BLACK;
    _state.iBG = BBEP_TRANSPARENT;
    _state.width = _state.native_width = d.width;
    _state.height = _state.native_height = d.height;
    _state.iFlags = d.flags;
    _state.pfnEinkPower = LilyGoEinkPower;
    _state.pfnIOInit = LilyGoIOInit;
    _state.pfnRowControl = LilyGoRowControl;
    int rc = bbepIOInit(&_state);
    if (rc == BBEP_SUCCESS) rc = bbepSetPanelSize(&_state, d.width, d.height, d.flags, d.iVCOM);
    if (rc == BBEP_SUCCESS) rc = setMode(BB_MODE_1BPP);  // also selects the pixel functions
    return rc;
  }

  void allOutputsLow() {
    _state.shift_data = 0;
    bbepSendShiftData(&_state);
  }
};

T547 epd;
bool ready = false;

}  // namespace

bool begin() {
  if (!ready) ready = epd.init() == BBEP_SUCCESS;
  return ready;
}

uint8_t *current() { return epd.currentBuffer(); }
uint8_t *previous() { return epd.previousBuffer(); }
void previousValid() { epd.setPreviousMode(BB_MODE_1BPP); }

void update(Refresh mode) {
  if (!ready) return;
  switch (mode) {
    case Refresh::Full: epd.fullUpdate(CLEAR_FAST, false); break;
    case Refresh::Fast: epd.fullUpdate(CLEAR_NONE, false); break;
    case Refresh::Partial: epd.partialUpdate(false); break;
  }
}

void powerOff() {
  if (!ready) return;
  epd.einkPower(0);
  epd.allOutputsLow();
}

}  // namespace panel

#endif  // BOARD_PANEL_FASTEPD
