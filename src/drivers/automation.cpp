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
 *   pause                      - Pause emulation
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

#include <mednafen/mednafen.h>
#include "../video/png.h"

// Saturn-specific accessors — implemented in ss.cpp to avoid header dependencies
namespace MDFN_IEN_SS {
 uint8 Automation_ReadMem8(uint32 addr);
 std::string Automation_DumpRegs(void);
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

// File modification time tracking
static time_t last_action_mtime = 0;

static void write_ack(const std::string& msg)
{
 std::ofstream f(ack_file, std::ios::trunc);
 if (f.is_open()) {
  f << msg << "\n";
  f.close();
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
  write_ack("ok run_to_frame " + std::to_string(n));
 }
 else if (cmd == "run") {
  frames_to_advance = -1;
  run_to_frame_target = -1;
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
  ss << " paused=" << (frames_to_advance == 0 ? "true" : "false");
  ss << " input=0x" << std::hex << input_buttons;
  write_ack(ss.str());
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

 if (st.st_mtime == last_action_mtime)
  return false;

 last_action_mtime = st.st_mtime;

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
 frames_to_advance = -1;  // Start free-running
 last_action_mtime = 0;

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
   write_ack("done frame_advance frame=" + std::to_string(frame_counter));
  }
 }

 // Poll for new commands (every frame)
 check_action_file();
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

bool Automation_DebugHook(uint32_t pc)
{
 // Not wired into SH-2 step loop for now (would add per-instruction overhead).
 // Register/memory dumps work any time via the action file.
 (void)pc;
 return false;
}
