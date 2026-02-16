/* automation.h -- File-based automation interface for Mednafen
 *
 * Provides:
 *  - Background window mode (no focus steal)
 *  - File-based command interface (frame advance, screenshot, input, debug)
 *  - SH-2 debug tools (register dump, memory dump, breakpoints, PC trace)
 *
 * Part of mednafen-saturn-debug fork for Daytona USA reverse engineering.
 */

#ifndef __MDFN_DRIVERS_AUTOMATION_H
#define __MDFN_DRIVERS_AUTOMATION_H

#include "main.h"

// Initialize automation subsystem. Call after Video_Init().
// base_dir: directory for action/ack files (e.g. mednafen base dir)
void Automation_Init(const std::string& base_dir);

// Poll for commands. Call once per frame from the game thread (MDFND_Update).
// surface/rect/lw: current framebuffer for screenshot commands
void Automation_Poll(void* surface, void* rect, void* lw);

// Shutdown automation subsystem.
void Automation_Kill(void);

// Check if automation mode is active (--automation flag passed)
bool Automation_IsActive(void);

// T1: Suppress SDL_RaiseWindow and focus grabbing
bool Automation_SuppressRaise(void);

// T2: Check if automation wants to override input for a port
// Returns true if automation is providing input, fills data
bool Automation_GetInput(unsigned port, uint8_t* data, unsigned data_size);

// T3: Debug hook called from SH-2 step (master CPU only)
// Returns true if execution should pause (breakpoint hit)
bool Automation_DebugHook(uint32_t pc);

#endif
