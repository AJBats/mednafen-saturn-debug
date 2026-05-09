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
 std::string Automation_CallStack(uint32 scan_size);
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

 // Per-CPU prev-PC ring + ISR depth, formatted for BP/WP/RWP hit logs.
 // Writes "prev_pc=0xAA,0xBB,0xCC,0xDD isr_depth=N" into out.
 //   cpu: 0=master, 1=slave (out-of-range clamped to master).
 // BP fires should pass 0 (Automation_DebugHook is master-only).
 // WP fires should pass Automation_GetCurrentBusCPU() to mirror the
 // automation_current_cpu heuristic used by Automation_CallStack.
 // Returns chars written (excl. NUL).
 size_t Automation_FormatPrevPCLine(unsigned cpu, char* out, size_t outsz);

 // Returns the CPU index of the most recent bus accessor (0=master, 1=slave),
 // defaulting to master for DMA/unknown. Matches the heuristic Automation_CallStack
 // uses; intended for watchpoint hit emitters to pick the right prev-PC ring.
 unsigned Automation_GetCurrentBusCPU(void);

 // Compute a 64-bit signature hash of the current control-flow context at CPU
 // `cpu` (isr-active flag + prev_pc[0] + shadow-stack depth + target chain).
 // Used by BP_DEDUPE to drop duplicate fires of the same logical entry-mode
 // on a persistent BP. Out-of-range cpu clamps to master (0).
 uint64 Automation_ComputeBPSignature(unsigned cpu);

 // Call tracing
 void Automation_EnableCallTrace(const char* path);
 void Automation_DisableCallTrace(void);
 void Automation_SetCallTraceFile(FILE* f);
 void Automation_ClearCallTraceFile(void);

 // Memory write watchpoints
 void Automation_SetWatchpoint(uint32 addr);
 void Automation_SetWatchpointFilter(bool active, uint32 value);
 void Automation_ClearWatchpoint(void);
 bool Automation_CheckWatchpointActive(void);
 void Automation_SetVDP2Watchpoint(uint32 lo, uint32 hi, const char* logpath);
 void Automation_ClearVDP2Watchpoint(void);

 // Memory read watchpoints (non-pausing, log-only)
 void Automation_SetReadWatchpoint(uint32 addr);
 void Automation_ClearReadWatchpoint(void);
 bool Automation_CheckReadWatchpointActive(void);

 // Gates for bulk byte-resolution read-WPs (per region). The bitmap
 // storage lives in drivers/automation.cpp; these bools fast-path the
 // inline read checks in BusRW_DB_CS0 (LWR) and BusRW_DB_CS3 (HWR) when
 // their region's bitmap is empty. A sweep loaded only into HWR pays no
 // cost on LWR reads and vice versa.
 void Automation_SetReadWatchpointBulkActive(bool lwr, bool hwr);

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

 // Code/Data Logging (CDL) — configurable address range
 void Automation_CDLStart(uint32 lo, uint32 hi);
 void Automation_CDLStop(void);
 void Automation_CDLReset(void);
 bool Automation_CDLDump(const char* path);
 bool Automation_CDLIsActive(void);
 uint32 Automation_CDLGetLo(void);
 uint32 Automation_CDLGetHi(void);
 uint32 Automation_CDLGetSize(void);

 // Memory read profiling
 void Automation_EnableMemReadProfile(const char* path, uint32 lo, uint32 hi);
 void Automation_DisableMemReadProfile(void);

 // DMA trace logging
 void Automation_EnableDMATrace(const char* path);
 void Automation_DisableDMATrace(void);
 void Automation_LogDMA(int level, uint32 src, uint32 dst, uint32 bytes);

 // Memory write profiling
 void Automation_EnableMemProfile(const char* path, uint32 lo, uint32 hi);
 void Automation_DisableMemProfile(void);
}

#endif
