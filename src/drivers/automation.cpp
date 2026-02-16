/* automation.cpp -- File-based automation interface for Mednafen
 *
 * Protocol:
 *   External tool writes commands to <base_dir>/mednafen_action.txt
 *   Mednafen executes command, writes result to <base_dir>/mednafen_ack.txt
 *   External tool reads ack, writes next command.
 *
 * Commands:
 *   frame_advance [N]          - Run N frames then pause (default 1)
 *   screenshot <path>          - Save framebuffer to PNG at given path
 *   input <button>             - Press button (START, A, B, C, X, Y, Z, UP, DOWN, LEFT, RIGHT, L, R)
 *   input_release <button>     - Release button
 *   input_clear                - Release all buttons
 *   run_to_frame <N>           - Run until frame N then pause
 *   quit                       - Clean shutdown
 *   dump_regs                  - Dump SH-2 master CPU registers
 *   dump_mem <addr> <size>     - Dump memory (hex), addr in hex
 *   status                     - Report current frame, pause state, etc.
 *   run                        - Free-run (unpause)
 *   pause                      - Pause emulation (blocking)
 *   dump_regs_bin <path>       - Write 22 uint32s (R0-R15,PC,SR,PR,GBR,VBR,MACH) to binary file
 *   dump_mem_bin <addr> <sz> <path> - Write raw memory bytes to binary file
 *   pc_trace_frame <path>      - Trace all master CPU PCs for 1 frame to binary file
 *   show_window                - Make the emulator window visible (for visual inspection)
 *   hide_window                - Hide the emulator window again
 *   step [N]                   - Step N CPU instructions then pause (default 1)
 *   breakpoint <addr>          - Add PC breakpoint (hex address)
 *   breakpoint_clear           - Remove all breakpoints
 *   breakpoint_list            - List active breakpoints
 *   continue                   - Resume execution (until next breakpoint or frame end)
 *
 * Part of mednafen-saturn-debug fork.
 */

#include "automation.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <time.h>
#ifdef WIN32
#include <windows.h>
#endif

#include <mednafen/mednafen.h>
#include "../video/png.h"

// Saturn-specific accessors — implemented in ss.cpp to avoid header dependencies
namespace MDFN_IEN_SS {
 uint8 Automation_ReadMem8(uint32 addr);
 std::string Automation_DumpRegs(void);
 void Automation_DumpRegsBin(const char* path);
 void Automation_EnableCPUHook(void);
 void Automation_DisableCPUHook(void);
 uint32 Automation_GetMasterPC(void);
}

static bool automation_active = false;
static std::string action_file;
static std::string ack_file;
static std::string auto_base_dir;

// Frame counter
static uint64_t frame_counter = 0;
static int64_t frames_to_advance = -1;  // -1 = free-running, 0 = paused, >0 = counting down
static int64_t run_to_frame_target = -1;

// Input state
// Bit layout matches Mednafen's IDII array order for Saturn digital gamepad:
//   data[0]: Z(0) Y(1) X(2) R(3) UP(4) DOWN(5) LEFT(6) RIGHT(7)
//   data[1]: B(0) C(1) A(2) START(3) pad(4) pad(5) pad(6) L(7)
enum SaturnButton {
 BTN_Z      = 0,
 BTN_Y      = 1,
 BTN_X      = 2,
 BTN_R      = 3,
 BTN_UP     = 4,
 BTN_DOWN   = 5,
 BTN_LEFT   = 6,
 BTN_RIGHT  = 7,
 BTN_B      = 8,
 BTN_C      = 9,
 BTN_A      = 10,
 BTN_START  = 11,
 // 12-14 are padding
 BTN_L      = 15,
};

static uint16_t input_buttons = 0;  // bitmask of pressed buttons
static bool input_override = false;

// Pending screenshot
static std::string pending_screenshot_path;

// Pending window visibility changes
static bool pending_show_window = false;
static bool pending_hide_window = false;

// PC trace state
static FILE* pc_trace_file = nullptr;
static bool pc_trace_active = false;
static bool pc_trace_frame_mode = false;

// Instruction stepping state
static int64_t instructions_to_step = -1;  // -1=not stepping, 0=step done, >0=counting
static bool instruction_paused = false;     // true when spin-waiting inside debug hook

