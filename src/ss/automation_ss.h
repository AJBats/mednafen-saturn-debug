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

 // Memory writes (writes to backing store, invalidates cache line)
 void Automation_WriteMem8(uint32 addr, uint8 val);

 // Bulk memory read — copies 'size' bytes from Saturn address space into 'buf'.
 // Uses backing store directly (bypasses cache) for speed on large reads.
 void Automation_ReadMemBlock(uint32 addr, uint8* buf, uint32 size);

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
 uint32 Automation_GetMasterSR(void);
 void Automation_SetMasterSR(uint32 val);

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

 // Code/Data Logging (CDL)
 void Automation_CDLStart(void);
 void Automation_CDLStop(void);
 void Automation_CDLReset(void);
 bool Automation_CDLDump(const char* path);
 bool Automation_CDLIsActive(void);

 // DMA trace logging
 void Automation_EnableDMATrace(const char* path);
 void Automation_DisableDMATrace(void);
 void Automation_LogDMA(int level, uint32 src, uint32 dst, uint32 bytes);

 // Memory write profiling
 void Automation_EnableMemProfile(const char* path, uint32 lo, uint32 hi);
 void Automation_DisableMemProfile(void);
}

#endif
