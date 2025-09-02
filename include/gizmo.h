#ifndef GIZMO_H
#define GIZMO_H

#include "external/tinyfiledialogs.h"

typedef GbcEmu GbcEmu;

bool handle_events(GbcEmu *emu);

void start_emulator(GbcEmu *emu);

#endif