// Breakpoint list
static std::vector<uint32_t> breakpoints;

// Track whether the CPU debug hook is currently enabled
static bool cpu_hook_active = false;

// Monotonic sequence counter — ensures every ack is unique even if PC/frame are same
static uint64_t ack_seq = 0;

// File modification time tracking
// On Linux, st_mtim gives nanosecond precision. On Windows, only seconds — so we
// also track file size to detect rapid successive writes within the same second.
static time_t last_action_mtime = 0;
static off_t last_action_size = 0;

static void write_ack(const std::string& msg)
{
 std::ofstream f(ack_file, std::ios::trunc);
 if (f.is_open()) {
  f << msg << "\n";
  f.close();
 }
}

// Enable/disable the SH-2 CPU debug hook based on what features need it.
// Called after any change to pc_trace, stepping, or breakpoint state.
static void update_cpu_hook(void)
{
 bool need = pc_trace_active || (instructions_to_step >= 0) || !breakpoints.empty();
 if (need && !cpu_hook_active) {
  MDFN_IEN_SS::Automation_EnableCPUHook();
  cpu_hook_active = true;
 } else if (!need && cpu_hook_active) {
  MDFN_IEN_SS::Automation_DisableCPUHook();
  cpu_hook_active = false;
 }
}

static int parse_button(const char* name)
{
 // Case-insensitive compare helper (avoids platform-specific strcasecmp)
 auto eq = [](const char* a, const char* b) -> bool {
  while (*a && *b) {
   if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
   a++; b++;
  }
  return *a == *b;
 };

 if (eq(name, "START")) return BTN_START;
 if (eq(name, "A")) return BTN_A;
 if (eq(name, "B")) return BTN_B;
 if (eq(name, "C")) return BTN_C;
 if (eq(name, "X")) return BTN_X;
 if (eq(name, "Y")) return BTN_Y;
 if (eq(name, "Z")) return BTN_Z;
 if (eq(name, "L")) return BTN_L;
 if (eq(name, "R")) return BTN_R;
 if (eq(name, "UP")) return BTN_UP;
 if (eq(name, "DOWN")) return BTN_DOWN;
 if (eq(name, "LEFT")) return BTN_LEFT;
 if (eq(name, "RIGHT")) return BTN_RIGHT;
 return -1;
}

static void dump_registers(void)
{
 write_ack(MDFN_IEN_SS::Automation_DumpRegs());
}

static void dump_memory(uint32_t addr, uint32_t size)
{
 // Clamp size to avoid huge dumps
 if (size > 0x10000) size = 0x10000;

 std::ostringstream ss;
 char buf[16];
 ss << "mem ";
 snprintf(buf, sizeof(buf), "%08X", addr);
 ss << buf;

 for (uint32_t i = 0; i < size; i++) {
  if ((i & 15) == 0) ss << "\n";
  snprintf(buf, sizeof(buf), "%02X ", MDFN_IEN_SS::Automation_ReadMem8(addr + i));
  ss << buf;
 }

 write_ack(ss.str());
}

static void do_screenshot(const MDFN_Surface* surface, const MDFN_Rect* rect, const int32* lw)
{
 if (pending_screenshot_path.empty() || !surface || !rect)
  return;

 try {
  PNGWrite(pending_screenshot_path, surface, *rect, lw);
  write_ack("ok screenshot " + pending_screenshot_path);
 } catch(std::exception& e) {
  write_ack(std::string("error screenshot: ") + e.what());
 }

 pending_screenshot_path.clear();
}

