// BMS Unified Firmware - Hub or Controller Mode
// Select mode in platformio.ini with build_flags: -D MODE_HUB or -D MODE_CONTROLLER

// Mode selection - defined in platformio.ini build_flags
// #define MODE_HUB
// #define MODE_CONTROLLER

#if !defined(MODE_HUB) && !defined(MODE_CONTROLLER)
  #error "Please define MODE_HUB or MODE_CONTROLLER in platformio.ini build_flags"
#endif

#if defined(MODE_HUB)
  #include "hub.h"
#elif defined(MODE_CONTROLLER)
  #include "controller.h"
#endif

// Main setup and loop - delegate to mode-specific functions
void setup() {
  #if defined(MODE_HUB)
    hubSetup();
  #elif defined(MODE_CONTROLLER)
    controllerSetup();
  #endif
}

void loop() {
  #if defined(MODE_HUB)
    hubLoop();
  #elif defined(MODE_CONTROLLER)
    controllerLoop();
  #endif
}
