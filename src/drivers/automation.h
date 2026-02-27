/* automation.h -- File-based automation interface for Mednafen
 *
 * Provides:
 *  - Background window mode (no focus steal)
 *  - File-based command interface (frame advance, screenshot, input, debug)
 *  - SH-2 debug tools (register dump, memory dump, PC trace)
 *  - Instruction-level stepping and PC breakpoints
 *
 * Part of mednafen-saturn-debug fork.
 */

#ifndef __MDFN_DRIVERS_AUTOMATION_H
#define __MDFN_DRIVERS_AUTOMATION_H

#include "main.h"

// Initialize automation subsystem. Call after Video_Init().
// base_dir: directory for action/ack files (e.g. mednafen base dir)
void Automation_Init(const std::string& base_dir);

// Poll for commands. Call once per frame from the game thread (MDFND_Update).
// surface/rect/lw: current framebuffer for screenshot commands
void Automation_Poll(const MDFN_Surface* surface, const MDFN_Rect* rect, const int32* lw);

// Shutdown automation subsystem.
void Automation_Kill(void);

// Check if automation mode is active (--automation flag passed)
bool Automation_IsActive(void);

// Suppress SDL_RaiseWindow and focus grabbing in automation mode
bool Automation_SuppressRaise(void);

// Check if automation wants to override input for a port.
// Automation input is ORed into existing keyboard state (additive, not exclusive).
bool Automation_GetInput(unsigned port, uint8_t* data, unsigned data_size);

// Debug hook called from SH-2 step (master CPU only).
// Checks breakpoints, steps, cycle targets. Spin-waits when paused.
// Always returns false (pause is handled internally via spin-wait).
bool Automation_DebugHook(uint32_t pc);

// Memory watchpoint hit callback -- called from ss.cpp when a write hits the watched address.
// source: "CPU" for CPU writes, "DMA" for SCU DMA writes.
void Automation_WatchpointHit(uint32_t pc, uint32_t addr, uint32_t old_val, uint32_t new_val, uint32_t pr, const char* source);

// Log a Mednafen system command (screenshot, save state, etc.) to the input trace file.
void Automation_LogSystemCommand(const char* cmd_name);

// Window visibility control -- consume pending show/hide requests.
// Returns true (and clears the flag) if a show_window/hide_window command was received.
// Caller should call SDL_ShowWindow/SDL_HideWindow accordingly.
bool Automation_ConsumePendingShowWindow(void);
bool Automation_ConsumePendingHideWindow(void);

#endif