static void process_command(const std::string& line)
{
 if (line.empty() || line[0] == '#')
  return;

 std::istringstream iss(line);
 std::string cmd;
 iss >> cmd;

 if (cmd == "frame_advance") {
  int64_t n = 1;
  iss >> n;
  if (n < 1) n = 1;
  frames_to_advance = n;
  instruction_paused = false;   // unblock instruction-level pause
  instructions_to_step = -1;    // cancel step mode
  update_cpu_hook();
  write_ack("ok frame_advance " + std::to_string(n));
 }
 else if (cmd == "screenshot") {
  std::string path;
  iss >> path;
  if (path.empty()) {
   write_ack("error screenshot: no path specified");
  } else {
   pending_screenshot_path = path;
   write_ack("ok screenshot_queued " + path);
  }
 }
 else if (cmd == "input") {
  std::string button;
  iss >> button;
  int b = parse_button(button.c_str());
  if (b >= 0) {
   input_buttons |= (1 << b);
   input_override = true;
   write_ack("ok input " + button);
  } else {
   write_ack("error input: unknown button " + button);
  }
 }
 else if (cmd == "input_release") {
  std::string button;
  iss >> button;
  int b = parse_button(button.c_str());
  if (b >= 0) {
   input_buttons &= ~(1 << b);
   if (input_buttons == 0) input_override = false;
   write_ack("ok input_release " + button);
  } else {
   write_ack("error input_release: unknown button " + button);
  }
 }
 else if (cmd == "input_clear") {
  input_buttons = 0;
  input_override = false;
  write_ack("ok input_clear");
 }
 else if (cmd == "run_to_frame") {
  int64_t n = 0;
  iss >> n;
  run_to_frame_target = n;
  frames_to_advance = -1;  // free-run until target
  instruction_paused = false;
  instructions_to_step = -1;
  update_cpu_hook();
  write_ack("ok run_to_frame " + std::to_string(n));
 }
 else if (cmd == "run") {
  frames_to_advance = -1;
  run_to_frame_target = -1;
  instruction_paused = false;
  instructions_to_step = -1;
  update_cpu_hook();
  write_ack("ok run");
 }
 else if (cmd == "pause") {
  frames_to_advance = 0;
  write_ack("ok pause frame=" + std::to_string(frame_counter));
 }
 else if (cmd == "quit") {
  write_ack("ok quit");
  MainRequestExit();
 }
 else if (cmd == "dump_regs") {
  dump_registers();
 }
 else if (cmd == "dump_mem") {
  uint32_t addr = 0, size = 256;
  iss >> std::hex >> addr >> size;
  dump_memory(addr, size);
 }
 else if (cmd == "status") {
  std::ostringstream ss;
  ss << "status frame=" << frame_counter;
  ss << " paused=" << ((frames_to_advance == 0 || instruction_paused) ? "true" : "false");
  ss << " inst_paused=" << (instruction_paused ? "true" : "false");
  ss << " breakpoints=" << breakpoints.size();
  ss << " input=0x" << std::hex << input_buttons;
  write_ack(ss.str());
 }
 else if (cmd == "dump_regs_bin") {
  std::string path;
  iss >> path;
  if (path.empty()) {
   write_ack("error dump_regs_bin: no path");
  } else {
   MDFN_IEN_SS::Automation_DumpRegsBin(path.c_str());
   write_ack("ok dump_regs_bin " + path);
  }
 }
 else if (cmd == "dump_mem_bin") {
  uint32_t addr = 0, size = 0;
  std::string path;
  iss >> std::hex >> addr >> size >> path;
  if (path.empty() || size == 0) {
   write_ack("error dump_mem_bin: need addr size path");
  } else {
   if (size > 0x100000) size = 0x100000;
   FILE* f = fopen(path.c_str(), "wb");
   if (f) {
    for (uint32_t i = 0; i < size; i++) {
     uint8_t b = MDFN_IEN_SS::Automation_ReadMem8(addr + i);
     fwrite(&b, 1, 1, f);
    }
    fclose(f);
    write_ack("ok dump_mem_bin");
   } else {
    write_ack("error dump_mem_bin: cannot open " + path);
   }
  }
 }
 else if (cmd == "pc_trace_frame") {
  std::string path;
  iss >> path;
  if (path.empty()) {
   write_ack("error pc_trace_frame: no path");
  } else {
   pc_trace_file = fopen(path.c_str(), "wb");
   if (!pc_trace_file) {
    write_ack("error pc_trace_frame: cannot open " + path);
   } else {
    pc_trace_active = true;
    pc_trace_frame_mode = true;
    frames_to_advance = 1;
    update_cpu_hook();
    write_ack("ok pc_trace_frame_started");
   }
  }
 }
 else if (cmd == "step") {
  int64_t n = 1;
  iss >> n;
  if (n < 1) n = 1;
  instructions_to_step = n;
  instruction_paused = false;  // unblock instruction-level pause if active
  // Unblock frame-level pause — the CPU hook will pause us after N instructions
  if (frames_to_advance == 0)
   frames_to_advance = -1;
  update_cpu_hook();
  write_ack("ok step " + std::to_string(n));
 }
 else if (cmd == "breakpoint") {
  uint32_t addr = 0;
  iss >> std::hex >> addr;
  breakpoints.push_back(addr);
  update_cpu_hook();
  char buf[32];
  snprintf(buf, sizeof(buf), "0x%08X", addr);
  write_ack(std::string("ok breakpoint ") + buf + " total=" + std::to_string(breakpoints.size()));
 }
 else if (cmd == "breakpoint_clear") {
  size_t count = breakpoints.size();
  breakpoints.clear();
  update_cpu_hook();
  write_ack("ok breakpoint_clear removed=" + std::to_string(count));
 }
 else if (cmd == "breakpoint_list") {
  std::ostringstream ss;
  ss << "breakpoints count=" << breakpoints.size();
  for (size_t i = 0; i < breakpoints.size(); i++) {
   char buf[16];
   snprintf(buf, sizeof(buf), " 0x%08X", breakpoints[i]);
   ss << buf;
  }
  write_ack(ss.str());
 }
 else if (cmd == "continue") {
  instruction_paused = false;  // unblock instruction-level pause
  instructions_to_step = -1;   // no step counting
  // Unblock frame-level pause — will run until breakpoint or next pause command
  if (frames_to_advance == 0)
   frames_to_advance = -1;
  update_cpu_hook();
  write_ack("ok continue");
 }
 else if (cmd == "show_window") {
  pending_show_window = true;
  write_ack("ok show_window");
 }
 else if (cmd == "hide_window") {
  pending_hide_window = true;
  write_ack("ok hide_window");
 }
 else {
  write_ack("error unknown command: " + cmd);
 }
}

