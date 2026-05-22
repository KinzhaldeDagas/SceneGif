#pragma once

#include "GameTypes.h"

class OSInputGlobalsEx;

void Screenshot_HandleInput(OSInputGlobalsEx* input);
void Screenshot_Tick();
void __cdecl Screenshot_HandleEngineRequest(UInt32 multiFlag);

bool Screenshot_SetKey(UInt32 keycode);
UInt16 Screenshot_GetKey();

bool Screenshot_SetFormat(const char* format);
const char* Screenshot_GetFormat();
