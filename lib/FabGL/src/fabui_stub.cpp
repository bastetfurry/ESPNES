// ESPNES: the keyboard and mouse drivers post their events to a fabgl::uiApp when one runs.
// ESPNES has none, and fabui.cpp (FabGL's GUI) does not build with ESP-IDF 5 as it is: this
// stands in for the one function they reference, so the GUI stays out of the firmware.
#include "fabui.h"

namespace fabgl {

bool uiApp::postEvent(uiEvent const * event)
{
  return false;
}

}