static bool check_action_file(void)
{
 struct stat st;
 if (stat(action_file.c_str(), &st) != 0)
  return false;

 // Detect changes via mtime + size (size handles rapid writes within same second on Windows)
 if (st.st_mtime == last_action_mtime && st.st_size == last_action_size)
  return false;

 last_action_mtime = st.st_mtime;
 last_action_size = st.st_size;

 // Read and process all commands
 std::ifstream f(action_file);
 if (!f.is_open())
  return false;

 std::string line;
 while (std::getline(f, line)) {
  // Strip \r (Windows line endings)
  if (!line.empty() && line.back() == '\r')
   line.pop_back();
  process_command(line);
 }
 f.close();

 return true;
}


// === Public API ===

void Automation_Init(const std::string& dir)
{
 auto_base_dir = dir;
 action_file = dir + "/mednafen_action.txt";
 ack_file = dir + "/mednafen_ack.txt";
 automation_active = true;
 frame_counter = 0;
 frames_to_advance = 0;  // Start PAUSED — external tool must send "run" or "frame_advance"
 last_action_mtime = 0;
 last_action_size = 0;

 // Write initial ack so external tools know we're ready
 write_ack("ready frame=0");

 fprintf(stderr, "Automation: initialized\n");
 fprintf(stderr, "  Action file: %s\n", action_file.c_str());
 fprintf(stderr, "  Ack file:    %s\n", ack_file.c_str());
}

