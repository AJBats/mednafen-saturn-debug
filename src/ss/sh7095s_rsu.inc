 enum : unsigned { which = 1 };
 enum : bool { EmulateICache = true };
 enum : bool { DebugMode = SH7095_DEBUG_MODE };
 enum : bool { CacheBypassHack = false };

#ifdef MDFN_ENABLE_DEV_BUILD
 assert(!ResumeTableP[DebugMode] || DebugMode == DM_Setting);
#endif
 //if(this != &CPU[1])
 // __builtin_unreachable();

 if(MDFN_UNLIKELY(timestamp >= bound_timestamp))
  return;

 if(ResumePoint)
 {
  //printf("Resume: 0x%016llx\n", (unsigned long long)(uintptr_t)ResumePoint);
  const void* const tmp = ResumePoint;
  ResumePoint = nullptr;
  goto *tmp;
 }

 do
 {
  if(DebugMode)
  {
   if(MDFN_LIKELY(ResumeTableP[DebugMode]))
    DBG_CPUHandler<1>();
  }

  //
  // Ideally, we would place SPEPRecover: after the FRT event check, but doing
  // so causes gcc(multiple versions) to produce inconceivably awful code under certain conditions
  // (such as disabling all the SS_DBG stuff at compile-time) because it thinks it's an important loop
  // or something?(even with all our branch hinting!)
  //
  SPEPRecover:;

  if(MDFN_UNLIKELY(timestamp >= FRT_WDT_NextTS))
  {
   FRT_WDT_Update();
   FRT_WDT_Recalc_NET();
  }

  if(MDFN_UNLIKELY(InsnTraceFile != nullptr))
  {
   char dis_buf[64];
   {
    auto peek16 = [](uint32 A) -> uint16 { return *(uint16*)(SH7095_FastMap[A >> SH7095_EXT_MAP_GRAN_BITS] + A); };
    auto peek32 = [](uint32 A) -> uint32 { return ((uint32)*(uint16*)(SH7095_FastMap[A >> SH7095_EXT_MAP_GRAN_BITS] + A) << 16)
     | *(uint16*)(SH7095_FastMap[(A|2) >> SH7095_EXT_MAP_GRAN_BITS] + (A|2)); };
    SH7095::Disassemble((uint16)Pipe_ID, PC - 4 + 4, dis_buf, peek16, peek32);
   }
   size_t dlen = strlen(dis_buf);
   if(dlen < 28) { memset(dis_buf + dlen, ' ', 28 - dlen); dis_buf[28] = 0; }

   char cpu_ch = (InsnTraceFile == CallTraceFile) ? 's' : 'S';
   fprintf(InsnTraceFile, "%u %c %08X %s "
    "R0=%08X R1=%08X R2=%08X R3=%08X R4=%08X R5=%08X R6=%08X R7=%08X "
    "R8=%08X R9=%08X R10=%08X R11=%08X R12=%08X R13=%08X R14=%08X R15=%08X "
    "PR=%08X SR=%08X GBR=%08X MACH=%08X MACL=%08X\n",
    (unsigned)timestamp, cpu_ch, PC - 4, dis_buf,
    R[0], R[1], R[2], R[3], R[4], R[5], R[6], R[7],
    R[8], R[9], R[10], R[11], R[12], R[13], R[14], R[15],
    PR, SR, GBR, MACH, MACL);
  }

  instr = (uint16)Pipe_ID;
  //
  #include "sh7095_ops.inc"
  //
 } while(timestamp < bound_timestamp);

