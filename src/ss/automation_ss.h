/* automation_ss.h -- Saturn-side automation accessors
 *
 * Shared interface between drivers/automation.cpp and ss/ss.cpp.
 * Functions are defined in ss.cpp within namespace MDFN_IEN_SS.
 *
 * Part of mednafen-saturn-debug fork.
 */

#ifndef __MDFN_SS_AUTOMATION_SS_H
#define __MDFN_SS_AUTOMATION_SS_H

#include <mednafen/types.h>
#include <string>
#include <cstdio>

namespace MDFN_IEN_SS {
 // Memory reads (cache-aware for instruction cache)
 uint8 Automation_ReadMem8(uint32 addr);

 // Register dumps
 std::string Automation_DumpRegs(void);
 void Automation_DumpRegsBin(const char* path);
 std::string Automation_DumpSlaveRegs(void);
 void Automation_DumpSlaveRegsBin(const char* path);
 void Automation_DumpVDP2RegsBin(const char* path);

 // CPU hook control
 void Automation_EnableCPUHook(void);
 void Automation_DisableCPUHook(void);
 uint32 Automation_GetMasterPC(void);
 int64_t Automation_GetMasterCycle(void);

 // Call tracing
 void Automation_EnableCallTrace(const char* path);
 void Automation_DisableCallTrace(void);
 void Automation_SetCallTraceFile(FILE* f);
 void Automation_ClearCallTraceFile(void);

 // Memory watchpoints
 void Automation_SetWatchpoint(uint32 addr);
 void Automation_ClearWatchpoint(void);
 bool Automation_CheckWatchpointActive(void);
 void Automation_SetVDP2Watchpoint(uint32 lo, uint32 hi, const char* logpath);
 void Automation_ClearVDP2Watchpoint(void);

 // CD Block tracing
 void CDB_EnableSCDQTrace(const char* path);
 void CDB_DisableSCDQTrace(void);
 void CDB_EnableCDBTrace(const char* path);
 void CDB_DisableCDBTrace(void);
 void CDB_SetCDBTraceFile(FILE* f);
 void CDB_ClearCDBTraceFile(void);

 // Deterministic mode
 void Automation_SetDeterministic(void);

 // Per-instruction tracing
 void Automation_EnableInsnTrace(const char* path, int64_t start_line, int64_t stop_line);
 void Automation_EnableInsnTraceUnified(int64_t start_line, int64_t stop_line);
 void Automation_DisableInsnTrace(void);
}

#endif