void Automation_Poll(void* surface, void* rect, void* lw)
{
 if (!automation_active)
  return;

 frame_counter++;

 // Handle pending screenshot with actual framebuffer
 if (!pending_screenshot_path.empty() && surface && rect) {
  do_screenshot(
   static_cast<const MDFN_Surface*>(surface),
   static_cast<const MDFN_Rect*>(rect),
   static_cast<const int32*>(lw)
  );
 }

 // Check run_to_frame
 if (run_to_frame_target >= 0 && (int64_t)frame_counter >= run_to_frame_target) {
  frames_to_advance = 0;  // Pause
  run_to_frame_target = -1;
  write_ack("done run_to_frame frame=" + std::to_string(frame_counter));
 }

 // Handle frame advance countdown
 if (frames_to_advance > 0) {
  frames_to_advance--;
  if (frames_to_advance == 0) {
   // If tracing a frame, close trace and disable hook
   if (pc_trace_frame_mode && pc_trace_file) {
    fclose(pc_trace_file);
    pc_trace_file = nullptr;
    pc_trace_active = false;
    pc_trace_frame_mode = false;
    update_cpu_hook();
    write_ack("done pc_trace_frame frame=" + std::to_string(frame_counter));
   } else {
    write_ack("done frame_advance frame=" + std::to_string(frame_counter));
   }
  }
 }

 // Poll for new commands (every frame)
 check_action_file();

 // Block emulation when paused — spin-wait until a command unpauses us.
 // This prevents the emulator from running ahead while the orchestrator
 // reads acks and sends new commands.
 while (frames_to_advance == 0 && automation_active) {
#ifdef WIN32
  Sleep(10); // 10ms
#else
  struct timespec ts = {0, 10000000}; // 10ms
  nanosleep(&ts, NULL);
#endif
  check_action_file();
 }
}

void Automation_Kill(void)
{
 if (automation_active) {
  write_ack("shutdown frame=" + std::to_string(frame_counter));
  automation_active = false;
 }
}

bool Automation_IsActive(void)
{
 return automation_active;
}

bool Automation_SuppressRaise(void)
{
 return automation_active;
}

bool Automation_GetInput(unsigned port, uint8_t* data, unsigned data_size)
{
 if (!automation_active || !input_override || port != 0)
  return false;

 // OR automation button presses into the existing data.
 // The Saturn digital pad uses 2 bytes, bits set = pressed in Mednafen's format.
 if (data_size >= 2) {
  data[0] |= (uint8_t)(input_buttons & 0xFF);
  data[1] |= (uint8_t)((input_buttons >> 8) & 0xFF);
 }

 return true;
}

bool Automation_ConsumePendingShowWindow(void)
{
 if (pending_show_window) {
  pending_show_window = false;
  return true;
 }
 return false;
}

bool Automation_ConsumePendingHideWindow(void)
{
 if (pending_hide_window) {
  pending_hide_window = false;
  return true;
 }
 return false;
}

bool Automation_DebugHook(uint32_t pc)
{
 // PC trace — record every instruction's PC to file
 if (pc_trace_active && pc_trace_file) {
  fwrite(&pc, 4, 1, pc_trace_file);
 }

 // Check breakpoints
 bool bp_hit = false;
 for (size_t i = 0; i < breakpoints.size(); i++) {
  if (pc == breakpoints[i]) {
   bp_hit = true;
   break;
  }
 }

 // Instruction step countdown
 if (instructions_to_step > 0)
  instructions_to_step--;

 // Determine if we should pause
 bool should_pause = bp_hit || (instructions_to_step == 0);
 if (!should_pause)
  return false;

 // Pause at instruction level
 instruction_paused = true;
 instructions_to_step = -1;

 // The `pc` parameter from the debug hook is the instruction decode address (PC_ID).
 // CPU[0].PC is the pipeline fetch address (typically 4 bytes ahead on SH-2).
 // For breakpoint hits, `pc` is guaranteed correct (it matched the breakpoint).
 // For step completion, use CPU[0].PC as the authoritative value.
 uint32_t real_pc = MDFN_IEN_SS::Automation_GetMasterPC();
 ack_seq++;

 char msg[256];
 if (bp_hit)
  snprintf(msg, sizeof(msg), "break pc=0x%08X addr=0x%08X frame=%llu seq=%llu",
   pc, pc, (unsigned long long)frame_counter, (unsigned long long)ack_seq);
 else
  snprintf(msg, sizeof(msg), "done step pc=0x%08X frame=%llu seq=%llu",
   real_pc, (unsigned long long)frame_counter, (unsigned long long)ack_seq);
 write_ack(msg);

 // Spin-wait for commands while paused at instruction level.
 // This blocks the SH-2 CPU loop. Commands like dump_regs, dump_mem,
 // step, continue, breakpoint all work during this pause because
 // check_action_file -> process_command handles them.
 while (instruction_paused && automation_active) {
#ifdef WIN32
  Sleep(10);
#else
  struct timespec ts = {0, 10000000}; // 10ms
  nanosleep(&ts, NULL);
#endif
  check_action_file();
 }

 return false;
}
