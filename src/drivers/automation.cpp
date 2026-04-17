/* automation.cpp -- File-based automation interface for Mednafen
 *
 * Protocol:
 *   External tool writes commands to <base_dir>/mednafen_action.txt
 *   Mednafen executes command, writes result to <base_dir>/mednafen_ack.txt
 *   External tool reads ack, writes next command.
 *
 * Commands:
 *   frame_advance [N]          - Run N frames then pause (default 1)
 *   screenshot <path>          - Save cached framebuffer to PNG (no frame advance, no PC movement)
 *   input <button>             - Press button (START, A, B, C, X, Y, Z, UP, DOWN, LEFT, RIGHT, L, R)
 *   input_release <button>     - Release button
 *   input_clear                - Release all buttons
 *   run_to_frame <N>           - Run until frame N then pause
 *   quit                       - Clean shutdown
 *   dump_regs                  - Dump SH-2 master CPU registers (text: 23 values incl MACL)
 *   dump_regs_bin <path>       - Write 22 uint32s (R0-R15,PC,SR,PR,GBR,VBR,MACH) to binary file
 *   dump_slave_regs            - Dump SH-2 slave CPU registers (text)
 *   dump_slave_regs_bin <path> - Write 22 uint32s for slave SH-2
 *   dump_mem <addr> <size>     - Dump memory (hex), addr in hex, max 4KB text (use dump_mem_bin for larger)
 *   dump_mem_bin <addr> <sz> <path> - Write raw memory bytes to binary file (max 1MB)
 *   poke <addr> <b0> [b1 ...]    - Write bytes to memory (hex addr, hex bytes). Updates cache.
 *   dump_region <name> <path>  - Dump named region: wram_high wram_low vdp1_vram vdp2_vram vdp2_cram sound_ram
 *   dump_vdp2_regs <path>      - Write VDP2 register state to binary file
 *   dump_cycle                 - Report current absolute master cycle count
 *   run_to_cycle <N>           - Run until master cycle count reaches N
 *   pc_trace_frame <path>      - Trace all master CPU PCs for 1 frame to binary file
 *   show_window                - Make the emulator window visible (for visual inspection)
 *   hide_window                - Hide the emulator window again
 *   step [N]                   - Step N CPU instructions then pause (default 1)
 *   breakpoint <addr> [log]    - Add PC breakpoint (hex address). "log" = log-only (no pause),
 *                                 writes full context (regs + call stack) to breakpoint_hits.txt
 *   breakpoint_remove <addr>   - Remove specific PC breakpoint
 *   breakpoint_clear           - Remove all breakpoints
 *   breakpoint_list            - List active breakpoints
 *   poke_breakpoint <trigger_pc> <n> <addr>:<val>:<width> ...
 *                              - On each hit at trigger_pc, write <n> pokes (atomic)
 *                                and continue without pausing. width is bits (8/16/32).
 *   poke_breakpoint_remove <trigger_pc> - Remove one poke trigger
 *   poke_breakpoint_clear      - Remove all poke triggers + any playback
 *   poke_breakpoint_list       - List all poke triggers
 *   poke_playback_start <trigger_pc> <base_addr> <start_row> <end_row> <on_end>
 *                       <n_cols> <col1> ... <colN> <csv_path>
 *                              - CSV-driven playback: each hit pokes one row's worth
 *                                of columns at base_addr+offset, then advances.
 *                                on_end: halt|loop|hold. end_row=-1 means EOF.
 *                                col spec: <csv_name>:<offset_hex>:<width>[:<slice_lo>:<slice_hi>]
 *                                csv_name matches the header cell (no spaces in name).
 *                                byte_slice extracts bytes [lo..hi) from a BE32 render of
 *                                the cell (for narrow fields packed inside a wider column).
 *   poke_playback_stop         - Stop playback and remove its trigger
 *   poke_playback_status       - Report row cursor, trigger hits, poke count
 *   call_trace <path>          - Start logging JSR/BSR/BSRF calls to text file
 *   call_trace_stop            - Stop call trace logging
 *   unified_trace <path>       - Combined call trace + CD Block events
 *   unified_trace_stop         - Stop unified trace
 *   insn_trace <path> <start> <stop> - Per-instruction trace to file
 *   insn_trace_unified <start> <stop> - Per-instruction trace into unified trace
 *   insn_trace_stop            - Stop instruction trace
 *   scdq_trace <path>          - Start logging SCDQ events
 *   scdq_trace_stop            - Stop SCDQ trace
 *   cdb_trace <path>           - Start logging CD Block events
 *   cdb_trace_stop             - Stop CD Block trace
 *   input_trace <path>         - Log real keyboard button presses/releases with frame numbers
 *   input_trace_stop           - Stop input trace logging
 *   input_playback <path>      - Replay recorded input trace (events injected at correct frames)
 *   input_playback_stop        - Stop input playback
 *   call_stack [scan_size]     - Heuristic SH-2 call stack (scans stack for return addresses)
 *   watchpoint <addr> [eq <val>] [log] - Break on memory write to addr (hex), reports PC+old+new value
 *                                 Optional: "eq <val>" only fires when new value == val
 *                                 Optional: "log" = log-only (no pause), writes full context to watchpoint_hits.txt
 *   watchpoint_clear           - Remove memory watchpoint
 *   read_watchpoint <addr> [log] - Break on memory read from addr (hex), reports PC+value. Pauses with
 *                                 full register dump + call stack, same as write watchpoints.
 *                                 Also logs all hits to read_watchpoint_hits.txt.
 *                                 Optional: "log" = log-only (no pause), writes full context to log file.
 *   read_watchpoint_clear      - Remove read watchpoint and resume if paused
 *   exception_break <mode>    - Control SH-2 exception reporting (enable=pause, log=log-only, disable=off)
 *                               Catches: address errors, illegal instructions, slot illegal, NMI.
 *                               Reports type, PC, SR, VBR, handler address + full register dump + call stack.
 *   vdp2_watchpoint <lo> <hi> <path> - Watch VDP2 address range
 *   vdp2_watchpoint_clear      - Remove VDP2 watchpoint
 *   cdl_start [lo hi]           - Start Code/Data Logging for address range [lo,hi)
 *                                 Defaults to HWR (0x06000000–0x06100000) if no args.
 *                                 Example: cdl_start 00200000 00300000 (LWR, 1MB)
 *   cdl_stop                    - Stop CDL (preserves bitmap)
 *   cdl_reset                   - Clear CDL bitmap without stopping
 *   cdl_dump <path>             - Dump CDL bitmap to file (8-byte header: lo+hi, then bitmap)
 *   cdl_status                  - Report CDL active state
 *   dma_trace <path>            - Start logging SCU DMA transfers to text file
 *   dma_trace_stop              - Stop DMA trace logging
 *   mem_profile <lo> <hi> <path> - Log writes to address range [lo,hi] to text file
 *   mem_profile_stop            - Stop memory write profiling
 *   mem_read_profile <lo> <hi> <path> - Log CPU reads in address range to text file (pc, pr, addr, sz)
 *   mem_read_profile_stop       - Stop memory read profiling
 *   mem_sample <addr> <sz> <frames> <path> - Dump memory region every frame for N frames to binary file
 *   mem_sample_stop             - Abort memory sampling early
 *   save_state <path>           - Save full emulator state to file
 *   load_state <path>           - Load emulator state from file
 *   deterministic              - Enable deterministic mode (fixed RTC seed)
 *   status                     - Report current frame, pause state, etc.
 *   run                        - Free-run (unpause)
 *   pause                      - Pause emulation (blocking)
 *
 * All ack responses include cycle=N seq=M appended by write_ack().
 * cycle= is absolute master SH-2 cycle count (int64); seq= is monotonic for change detection.
 *
 * Part of mednafen-saturn-debug fork.
 */

#include "automation.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <fstream>
#include "../MemoryStream.h"
#include "../compress/GZFileStream.h"
#include "../endian.h"
#include <sstream>
#include <sys/stat.h>
#include <time.h>
#ifdef WIN32
#include <windows.h>
#endif

#include <mednafen/mednafen.h>
#include <mednafen/state.h>
#include <mednafen/FileStream.h>
#include "../video/png.h"
#include "../ss/automation_ss.h"
#include "video.h"

static FILE* unified_trace_file = nullptr;
static bool automation_active = false;

// Check if the main thread received a quit request (SDL_QUIT from window
// close, etc.) so spin-wait loops can exit cleanly.  The main thread's
// PumpWrap() handles SDL events and sets NeedExitNow; we just check it
// here since SDL_PumpEvents must only be called from the main thread.
static void check_exit_requested(void)
{
 if (MainExitPending())
  automation_active = false;
}
static std::string action_file;
static std::string ack_file;
static std::string auto_base_dir;

// Frame counter
static uint64_t frame_counter = 0;
static int64_t frames_to_advance = -1;  // -1 = free-running, 0 = paused, >0 = counting down
static int64_t run_to_frame_target = -1;

// Per-frame memory sampler: dumps a memory region every frame to binary file
static FILE*    mem_sample_file = nullptr;
static uint32_t mem_sample_addr = 0;
static uint32_t mem_sample_size = 0;
static int64_t  mem_sample_frames = 0;     // frames remaining
static int64_t  mem_sample_total = 0;      // total frames requested

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

// Automation input is ADDITIVE (ORed into keyboard state).
// It does not suppress real keyboard input -- both sources contribute.
static uint16_t input_buttons = 0;  // bitmask of pressed buttons
static bool input_override = false;

// Cached framebuffer for instant screenshots (no frame advance needed).
// Updated every frame in Automation_Poll with a full copy of the pixel data.
// Saturn max: 704x576x4 = ~1.5MB — trivial.
static uint32_t* cached_fb_pixels = nullptr;
static int32* cached_fb_lw = nullptr;
static MDFN_Rect cached_fb_rect;
static MDFN_PixelFormat cached_fb_format;
static int32 cached_fb_w = 0, cached_fb_h = 0, cached_fb_pitch = 0;
static bool cached_fb_valid = false;

// Pending window visibility changes
// show_window/hide_window now call Video_Automation*Window() directly (no pending flag)

// PC trace state
static FILE* pc_trace_file = nullptr;
static bool pc_trace_active = false;
static bool pc_trace_frame_mode = false;

// Instruction stepping state
static int64_t instructions_to_step = -1;  // -1=not stepping, 0=step done, >0=counting
static bool instruction_paused = false;     // true when spin-waiting inside debug hook

// Breakpoint set (O(1) lookup, deduplicates automatically)
static std::unordered_set<uint32_t> breakpoints;

// Track whether the CPU debug hook is currently enabled
static bool cpu_hook_active = false;

// Cycle-based stopping
static int64_t run_to_cycle_target = -1;  // -1 = not active

// Memory watchpoint state
static bool watchpoint_active = false;
static uint32_t watchpoint_addr = 0;
static bool watchpoint_paused = false;  // true when paused on watchpoint hit
static bool watchpoint_log_mode = false; // true = log-only (no pause), false = pause on hit
static FILE* wp_log = nullptr;          // watchpoint hit log file
static bool watchpoint_filter_active = false;  // conditional: only fire on specific value
static uint32_t watchpoint_filter_value = 0;   // value to match (when filter active)

// Read watchpoint state — same pattern as write watchpoints
static bool read_watchpoint_active = false;
static uint32_t read_watchpoint_addr = 0;
static bool read_watchpoint_paused = false;
static bool read_watchpoint_log_mode = false; // true = log-only (no pause)
static FILE* rwp_log = nullptr;

// Exception break state
// "enable" = pause on exception (like breakpoints)
// "log"    = log to file without pausing
// "disable" = let BIOS handle it (default)
static enum { EXC_DISABLE, EXC_ENABLE, EXC_LOG } exception_mode = EXC_DISABLE;
static bool exception_paused = false;
static FILE* exc_log = nullptr;

// Breakpoint logging state
static bool breakpoint_log_mode = false;  // true = log-only (no pause) for ALL breakpoints
static FILE* bp_log = nullptr;

// Poke triggers: on hit at trigger PC, write memory then continue without pausing.
// Each entry is either a static poke list, or a CSV-driven playback that advances
// one row per hit. Writes go through Automation_WriteMem8 (same as the `poke`
// command) and are atomic within a single hit — no SH-2 cycles intervene.
struct PokeOp {
 uint32_t addr;
 uint32_t value;
 uint8_t  width;  // bits: 8, 16, or 32
};

struct PokePlaybackCol {
 uint32_t offset;        // byte offset from base_addr
 uint8_t  width;         // bits: 8, 16, or 32
 size_t   csv_col_idx;   // column index in parsed CSV row
 bool     slice_enabled;
 int      slice_lo;      // inclusive byte index into BE32 source
 int      slice_hi;      // exclusive
};

enum PokePlaybackOnEnd { PPOE_HALT, PPOE_LOOP, PPOE_HOLD };

struct PokeTrigger {
 bool is_playback = false;

 // Static mode
 std::vector<PokeOp> static_pokes;

 // Playback mode
 uint32_t                              base_addr   = 0;
 std::vector<PokePlaybackCol>          columns;
 // rows[row_idx][col_idx] = pre-computed bytes to write.
 std::vector<std::vector<std::vector<uint8_t>>> rows;
 size_t                                start_row   = 0;
 size_t                                end_row     = 0;   // exclusive
 size_t                                cur_row     = 0;
 PokePlaybackOnEnd                     on_end      = PPOE_HOLD;
 bool                                  done        = false; // halt fired, no more pokes

 // Diagnostics
 uint64_t trigger_hits    = 0;
 uint64_t pokes_performed = 0;
};

static std::unordered_map<uint32_t, PokeTrigger> poke_triggers;
// One playback trigger at a time (single cursor). Tracked so stop/status can find it.
static bool     poke_playback_running = false;
static uint32_t poke_playback_pc = 0;
// Set by the hit callback when on_end=halt fires; consumed by Automation_DebugHook
// to pause execution and emit a "done poke_playback" ack.
static bool     poke_playback_halt_pending = false;
static uint32_t poke_playback_halt_pc = 0;

// Input trace state -- logs real keyboard input changes with frame numbers
static FILE* input_trace_file = nullptr;
static uint16_t last_traced_input = 0;

// Input playback state -- replays recorded input traces.
// Events are loaded into a vector on input_playback <path>.
// Automation_Poll checks events each frame and adjusts input_buttons.
struct InputEvent {
 uint64_t frame;
 bool press;     // true=press, false=release
 int button;     // BTN_* enum value
};
static std::vector<InputEvent> playback_events;
static size_t playback_index = 0;        // next event to process
static bool playback_active = false;
static uint64_t playback_base_frame = 0; // frame_counter at playback start

// Monotonic sequence counter -- appended to every ack to guarantee uniqueness.
// This solves change detection on DrvFS (Windows->WSL) where stat() mtime has
// only 1-second resolution and file size padding isn't always sufficient.
static uint64_t ack_seq = 0;

// Content-based change detection for action file.
// DrvFS (Windows->WSL filesystem) has unreliable stat() mtime caching that can
// miss rapid file updates from the Windows side. Instead of stat(), we read
// the first line (comment with sequence number) and compare to last-seen content.
static std::string last_action_header;

// Get current absolute master cycle count (for inclusion in ack messages).
static int64_t get_cycle(void)
{
 return MDFN_IEN_SS::Automation_GetMasterCycle();
}

static void write_ack(const std::string& msg)
{
 ack_seq++;
 int64_t cyc = get_cycle();
 std::ofstream f(ack_file, std::ios::trunc);
 if (f.is_open()) {
  f << msg << " cycle=" << cyc << " seq=" << ack_seq << "\n";
  f.close();
 }
}

// Enable/disable the SH-2 CPU debug hook based on what features need it.
// Called after any change to pc_trace, stepping, or breakpoint state.
static void update_cpu_hook(void)
{
 // Watchpoints don't need the CPU hook -- they're detected inline in BusRW_DB_CS3
 bool need = pc_trace_active || (instructions_to_step >= 0) || !breakpoints.empty()
          || (run_to_cycle_target >= 0) || !poke_triggers.empty();
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
 // Clamp text dumps to 4KB -- use dump_mem_bin for larger reads
 if (size > 0x1000) {
  char msg[128];
  snprintf(msg, sizeof(msg), "error dump_mem: size 0x%X exceeds 4KB text limit, use dump_mem_bin", size);
  write_ack(msg);
  return;
 }

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

static void do_screenshot(const std::string& path)
{
 if (!cached_fb_valid || !cached_fb_pixels) {
  write_ack("error screenshot: no cached framebuffer (need at least 1 frame)");
  return;
 }

 // Create a temporary surface pointing to our cached pixel buffer.
 // pixels_is_external=true (p_pixels != NULL) so destructor won't free it.
 MDFN_Surface tmp(cached_fb_pixels, cached_fb_w, cached_fb_h, cached_fb_pitch, cached_fb_format);
 try {
  PNGWrite(path, &tmp, cached_fb_rect, cached_fb_lw);
  write_ack("ok screenshot " + path);
 } catch(std::exception& e) {
  write_ack(std::string("error screenshot: ") + e.what());
 }
}

static void close_wp_log(void)
{
 if (wp_log) {
  fclose(wp_log);
  wp_log = nullptr;
 }
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
  watchpoint_paused = false;    // unblock watchpoint pause
  read_watchpoint_paused = false;
  exception_paused = false;
  instructions_to_step = -1;    // cancel step mode
  run_to_cycle_target = -1;     // cancel cycle target
  update_cpu_hook();
  write_ack("ok frame_advance " + std::to_string(n));
 }
 else if (cmd == "screenshot") {
  std::string path;
  iss >> path;
  if (path.empty()) {
   write_ack("error screenshot: no path specified");
  } else {
   // Immediate screenshot from cached framebuffer — no frame advance, no PC movement.
   do_screenshot(path);
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
  watchpoint_paused = false;
  read_watchpoint_paused = false;
  exception_paused = false;
  instructions_to_step = -1;
  run_to_cycle_target = -1;
  update_cpu_hook();
  write_ack("ok run_to_frame " + std::to_string(n));
 }
 else if (cmd == "run") {
  frames_to_advance = -1;
  run_to_frame_target = -1;
  instruction_paused = false;
  watchpoint_paused = false;
  read_watchpoint_paused = false;
  exception_paused = false;
  instructions_to_step = -1;
  run_to_cycle_target = -1;
  update_cpu_hook();
  // No ack -- single-threaded, command is guaranteed to execute.
  // Next ack will be the break/watchpoint event (no overwrite race).
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
  uint32_t addr = 0, size = 256;  // default 256 bytes if size omitted
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
 else if (cmd == "dump_slave_regs") {
  write_ack(MDFN_IEN_SS::Automation_DumpSlaveRegs());
 }
 else if (cmd == "call_stack") {
  uint32_t scan = 0x400;  // default 1024 bytes
  iss >> std::hex >> scan;
  if (scan > 0x1000) scan = 0x1000;
  write_ack(MDFN_IEN_SS::Automation_CallStack(scan));
 }
 else if (cmd == "dump_slave_regs_bin") {
  std::string path;
  iss >> path;
  if (path.empty()) {
   write_ack("error dump_slave_regs_bin: no path");
  } else {
   MDFN_IEN_SS::Automation_DumpSlaveRegsBin(path.c_str());
   write_ack("ok dump_slave_regs_bin " + path);
  }
 }
 else if (cmd == "poke") {
  // Write bytes to memory. Usage: poke <addr_hex> <byte_hex> [byte_hex ...]
  uint32_t addr = 0;
  iss >> std::hex >> addr;
  int count = 0;
  uint32_t val;
  while (iss >> std::hex >> val) {
   MDFN_IEN_SS::Automation_WriteMem8(addr + count, (uint8_t)(val & 0xFF));
   count++;
  }
  char buf[128];
  snprintf(buf, sizeof(buf), "ok poke 0x%08X %d bytes", addr, count);
  write_ack(buf);
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
    char ack_msg[128];
    snprintf(ack_msg, sizeof(ack_msg), "ok dump_mem_bin 0x%08X 0x%X", addr, size);
    write_ack(ack_msg);
   } else {
    write_ack("error dump_mem_bin: cannot open " + path);
   }
  }
 }
 else if (cmd == "dump_region") {
  // Convenience command: dump named memory region to binary file.
  // Usage: dump_region <name> <path>
  // Regions: wram_high (1MB), wram_low (1MB), vdp1_vram (512KB),
  //          vdp2_vram (512KB), vdp2_cram (4KB), sound_ram (512KB)
  std::string name, path;
  iss >> name >> path;
  if (name.empty() || path.empty()) {
   write_ack("error dump_region: usage: dump_region <name> <path>");
  } else {
   uint32_t addr = 0, size = 0;
   if (name == "wram_high")      { addr = 0x06000000; size = 0x100000; }
   else if (name == "wram_low")  { addr = 0x00200000; size = 0x100000; }
   else if (name == "vdp1_vram") { addr = 0x05C00000; size = 0x080000; }
   else if (name == "vdp2_vram") { addr = 0x05E00000; size = 0x080000; }
   else if (name == "vdp2_cram") { addr = 0x05F00000; size = 0x001000; }
   else if (name == "sound_ram") { addr = 0x05A00000; size = 0x080000; }
   else {
    write_ack("error dump_region: unknown region '" + name +
     "' (valid: wram_high wram_low vdp1_vram vdp2_vram vdp2_cram sound_ram)");
    return;
   }
   std::vector<uint8_t> buf(size);
   MDFN_IEN_SS::Automation_ReadMemBlock(addr, buf.data(), size);
   FILE* f = fopen(path.c_str(), "wb");
   if (f) {
    fwrite(buf.data(), 1, size, f);
    fclose(f);
    char ack_msg[128];
    snprintf(ack_msg, sizeof(ack_msg), "ok dump_region %s 0x%08X 0x%X %s",
     name.c_str(), addr, size, path.c_str());
    write_ack(ack_msg);
   } else {
    write_ack("error dump_region: cannot open " + path);
   }
  }
 }
 else if (cmd == "dump_vdp2_regs") {
  std::string path;
  iss >> path;
  if (path.empty()) {
   write_ack("error dump_vdp2_regs: no path");
  } else {
   MDFN_IEN_SS::Automation_DumpVDP2RegsBin(path.c_str());
   write_ack("ok dump_vdp2_regs " + path);
  }
 }
 else if (cmd == "save_state") {
  std::string path;
  std::getline(iss >> std::ws, path);
  if (path.empty()) {
   write_ack("error save_state: no path");
  } else {
   try {
    // Match MDFNI_SaveState: SaveSM to memory, then gzip to file
    MemoryStream ms(65536);
    MDFNSS_SaveSM(&ms);
    GZFileStream gp(path, GZFileStream::MODE::WRITE, 6);
    gp.write(ms.map(), ms.size());
    gp.close();
    write_ack("ok save_state " + path);
   } catch (std::exception& e) {
    write_ack(std::string("error save_state: ") + e.what());
   }
  }
 }
 else if (cmd == "load_state") {
  std::string path;
  std::getline(iss >> std::ws, path);
  if (path.empty()) {
   write_ack("error load_state: no path");
  } else {
   try {
    // Match MDFNI_LoadState: gzip decompress, parse header, LoadSM
    GZFileStream st(path, GZFileStream::MODE::READ);
    uint8 header[32];
    st.read(header, 32);
    uint32 st_len = MDFN_de32lsb(header + 16 + 4) & 0x7FFFFFFF;
    if (st_len < 32)
     throw std::runtime_error("Save state header length field is bad");
    MemoryStream sm(st_len, -1);
    memcpy(sm.map(), header, 32);
    st.read(sm.map() + 32, st_len - 32);
    MDFNSS_LoadSM(&sm, false);
    frame_counter = 0;  // Reset to 0 — all frame references are relative to save state load
    write_ack("ok load_state " + path);
   } catch (std::exception& e) {
    write_ack(std::string("error load_state: ") + e.what());
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
    instruction_paused = false;   // unblock instruction-level pause
    watchpoint_paused = false;    // unblock watchpoint pause
  read_watchpoint_paused = false;
  exception_paused = false;
    instructions_to_step = -1;    // cancel step mode
    run_to_cycle_target = -1;     // cancel cycle target
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
  watchpoint_paused = false;   // unblock watchpoint pause
  read_watchpoint_paused = false;
  exception_paused = false;
  // Unblock frame-level pause -- the CPU hook will pause us after N instructions
  if (frames_to_advance == 0)
   frames_to_advance = -1;
  update_cpu_hook();
  write_ack("ok step " + std::to_string(n));
 }
 else if (cmd == "breakpoint") {
  uint32_t addr = 0;
  iss >> std::hex >> addr;
  // Check for "log" flag
  std::string token;
  if (iss >> token && token == "log") {
   breakpoint_log_mode = true;
   if (!bp_log) {
    std::string path = auto_base_dir + "/breakpoint_hits.txt";
    bp_log = fopen(path.c_str(), "w");
    if (bp_log)
     fprintf(bp_log, "# Breakpoint hit log (log mode)\n");
   }
  }
  breakpoints.insert(addr);
  update_cpu_hook();
  char buf[64];
  snprintf(buf, sizeof(buf), "0x%08X", addr);
  std::string ack_msg = "ok breakpoint " + std::string(buf) + " total=" + std::to_string(breakpoints.size());
  if (breakpoint_log_mode) ack_msg += " log";
  write_ack(ack_msg);
 }
 else if (cmd == "breakpoint_remove") {
  uint32_t addr = 0;
  iss >> std::hex >> addr;
  size_t removed = breakpoints.erase(addr);
  update_cpu_hook();
  char buf[64];
  if (removed) {
   snprintf(buf, sizeof(buf), "ok breakpoint_remove 0x%08X total=%zu", addr, breakpoints.size());
  } else {
   snprintf(buf, sizeof(buf), "error breakpoint_remove: not found 0x%08X", addr);
  }
  write_ack(buf);
 }
 else if (cmd == "breakpoint_clear") {
  size_t count = breakpoints.size();
  breakpoints.clear();
  breakpoint_log_mode = false;
  if (bp_log) { fclose(bp_log); bp_log = nullptr; }
  update_cpu_hook();
  write_ack("ok breakpoint_clear removed=" + std::to_string(count));
 }
 else if (cmd == "breakpoint_list") {
  std::ostringstream ss;
  ss << "breakpoints count=" << breakpoints.size();
  for (auto addr : breakpoints) {
   char buf[16];
   snprintf(buf, sizeof(buf), " 0x%08X", addr);
   ss << buf;
  }
  write_ack(ss.str());
 }
 else if (cmd == "show_window") {
  Video_AutomationShowWindow();
  write_ack("ok show_window");
 }
 else if (cmd == "hide_window") {
  Video_AutomationHideWindow();
  write_ack("ok hide_window");
 }
 else if (cmd == "call_trace") {
  std::string path;
  iss >> path;
  if (path.empty()) {
   write_ack("error call_trace: no path");
  } else {
   MDFN_IEN_SS::Automation_EnableCallTrace(path.c_str());
   write_ack("ok call_trace " + path);
  }
 }
 else if (cmd == "call_trace_stop") {
  MDFN_IEN_SS::Automation_DisableCallTrace();
  write_ack("ok call_trace_stop");
 }
 else if (cmd == "scdq_trace") {
  std::string path;
  iss >> path;
  if (path.empty()) {
   write_ack("error scdq_trace: no path");
  } else {
   MDFN_IEN_SS::CDB_EnableSCDQTrace(path.c_str());
   write_ack("ok scdq_trace " + path);
  }
 }
 else if (cmd == "scdq_trace_stop") {
  MDFN_IEN_SS::CDB_DisableSCDQTrace();
  write_ack("ok scdq_trace_stop");
 }
 else if (cmd == "cdb_trace") {
  std::string path;
  iss >> path;
  if (path.empty()) {
   write_ack("error cdb_trace: no path");
  } else {
   MDFN_IEN_SS::CDB_EnableCDBTrace(path.c_str());
   write_ack("ok cdb_trace " + path);
  }
 }
 else if (cmd == "cdb_trace_stop") {
  MDFN_IEN_SS::CDB_DisableCDBTrace();
  write_ack("ok cdb_trace_stop");
 }
 else if (cmd == "unified_trace") {
  std::string path;
  iss >> path;
  if (path.empty()) {
   write_ack("error unified_trace: no path");
  } else {
   // Stop any existing unified trace
   if (unified_trace_file) {
    MDFN_IEN_SS::Automation_ClearCallTraceFile();
    MDFN_IEN_SS::CDB_ClearCDBTraceFile();
    fclose(unified_trace_file);
    unified_trace_file = nullptr;
   }
   unified_trace_file = fopen(path.c_str(), "w");
   if (unified_trace_file) {
    fprintf(unified_trace_file, "# Unified trace: <sh2_cycle> <source> <details>\n"
      "# M/S = SH-2 master/slave call, CMD/DRV/IRQ/BUF = CD Block\n");
    MDFN_IEN_SS::Automation_SetCallTraceFile(unified_trace_file);
    MDFN_IEN_SS::CDB_SetCDBTraceFile(unified_trace_file);
    write_ack("ok unified_trace " + path);
   } else {
    write_ack("error unified_trace: failed to open " + path);
   }
  }
 }
 else if (cmd == "unified_trace_stop") {
  MDFN_IEN_SS::Automation_ClearCallTraceFile();
  MDFN_IEN_SS::CDB_ClearCDBTraceFile();
  if (unified_trace_file) {
   fflush(unified_trace_file);
   fclose(unified_trace_file);
   unified_trace_file = nullptr;
  }
  write_ack("ok unified_trace_stop");
 }
 else if (cmd == "input_trace") {
  std::string path;
  iss >> path;
  if (path.empty()) {
   write_ack("error input_trace: no path");
  } else {
   if (input_trace_file) fclose(input_trace_file);
   input_trace_file = fopen(path.c_str(), "w");
   if (input_trace_file) {
    fprintf(input_trace_file, "# Input trace -- frame PRESS/RELEASE BUTTON\n");
    last_traced_input = 0;
    write_ack("ok input_trace " + path);
   } else {
    write_ack("error input_trace: cannot open " + path);
   }
  }
 }
 else if (cmd == "input_trace_stop") {
  if (input_trace_file) {
   fclose(input_trace_file);
   input_trace_file = nullptr;
  }
  write_ack("ok input_trace_stop");
 }
 else if (cmd == "input_playback") {
  std::string path;
  iss >> path;
  if (path.empty()) {
   write_ack("error input_playback: no path");
  } else {
   FILE* f = fopen(path.c_str(), "r");
   if (!f) {
    write_ack("error input_playback: cannot open " + path);
   } else {
    playback_events.clear();
    playback_index = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
     // Parse: frame=N PRESS/RELEASE BUTTON
     uint64_t fr = 0;
     char action[32] = {0}, button[32] = {0};
     if (sscanf(line, "frame=%llu %31s %31s", (unsigned long long*)&fr, action, button) == 3) {
      int b = parse_button(button);
      if (b >= 0) {
       InputEvent ev;
       ev.frame = fr;
       ev.press = (strcmp(action, "PRESS") == 0);
       ev.button = b;
       playback_events.push_back(ev);
      }
     }
    }
    fclose(f);
    // Clear current input state for clean playback
    input_buttons = 0;
    input_override = false;
    playback_base_frame = frame_counter;
    playback_active = true;
    char buf[256];
    snprintf(buf, sizeof(buf), "ok input_playback %zu events from %s",
             playback_events.size(), path.c_str());
    write_ack(buf);
   }
  }
 }
 else if (cmd == "input_playback_stop") {
  playback_active = false;
  playback_events.clear();
  playback_index = 0;
  write_ack("ok input_playback_stop");
 }
 else if (cmd == "watchpoint") {
  uint32_t addr = 0;
  iss >> std::hex >> addr;
  watchpoint_addr = addr;
  watchpoint_active = true;
  watchpoint_paused = false;
  read_watchpoint_paused = false;
  exception_paused = false;
  watchpoint_filter_active = false;
  watchpoint_filter_value = 0;
  watchpoint_log_mode = false;
  // Close stale log from previous watchpoint
  close_wp_log();
  MDFN_IEN_SS::Automation_SetWatchpoint(addr);
  // Parse optional flags: "watchpoint <addr> [eq <value>] [log]"
  std::string token;
  while (iss >> token) {
   if (token == "eq") {
    uint32_t filter_val = 0;
    iss >> std::hex >> filter_val;
    watchpoint_filter_active = true;
    watchpoint_filter_value = filter_val;
    MDFN_IEN_SS::Automation_SetWatchpointFilter(true, filter_val);
   } else if (token == "log") {
    watchpoint_log_mode = true;
   }
  }
  update_cpu_hook();
  char buf[128];
  if (watchpoint_filter_active && watchpoint_log_mode)
   snprintf(buf, sizeof(buf), "ok watchpoint 0x%08X eq 0x%08X log", addr, watchpoint_filter_value);
  else if (watchpoint_filter_active)
   snprintf(buf, sizeof(buf), "ok watchpoint 0x%08X eq 0x%08X", addr, watchpoint_filter_value);
  else if (watchpoint_log_mode)
   snprintf(buf, sizeof(buf), "ok watchpoint 0x%08X log", addr);
  else
   snprintf(buf, sizeof(buf), "ok watchpoint 0x%08X", addr);
  write_ack(buf);
 }
 else if (cmd == "watchpoint_clear") {
  watchpoint_active = false;
  watchpoint_paused = false;
  read_watchpoint_paused = false;
  exception_paused = false;
  close_wp_log();
  MDFN_IEN_SS::Automation_ClearWatchpoint();
  update_cpu_hook();
  write_ack("ok watchpoint_clear");
 }
 else if (cmd == "vdp2_watchpoint") {
  uint32_t lo = 0, hi = 0;
  std::string logpath;
  iss >> std::hex >> lo >> hi >> logpath;
  if (logpath.empty()) {
   write_ack("error vdp2_watchpoint: usage: vdp2_watchpoint <lo_hex> <hi_hex> <logpath>");
  } else {
   MDFN_IEN_SS::Automation_SetVDP2Watchpoint(lo, hi, logpath.c_str());
   char buf[128];
   snprintf(buf, sizeof(buf), "ok vdp2_watchpoint 0x%08X-0x%08X %s", lo, hi, logpath.c_str());
   write_ack(buf);
  }
 }
 else if (cmd == "vdp2_watchpoint_clear") {
  MDFN_IEN_SS::Automation_ClearVDP2Watchpoint();
  write_ack("ok vdp2_watchpoint_clear");
 }
 else if (cmd == "read_watchpoint") {
  uint32_t addr = 0;
  iss >> std::hex >> addr;
  read_watchpoint_addr = addr;
  read_watchpoint_active = true;
  read_watchpoint_log_mode = false;
  // Close stale log
  if (rwp_log) { fclose(rwp_log); rwp_log = nullptr; }
  // Check for "log" flag
  std::string token;
  if (iss >> token && token == "log") {
   read_watchpoint_log_mode = true;
  }
  MDFN_IEN_SS::Automation_SetReadWatchpoint(addr);
  char buf[128];
  if (read_watchpoint_log_mode)
   snprintf(buf, sizeof(buf), "ok read_watchpoint 0x%08X log", addr);
  else
   snprintf(buf, sizeof(buf), "ok read_watchpoint 0x%08X", addr);
  write_ack(buf);
 }
 else if (cmd == "read_watchpoint_clear") {
  read_watchpoint_active = false;
  read_watchpoint_paused = false;
  exception_paused = false;
  if (rwp_log) { fclose(rwp_log); rwp_log = nullptr; }
  MDFN_IEN_SS::Automation_ClearReadWatchpoint();
  write_ack("ok read_watchpoint_clear");
 }
 else if (cmd == "deterministic") {
  MDFN_IEN_SS::Automation_SetDeterministic();
  write_ack("ok deterministic");
 }
 else if (cmd == "exception_break") {
  std::string mode;
  iss >> mode;
  if (mode == "enable") {
   exception_mode = EXC_ENABLE;
   write_ack("ok exception_break enable");
  } else if (mode == "log") {
   exception_mode = EXC_LOG;
   if (!exc_log) {
    std::string path = auto_base_dir + "/exception_hits.txt";
    exc_log = fopen(path.c_str(), "w");
    if (exc_log) fprintf(exc_log, "# SH-2 exception log\n");
   }
   write_ack("ok exception_break log");
  } else if (mode == "disable") {
   exception_mode = EXC_DISABLE;
   exception_paused = false;
   if (exc_log) { fclose(exc_log); exc_log = nullptr; }
   write_ack("ok exception_break disable");
  } else {
   write_ack("error exception_break: usage: exception_break <enable|log|disable>");
  }
 }
 else if (cmd == "insn_trace") {
  std::string path;
  int64_t start_line = 0, stop_line = 0;
  iss >> path >> start_line >> stop_line;
  if (path.empty() || start_line <= 0 || stop_line <= 0) {
   write_ack("error insn_trace: usage: insn_trace <path> <start_line> <stop_line>");
  } else {
   MDFN_IEN_SS::Automation_EnableInsnTrace(path.c_str(), start_line, stop_line);
   char buf[256];
   snprintf(buf, sizeof(buf), "ok insn_trace %s start=%lld stop=%lld", path.c_str(), (long long)start_line, (long long)stop_line);
   write_ack(buf);
  }
 }
 else if (cmd == "insn_trace_unified") {
  int64_t start_line = 0, stop_line = 0;
  iss >> start_line >> stop_line;
  if (start_line <= 0 || stop_line <= 0) {
   write_ack("error insn_trace_unified: usage: insn_trace_unified <start_line> <stop_line>");
  } else {
   MDFN_IEN_SS::Automation_EnableInsnTraceUnified(start_line, stop_line);
   char buf[256];
   snprintf(buf, sizeof(buf), "ok insn_trace_unified start=%lld stop=%lld", (long long)start_line, (long long)stop_line);
   write_ack(buf);
  }
 }
 else if (cmd == "insn_trace_stop") {
  MDFN_IEN_SS::Automation_DisableInsnTrace();
  write_ack("ok insn_trace_stop");
 }
 else if (cmd == "dump_cycle") {
  char buf[64];
  snprintf(buf, sizeof(buf), "ok dump_cycle value=%lld", (long long)get_cycle());
  write_ack(buf);
 }
 else if (cmd == "run_to_cycle") {
  int64_t n = 0;
  iss >> n;
  int64_t current = get_cycle();
  if (n <= current) {
   char buf[128];
   snprintf(buf, sizeof(buf), "warning run_to_cycle: target %lld <= current %lld, firing immediately", (long long)n, (long long)current);
   write_ack(buf);
   return;
  }
  run_to_cycle_target = n;
  instruction_paused = false;
  watchpoint_paused = false;
  read_watchpoint_paused = false;
  exception_paused = false;
  instructions_to_step = -1;
  if (frames_to_advance == 0)
   frames_to_advance = -1;
  update_cpu_hook();
  char buf[128];
  snprintf(buf, sizeof(buf), "ok run_to_cycle target=%lld", (long long)n);
  write_ack(buf);
 }
 else if (cmd == "cdl_start") {
  // Optional: cdl_start <lo_hex> <hi_hex>
  // Defaults to HWR: 0x06000000-0x06100000
  uint32_t lo = 0x06000000, hi = 0x06100000;
  uint32_t user_lo;
  if (iss >> std::hex >> user_lo) {
   uint32_t user_hi;
   if (!(iss >> std::hex >> user_hi)) {
    write_ack("error cdl_start: need both lo and hi, or neither");
   } else {
    lo = user_lo;
    hi = user_hi;
   }
  }
  MDFN_IEN_SS::Automation_CDLStart(lo, hi);
  // Use actual range from CDL state (after masking/validation)
  uint32_t actual_lo = MDFN_IEN_SS::Automation_CDLGetLo();
  uint32_t actual_hi = MDFN_IEN_SS::Automation_CDLGetHi();
  uint32_t actual_size = MDFN_IEN_SS::Automation_CDLGetSize();
  if (actual_size == 0) {
   write_ack("error cdl_start: invalid range");
  } else {
   char buf[128];
   snprintf(buf, sizeof(buf), "ok cdl_start 0x%08X-0x%08X (%uKB)",
            actual_lo, actual_hi, actual_size / 1024);
   write_ack(buf);
  }
 }
 else if (cmd == "cdl_stop") {
  MDFN_IEN_SS::Automation_CDLStop();
  write_ack("ok cdl_stop");
 }
 else if (cmd == "cdl_reset") {
  MDFN_IEN_SS::Automation_CDLReset();
  write_ack("ok cdl_reset");
 }
 else if (cmd == "cdl_dump") {
  std::string path;
  iss >> path;
  if (path.empty()) {
   write_ack("error cdl_dump: no path");
  } else {
   if (MDFN_IEN_SS::Automation_CDLDump(path.c_str()))
    write_ack("ok cdl_dump " + path);
   else
    write_ack("error cdl_dump: cannot open " + path);
  }
 }
 else if (cmd == "cdl_status") {
  write_ack(std::string("ok cdl_status active=") +
   (MDFN_IEN_SS::Automation_CDLIsActive() ? "true" : "false"));
 }
 else if (cmd == "dma_trace") {
  std::string path;
  iss >> path;
  if (path.empty()) {
   write_ack("error dma_trace: no path");
  } else {
   MDFN_IEN_SS::Automation_EnableDMATrace(path.c_str());
   write_ack("ok dma_trace " + path);
  }
 }
 else if (cmd == "dma_trace_stop") {
  MDFN_IEN_SS::Automation_DisableDMATrace();
  write_ack("ok dma_trace_stop");
 }
 else if (cmd == "mem_profile") {
  uint32_t lo = 0, hi = 0;
  std::string path;
  iss >> std::hex >> lo >> hi >> path;
  if (path.empty()) {
   write_ack("error mem_profile: usage: mem_profile <lo_hex> <hi_hex> <path>");
  } else {
   MDFN_IEN_SS::Automation_EnableMemProfile(path.c_str(), lo, hi);
   char buf[256];
   snprintf(buf, sizeof(buf), "ok mem_profile 0x%08X-0x%08X %s", lo, hi, path.c_str());
   write_ack(buf);
  }
 }
 else if (cmd == "mem_profile_stop") {
  MDFN_IEN_SS::Automation_DisableMemProfile();
  write_ack("ok mem_profile_stop");
 }
 else if (cmd == "mem_read_profile") {
  uint32_t lo = 0, hi = 0;
  std::string path;
  iss >> std::hex >> lo >> hi >> path;
  if (path.empty()) {
   write_ack("error mem_read_profile: usage: mem_read_profile <lo_hex> <hi_hex> <path>");
  } else {
   MDFN_IEN_SS::Automation_EnableMemReadProfile(path.c_str(), lo, hi);
   char buf[256];
   snprintf(buf, sizeof(buf), "ok mem_read_profile 0x%08X-0x%08X %s", lo, hi, path.c_str());
   write_ack(buf);
  }
 }
 else if (cmd == "mem_read_profile_stop") {
  MDFN_IEN_SS::Automation_DisableMemReadProfile();
  write_ack("ok mem_read_profile_stop");
 }
 else if (cmd == "mem_sample") {
  uint32_t addr = 0, sz = 0;
  int64_t frames = 0;
  std::string path;
  iss >> std::hex >> addr >> sz >> std::dec >> frames >> path;
  if (path.empty() || sz == 0 || frames <= 0) {
   write_ack("error mem_sample: usage: mem_sample <addr_hex> <size_hex> <frames_dec> <path>");
  } else {
   if (sz > 0x10000) sz = 0x10000;  // 64KB max per frame
   if (mem_sample_file) fclose(mem_sample_file);
   mem_sample_file = fopen(path.c_str(), "wb");
   if (mem_sample_file) {
    mem_sample_addr = addr;
    mem_sample_size = sz;
    mem_sample_frames = frames;
    mem_sample_total = frames;
    // Free-run — mem_sample_frames controls when to stop
    frames_to_advance = -1;
    instruction_paused = false;
    watchpoint_paused = false;
    read_watchpoint_paused = false;
    exception_paused = false;
    instructions_to_step = -1;
    run_to_cycle_target = -1;
    update_cpu_hook();
    char buf[256];
    snprintf(buf, sizeof(buf), "ok mem_sample 0x%08X 0x%X %lld %s",
             addr, sz, (long long)frames, path.c_str());
    write_ack(buf);
   } else {
    write_ack("error mem_sample: cannot open " + path);
   }
  }
 }
 else if (cmd == "mem_sample_stop") {
  if (mem_sample_file) {
   int64_t captured = mem_sample_total - mem_sample_frames;
   fclose(mem_sample_file);
   mem_sample_file = nullptr;
   mem_sample_frames = 0;
   char buf[128];
   snprintf(buf, sizeof(buf), "ok mem_sample_stop captured=%lld", (long long)captured);
   write_ack(buf);
  } else {
   write_ack("ok mem_sample_stop (not active)");
  }
 }
 else if (cmd == "poke_breakpoint") {
  // poke_breakpoint <trigger_pc_hex> <n_pokes> <addr_hex>:<val_hex>:<width> ...
  // Each hit at trigger_pc writes all pokes (atomic, no intervening cycles)
  // and continues without pausing. Replaces any existing trigger at that PC.
  uint32_t tpc = 0;
  int n_pokes = 0;
  iss >> std::hex >> tpc;
  iss >> std::dec >> n_pokes;
  if (n_pokes < 1 || n_pokes > 64) {
   write_ack("error poke_breakpoint: n_pokes out of range (1..64)");
   return;
  }
  std::vector<PokeOp> pokes;
  pokes.reserve(n_pokes);
  for (int i = 0; i < n_pokes; i++) {
   std::string spec;
   if (!(iss >> spec)) {
    write_ack("error poke_breakpoint: missing poke spec");
    return;
   }
   // Split spec on ':'
   size_t c1 = spec.find(':');
   size_t c2 = (c1 == std::string::npos) ? std::string::npos : spec.find(':', c1 + 1);
   if (c1 == std::string::npos || c2 == std::string::npos) {
    write_ack("error poke_breakpoint: bad spec (need addr:val:width): " + spec);
    return;
   }
   PokeOp p;
   try {
    p.addr  = (uint32_t)std::stoul(spec.substr(0, c1), nullptr, 16);
    p.value = (uint32_t)std::stoul(spec.substr(c1 + 1, c2 - c1 - 1), nullptr, 16);
    int w   = std::stoi(spec.substr(c2 + 1));
    if (w != 8 && w != 16 && w != 32) {
     write_ack("error poke_breakpoint: width must be 8/16/32");
     return;
    }
    p.width = (uint8_t)w;
   } catch (...) {
    write_ack("error poke_breakpoint: parse error in spec: " + spec);
    return;
   }
   pokes.push_back(p);
  }
  // If a playback was active at this PC, it's being replaced — clear flags.
  auto existing = poke_triggers.find(tpc);
  if (existing != poke_triggers.end() && existing->second.is_playback) {
   if (poke_playback_running && poke_playback_pc == tpc) {
    poke_playback_running = false;
    poke_playback_pc = 0;
   }
  }
  PokeTrigger trig;
  trig.is_playback     = false;
  trig.static_pokes    = std::move(pokes);
  trig.trigger_hits    = 0;
  trig.pokes_performed = 0;
  poke_triggers[tpc]   = std::move(trig);
  update_cpu_hook();
  char buf[128];
  snprintf(buf, sizeof(buf), "ok poke_breakpoint 0x%08X pokes=%d total_triggers=%zu",
           tpc, n_pokes, poke_triggers.size());
  write_ack(buf);
 }
 else if (cmd == "poke_breakpoint_remove") {
  uint32_t tpc = 0;
  iss >> std::hex >> tpc;
  auto it = poke_triggers.find(tpc);
  if (it == poke_triggers.end()) {
   char buf[64];
   snprintf(buf, sizeof(buf), "error poke_breakpoint_remove: not found 0x%08X", tpc);
   write_ack(buf);
   return;
  }
  if (it->second.is_playback && poke_playback_running && poke_playback_pc == tpc) {
   poke_playback_running = false;
   poke_playback_pc = 0;
  }
  poke_triggers.erase(it);
  update_cpu_hook();
  char buf[128];
  snprintf(buf, sizeof(buf), "ok poke_breakpoint_remove 0x%08X total_triggers=%zu",
           tpc, poke_triggers.size());
  write_ack(buf);
 }
 else if (cmd == "poke_breakpoint_clear") {
  size_t count = poke_triggers.size();
  poke_triggers.clear();
  poke_playback_running = false;
  poke_playback_pc = 0;
  poke_playback_halt_pending = false;
  update_cpu_hook();
  write_ack("ok poke_breakpoint_clear removed=" + std::to_string(count));
 }
 else if (cmd == "poke_breakpoint_list") {
  std::ostringstream ss;
  ss << "poke_triggers count=" << poke_triggers.size();
  for (const auto& kv : poke_triggers) {
   char buf[128];
   if (kv.second.is_playback) {
    snprintf(buf, sizeof(buf), " 0x%08X=playback(rows=%zu,cur=%zu,hits=%llu)",
             kv.first, kv.second.rows.size(), kv.second.cur_row,
             (unsigned long long)kv.second.trigger_hits);
   } else {
    snprintf(buf, sizeof(buf), " 0x%08X=static(pokes=%zu,hits=%llu)",
             kv.first, kv.second.static_pokes.size(),
             (unsigned long long)kv.second.trigger_hits);
   }
   ss << buf;
  }
  write_ack(ss.str());
 }
 else if (cmd == "poke_playback_start") {
  // poke_playback_start <trigger_pc> <base_addr> <start_row> <end_row>
  //                     <on_end> <n_cols> <col1> ... <colN> <csv_path>
  // Each col: <csv_name>:<offset_hex>:<width>[:<slice_lo>:<slice_hi>]
  // on_end: halt|loop|hold
  // end_row=-1 means EOF. Replaces any existing trigger at that PC, and
  // supersedes any prior playback (only one playback cursor at a time).
  uint32_t tpc = 0, base = 0;
  int64_t  start_row_in = 0, end_row_in = -1;
  std::string on_end_str;
  int n_cols = 0;
  iss >> std::hex >> tpc >> base;
  iss >> std::dec >> start_row_in >> end_row_in >> on_end_str >> n_cols;
  if (n_cols < 1 || n_cols > 256) {
   write_ack("error poke_playback_start: n_cols out of range (1..256)");
   return;
  }
  PokePlaybackOnEnd on_end;
  if      (on_end_str == "halt") on_end = PPOE_HALT;
  else if (on_end_str == "loop") on_end = PPOE_LOOP;
  else if (on_end_str == "hold") on_end = PPOE_HOLD;
  else {
   write_ack("error poke_playback_start: on_end must be halt|loop|hold");
   return;
  }
  // Parse column specs
  std::vector<std::string>        col_names(n_cols);
  std::vector<PokePlaybackCol>    cols(n_cols);
  for (int i = 0; i < n_cols; i++) {
   std::string spec;
   if (!(iss >> spec)) {
    write_ack("error poke_playback_start: missing column spec");
    return;
   }
   // Split on ':' into up to 5 parts: name, offset, width, [slice_lo, slice_hi]
   std::vector<std::string> parts;
   size_t start = 0;
   while (true) {
    size_t pos = spec.find(':', start);
    if (pos == std::string::npos) { parts.push_back(spec.substr(start)); break; }
    parts.push_back(spec.substr(start, pos - start));
    start = pos + 1;
   }
   if (parts.size() != 3 && parts.size() != 5) {
    write_ack("error poke_playback_start: column spec must be name:offset:width[:lo:hi] — got " + spec);
    return;
   }
   try {
    col_names[i]          = parts[0];
    cols[i].offset        = (uint32_t)std::stoul(parts[1], nullptr, 16);
    int w                 = std::stoi(parts[2]);
    if (w != 8 && w != 16 && w != 32) {
     write_ack("error poke_playback_start: width must be 8/16/32");
     return;
    }
    cols[i].width         = (uint8_t)w;
    cols[i].slice_enabled = (parts.size() == 5);
    if (cols[i].slice_enabled) {
     cols[i].slice_lo = std::stoi(parts[3]);
     cols[i].slice_hi = std::stoi(parts[4]);
     if (cols[i].slice_lo < 0 || cols[i].slice_hi > 4
         || cols[i].slice_lo >= cols[i].slice_hi
         || (cols[i].slice_hi - cols[i].slice_lo) * 8 != w) {
      write_ack("error poke_playback_start: bad byte_slice (must be within [0,4] and match width)");
      return;
     }
    } else {
     cols[i].slice_lo = cols[i].slice_hi = 0;
    }
    cols[i].csv_col_idx   = 0; // filled in after header parse
   } catch (...) {
    write_ack("error poke_playback_start: parse error in col spec: " + spec);
    return;
   }
  }
  // Rest of line is the CSV path (single token, no spaces supported)
  std::string csv_path;
  iss >> csv_path;
  if (csv_path.empty()) {
   write_ack("error poke_playback_start: missing csv_path");
   return;
  }
  std::ifstream cf(csv_path);
  if (!cf.is_open()) {
   write_ack("error poke_playback_start: cannot open CSV " + csv_path);
   return;
  }
  // Parse header
  std::string line;
  if (!std::getline(cf, line)) {
   write_ack("error poke_playback_start: empty CSV");
   return;
  }
  auto split_csv = [](const std::string& s, std::vector<std::string>& out) {
   out.clear();
   std::string tok;
   for (size_t i = 0; i < s.size(); i++) {
    char ch = s[i];
    if (ch == ',') { out.push_back(tok); tok.clear(); }
    else if (ch != '\r' && ch != '\n') tok.push_back(ch);
   }
   out.push_back(tok);
  };
  std::vector<std::string> headers;
  split_csv(line, headers);
  // Resolve each requested name to a header index
  for (int i = 0; i < n_cols; i++) {
   bool found = false;
   for (size_t j = 0; j < headers.size(); j++) {
    if (headers[j] == col_names[i]) { cols[i].csv_col_idx = j; found = true; break; }
   }
   if (!found) {
    write_ack("error poke_playback_start: column '" + col_names[i] + "' not in CSV header");
    return;
   }
  }
  // Parse all data rows, pre-compute bytes for each (row, col)
  std::vector<std::vector<std::vector<uint8_t>>> rows;
  while (std::getline(cf, line)) {
   if (line.empty()) continue;
   if (line[0] == '#') continue;
   std::vector<std::string> cells;
   split_csv(line, cells);
   std::vector<std::vector<uint8_t>> row_bytes(n_cols);
   for (int c = 0; c < n_cols; c++) {
    if (cols[c].csv_col_idx >= cells.size()) {
     char buf[160];
     snprintf(buf, sizeof(buf),
              "error poke_playback_start: row %zu has %zu cols, col index %zu out of range",
              rows.size(), cells.size(), cols[c].csv_col_idx);
     write_ack(buf);
     return;
    }
    const std::string& cell = cells[cols[c].csv_col_idx];
    std::string s = cell;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    uint64_t val;
    try {
     val = std::stoull(s, nullptr, 16);
    } catch (...) {
     write_ack("error poke_playback_start: bad hex cell '" + cell + "'");
     return;
    }
    // Render as big-endian 4 bytes (source assumed 32-bit hex column)
    uint8_t be4[4] = {
     (uint8_t)((val >> 24) & 0xFF),
     (uint8_t)((val >> 16) & 0xFF),
     (uint8_t)((val >>  8) & 0xFF),
     (uint8_t)(val         & 0xFF)
    };
    std::vector<uint8_t> bytes;
    if (cols[c].slice_enabled) {
     for (int b = cols[c].slice_lo; b < cols[c].slice_hi; b++) bytes.push_back(be4[b]);
    } else {
     // Take low (width/8) bytes in BE order
     int n = cols[c].width / 8;
     for (int b = 4 - n; b < 4; b++) bytes.push_back(be4[b]);
    }
    row_bytes[c] = std::move(bytes);
   }
   rows.push_back(std::move(row_bytes));
  }
  cf.close();
  if (rows.empty()) {
   write_ack("error poke_playback_start: CSV has no data rows");
   return;
  }
  // Resolve row bounds
  size_t start_row = (start_row_in < 0) ? 0 : (size_t)start_row_in;
  size_t end_row   = (end_row_in   < 0) ? rows.size() : (size_t)end_row_in;
  if (start_row >= rows.size() || end_row > rows.size() || start_row >= end_row) {
   char buf[160];
   snprintf(buf, sizeof(buf),
            "error poke_playback_start: bad row range start=%zu end=%zu total=%zu",
            start_row, end_row, rows.size());
   write_ack(buf);
   return;
  }
  // Supersede any prior playback
  if (poke_playback_running) {
   poke_triggers.erase(poke_playback_pc);
   poke_playback_running = false;
  }
  PokeTrigger trig;
  trig.is_playback     = true;
  trig.base_addr       = base;
  trig.columns         = std::move(cols);
  trig.rows            = std::move(rows);
  trig.start_row       = start_row;
  trig.end_row         = end_row;
  trig.cur_row         = start_row;
  trig.on_end          = on_end;
  trig.done            = false;
  trig.trigger_hits    = 0;
  trig.pokes_performed = 0;
  poke_triggers[tpc]   = std::move(trig);
  poke_playback_running = true;
  poke_playback_pc      = tpc;
  poke_playback_halt_pending = false;
  update_cpu_hook();
  char buf[192];
  snprintf(buf, sizeof(buf),
           "ok poke_playback_start 0x%08X base=0x%08X rows=%zu..%zu cols=%d on_end=%s",
           tpc, base, start_row, end_row, n_cols, on_end_str.c_str());
  write_ack(buf);
 }
 else if (cmd == "poke_playback_stop") {
  if (!poke_playback_running) {
   write_ack("ok poke_playback_stop (not active)");
   return;
  }
  uint32_t tpc = poke_playback_pc;
  poke_triggers.erase(tpc);
  poke_playback_running = false;
  poke_playback_pc = 0;
  poke_playback_halt_pending = false;
  update_cpu_hook();
  char buf[96];
  snprintf(buf, sizeof(buf), "ok poke_playback_stop 0x%08X", tpc);
  write_ack(buf);
 }
 else if (cmd == "poke_playback_status") {
  if (!poke_playback_running) {
   write_ack("poke_playback inactive");
   return;
  }
  auto it = poke_triggers.find(poke_playback_pc);
  if (it == poke_triggers.end() || !it->second.is_playback) {
   write_ack("poke_playback inactive (stale)");
   return;
  }
  const auto& t = it->second;
  char buf[256];
  snprintf(buf, sizeof(buf),
           "poke_playback pc=0x%08X base=0x%08X row=%zu/%zu start=%zu end=%zu "
           "triggers_seen=%llu pokes=%llu done=%s on_end=%s",
           poke_playback_pc, t.base_addr, t.cur_row, t.end_row,
           t.start_row, t.end_row,
           (unsigned long long)t.trigger_hits,
           (unsigned long long)t.pokes_performed,
           t.done ? "true" : "false",
           (t.on_end == PPOE_HALT) ? "halt" :
           (t.on_end == PPOE_LOOP) ? "loop" : "hold");
  write_ack(buf);
 }
 else {
  write_ack("error unknown command: " + cmd);
 }
}

static bool check_action_file(void)
{
 // Content-based change detection: read file and check if header line changed.
 // This avoids DrvFS stat() caching issues.
 std::ifstream f(action_file);
 if (!f.is_open())
  return false;

 // Read first line (header comment with sequence number, e.g. "# 5.....")
 std::string header;
 if (!std::getline(f, header)) {
  f.close();
  return false;
 }
 // Strip \r
 if (!header.empty() && header.back() == '\r')
  header.pop_back();

 if (header == last_action_header) {
  f.close();
  return false;
 }
 last_action_header = header;

 // Process remaining lines as commands
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
 frames_to_advance = 0;  // Start PAUSED -- external tool must send "run" or "frame_advance"
 last_action_header.clear();

 // Write initial ack so external tools know we're ready
 write_ack("ready frame=0");

 fprintf(stderr, "Automation: initialized\n");
 fprintf(stderr, "  Action file: %s\n", action_file.c_str());
 fprintf(stderr, "  Ack file:    %s\n", ack_file.c_str());
}

void Automation_Poll(const MDFN_Surface* surface, const MDFN_Rect* rect, const int32* lw)
{
 if (!automation_active)
  return;

 frame_counter++;

 // Cache framebuffer for instant screenshots (no frame advance needed).
 if (surface && rect && surface->pixels) {
  // Reallocate if dimensions changed
  if (!cached_fb_pixels || cached_fb_w != surface->w || cached_fb_h != surface->h
      || cached_fb_pitch != surface->pitchinpix) {
   delete[] cached_fb_pixels;
   delete[] cached_fb_lw;
   cached_fb_pixels = new uint32_t[surface->pitchinpix * surface->h];
   cached_fb_lw = new int32[surface->h];
   cached_fb_w = surface->w;
   cached_fb_h = surface->h;
   cached_fb_pitch = surface->pitchinpix;
  }
  memcpy(cached_fb_pixels, surface->pixels, surface->pitchinpix * surface->h * sizeof(uint32_t));
  if (lw) memcpy(cached_fb_lw, lw, surface->h * sizeof(int32));
  else memset(cached_fb_lw, 0, surface->h * sizeof(int32));
  cached_fb_rect = *rect;
  cached_fb_format = surface->format;
  cached_fb_valid = true;
 }

 // Check run_to_frame
 if (run_to_frame_target >= 0 && (int64_t)frame_counter >= run_to_frame_target) {
  frames_to_advance = 0;  // Pause
  run_to_frame_target = -1;
  write_ack("done run_to_frame frame=" + std::to_string(frame_counter));
 }

 // Input playback: process events up to and including the current frame.
 // Apply ALL events that should have fired by now (not just exact matches),
 // so that frame=0 events work even if Poll first runs at frame 1.
 if (playback_active) {
  uint64_t rel_frame = frame_counter - playback_base_frame;
  while (playback_index < playback_events.size()
         && playback_events[playback_index].frame <= rel_frame) {
   const auto& ev = playback_events[playback_index];
   if (ev.press) {
    input_buttons |= (1 << ev.button);
   } else {
    input_buttons &= ~(1 << ev.button);
   }
   input_override = true;
   playback_index++;
  }
  // Auto-stop when all events consumed
  if (playback_index >= playback_events.size()) {
   playback_active = false;
  }
 }

 // Per-frame memory sampler: dump region to binary file each frame
 if (mem_sample_file && mem_sample_frames > 0) {
  uint8_t sample_buf[0x10000]; // 64KB max (matches command handler cap)
  for (uint32_t i = 0; i < mem_sample_size; i++) {
   sample_buf[i] = MDFN_IEN_SS::Automation_ReadMem8(mem_sample_addr + i);
  }
  fwrite(sample_buf, 1, mem_sample_size, mem_sample_file);
  mem_sample_frames--;
  if (mem_sample_frames == 0) {
   fclose(mem_sample_file);
   mem_sample_file = nullptr;
   frames_to_advance = 0; // pause emulation
   char buf[256];
   snprintf(buf, sizeof(buf), "done mem_sample frames=%lld addr=0x%08X size=0x%X",
            (long long)mem_sample_total, mem_sample_addr, mem_sample_size);
   write_ack(buf);
  }
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

 // Unstick CPU if caught inside VBlank handler with interrupts masked.
 // The SH-2's VBlank interrupt is delivered at IRL level 15. If the frame
 // boundary catches the CPU inside the VBlank handler (which runs with
 // SR.I=15 from interrupt acceptance), subsequent frame_advance calls
 // will be stuck because 15 > 15 is false and VBlank can never fire.
 // Fix: lower SR.I to 0 at frame-level pause so the next frame_advance
 // can process VBlank normally. SetMasterSR calls RecalcPendingIntPEX().
 if (frames_to_advance == 0) {
  uint32_t sr = MDFN_IEN_SS::Automation_GetMasterSR();
  if (((sr >> 4) & 0xF) >= 0xF) {
   MDFN_IEN_SS::Automation_SetMasterSR(sr & ~0xF0);
  }
 }

 // Block emulation when paused -- spin-wait until a command unpauses us.
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
  check_exit_requested();
 }
}

void Automation_Kill(void)
{
 // Send shutdown ack only if we were actively running (not already
 // cleared by check_exit_requested during a window-close).
 if (automation_active)
  write_ack("shutdown frame=" + std::to_string(frame_counter));
 automation_active = false;

 // Unconditionally clean up all resources — check_exit_requested may
 // have cleared automation_active before we get here, but file handles
 // still need flushing and closing.
 close_wp_log();
 if (rwp_log) { fclose(rwp_log); rwp_log = nullptr; }
 if (bp_log) { fclose(bp_log); bp_log = nullptr; }
 if (exc_log) { fclose(exc_log); exc_log = nullptr; }
 poke_triggers.clear();
 poke_playback_running = false;
 poke_playback_pc = 0;
 poke_playback_halt_pending = false;
 if (unified_trace_file) { fclose(unified_trace_file); unified_trace_file = nullptr; }
 if (pc_trace_file) { fclose(pc_trace_file); pc_trace_file = nullptr; }
 if (input_trace_file) { fclose(input_trace_file); input_trace_file = nullptr; }
 if (mem_sample_file) { fclose(mem_sample_file); mem_sample_file = nullptr; }
 delete[] cached_fb_pixels;  cached_fb_pixels = nullptr;
 delete[] cached_fb_lw;      cached_fb_lw = nullptr;
 cached_fb_valid = false;
 MDFN_IEN_SS::Automation_CDLStop();
 MDFN_IEN_SS::Automation_DisableMemProfile();
 MDFN_IEN_SS::Automation_DisableMemReadProfile();
 MDFN_IEN_SS::Automation_DisableDMATrace();
 MDFN_IEN_SS::Automation_DisableCallTrace();
 MDFN_IEN_SS::Automation_DisableInsnTrace();
}

bool Automation_IsActive(void)
{
 return automation_active;
}

bool Automation_SuppressRaise(void)
{
 return automation_active;
}

void Automation_LogSystemCommand(const char* cmd_name)
{
 if (!automation_active || !input_trace_file)
  return;
 fprintf(input_trace_file, "frame=%llu SYSCMD %s\n",
  (unsigned long long)frame_counter, cmd_name);
 fflush(input_trace_file);
}

bool Automation_GetInput(unsigned port, uint8_t* data, unsigned data_size)
{
 // OR automation button presses into the existing data.
 // NOTE: This is additive -- real keyboard input is NOT suppressed.
 // Both automation and keyboard presses contribute to the final state.
 if (automation_active && input_override && port == 0 && data_size >= 2) {
  data[0] |= (uint8_t)(input_buttons & 0xFF);
  data[1] |= (uint8_t)((input_buttons >> 8) & 0xFF);
 }

 // Input tracing: log combined button state (keyboard + automation) AFTER override.
 // This captures the full input picture — what the game actually sees.
 if (automation_active && input_trace_file && port == 0 && data_size >= 2) {
  uint16_t cur = data[0] | ((uint16_t)data[1] << 8);
  if (cur != last_traced_input) {
   static const struct { int bit; const char* name; } btn_names[] = {
    {BTN_Z, "Z"}, {BTN_Y, "Y"}, {BTN_X, "X"}, {BTN_R, "R"},
    {BTN_UP, "UP"}, {BTN_DOWN, "DOWN"}, {BTN_LEFT, "LEFT"}, {BTN_RIGHT, "RIGHT"},
    {BTN_B, "B"}, {BTN_C, "C"}, {BTN_A, "A"}, {BTN_START, "START"}, {BTN_L, "L"},
   };
   uint16_t changed = cur ^ last_traced_input;
   for (int i = 0; i < 13; i++) {
    if (changed & (1 << btn_names[i].bit)) {
     bool pressed = (cur & (1 << btn_names[i].bit)) != 0;
     fprintf(input_trace_file, "frame=%llu %s %s\n",
      (unsigned long long)frame_counter,
      pressed ? "PRESS" : "RELEASE",
      btn_names[i].name);
    }
   }
   fflush(input_trace_file);
   last_traced_input = cur;
  }
 }

 return (automation_active && input_override && port == 0);
}

bool Automation_ConsumePendingShowWindow(void)
{
 // Kept as stub — show_window now calls SDL directly from command handler
 return false;
}

bool Automation_ConsumePendingHideWindow(void)
{
 // Kept as stub — hide_window now calls SDL directly from command handler
 return false;
}

// Called from ss.cpp (BusRW_DB_CS3 / DMA_Write) when a memory write hits the watched address.
// pc = current CPU PC, addr = full address written, old_val/new_val = 32-bit values.
// pr = return address register (caller context).
// source = "CPU" or "DMA" -- indicates write origin.
// This runs inline in the CPU execution path -- must NOT block.
void Automation_WatchpointHit(uint32_t pc, uint32_t addr, uint32_t old_val, uint32_t new_val, uint32_t pr, const char* source)
{
 if (!watchpoint_active || !automation_active)
  return;

 // Log hit to watchpoint log file (append mode)
 if (!wp_log) {
  std::string path = auto_base_dir + "/watchpoint_hits.txt";
  wp_log = fopen(path.c_str(), "w");
  if (wp_log) {
   if (watchpoint_filter_active)
    fprintf(wp_log, "# Watchpoint hits for addr 0x%08X (filter: eq 0x%08X)\n", watchpoint_addr, watchpoint_filter_value);
   else
    fprintf(wp_log, "# Watchpoint hits for addr 0x%08X\n", watchpoint_addr);
  }
 }
 if (wp_log) {
  fprintf(wp_log, "pc=0x%08X pr=0x%08X addr=0x%08X old=0x%08X new=0x%08X source=%s frame=%llu\n",
   pc, pr, addr, old_val, new_val, source, (unsigned long long)frame_counter);
  fflush(wp_log);
 }

 char msg[256];
 snprintf(msg, sizeof(msg),
  "hit watchpoint pc=0x%08X pr=0x%08X addr=0x%08X old=0x%08X new=0x%08X source=%s frame=%llu",
  pc, pr, addr, old_val, new_val, source, (unsigned long long)frame_counter);

 // Log mode: write full context to log file, don't pause
 if (watchpoint_log_mode) {
  if (wp_log) {
   std::string regs = MDFN_IEN_SS::Automation_DumpRegs();
   std::string stack = MDFN_IEN_SS::Automation_CallStack(0x400);
   fprintf(wp_log, "--- %s ---\n%s\n%s\n", msg, regs.c_str(), stack.c_str());
   fflush(wp_log);
  }
  return;
 }

 // Pause mode: write ack and spin-wait.
 // This spin-waits inside the memory bus write path (BusRW_DB_CS3).
 // Safe because Mednafen is single-threaded.
 std::string full_msg;
 if (run_to_frame_target >= 0) {
  full_msg = "done run_to_frame frame=" + std::to_string(frame_counter)
           + " STOPPED_BY_WATCHPOINT " + msg;
  run_to_frame_target = -1;
  frames_to_advance = 0;
 } else if (run_to_cycle_target >= 0) {
  full_msg = "done run_to_cycle STOPPED_BY_WATCHPOINT " + std::string(msg);
  run_to_cycle_target = -1;
  frames_to_advance = 0;
 } else {
  full_msg = msg;
 }

 full_msg += "\n" + MDFN_IEN_SS::Automation_DumpRegs();
 full_msg += "\n" + MDFN_IEN_SS::Automation_CallStack(0x400);
 write_ack(full_msg);

 watchpoint_paused = true;
 while (watchpoint_paused && automation_active) {
#ifdef WIN32
  Sleep(10);
#else
  struct timespec ts = {0, 10000000}; // 10ms
  nanosleep(&ts, NULL);
#endif
  check_action_file();
  check_exit_requested();
 }
}

void Automation_ReadWatchpointHit(uint32_t pc, uint32_t addr, uint32_t val, uint32_t pr)
{
 if (!read_watchpoint_active || !automation_active)
  return;

 // Log hit to file (always, both modes)
 if (!rwp_log) {
  std::string path = auto_base_dir + "/read_watchpoint_hits.txt";
  rwp_log = fopen(path.c_str(), "w");
  if (rwp_log)
   fprintf(rwp_log, "# Read watchpoint hits for addr 0x%08X\n", read_watchpoint_addr);
 }

 char msg[256];
 snprintf(msg, sizeof(msg),
  "hit read_watchpoint pc=0x%08X pr=0x%08X addr=0x%08X val=0x%08X frame=%llu",
  pc, pr, addr, val, (unsigned long long)frame_counter);

 // Log mode: write full context to log file, don't pause
 if (read_watchpoint_log_mode) {
  if (rwp_log) {
   std::string regs = MDFN_IEN_SS::Automation_DumpRegs();
   std::string stack = MDFN_IEN_SS::Automation_CallStack(0x400);
   fprintf(rwp_log, "--- %s ---\n%s\n%s\n", msg, regs.c_str(), stack.c_str());
   fflush(rwp_log);
  }
  return;
 }

 // Pause mode: log summary line then ack + spin-wait
 if (rwp_log) {
  fprintf(rwp_log, "pc=0x%08X pr=0x%08X addr=0x%08X val=0x%08X frame=%llu\n",
   pc, pr, addr, val, (unsigned long long)frame_counter);
  fflush(rwp_log);
 }

 std::string full_msg;
 if (run_to_frame_target >= 0) {
  full_msg = "done run_to_frame frame=" + std::to_string(frame_counter)
           + " STOPPED_BY_READ_WATCHPOINT " + msg;
  run_to_frame_target = -1;
  frames_to_advance = 0;
 } else if (run_to_cycle_target >= 0) {
  full_msg = "done run_to_cycle STOPPED_BY_READ_WATCHPOINT " + std::string(msg);
  run_to_cycle_target = -1;
  frames_to_advance = 0;
 } else {
  full_msg = msg;
 }

 full_msg += "\n" + MDFN_IEN_SS::Automation_DumpRegs();
 full_msg += "\n" + MDFN_IEN_SS::Automation_CallStack(0x400);
 write_ack(full_msg);

 read_watchpoint_paused = true;
 while (read_watchpoint_paused && automation_active) {
#ifdef WIN32
  Sleep(10);
#else
  struct timespec ts = {0, 10000000}; // 10ms
  nanosleep(&ts, NULL);
#endif
  check_action_file();
  check_exit_requested();
 }
}

static const char* exception_name(unsigned exnum)
{
 switch (exnum) {
  case 2: return "illegal_instruction";
  case 3: return "slot_illegal";
  case 4: return "cpu_address_error";
  case 5: return "dma_address_error";
  case 6: return "nmi";
  case 7: return "user_break";
  case 8: return "trap";
  default: return "unknown";
 }
}

void Automation_ExceptionHit(unsigned exnum, unsigned vecnum, uint32_t pc, uint32_t sr,
                              uint32_t r15, uint32_t pr, uint32_t vbr, uint32_t handler_pc)
{
 if (!automation_active || exception_mode == EXC_DISABLE)
  return;

 // Don't report if already paused on another event (e.g., watchpoint hit
 // on the stack push inside the Exception macro itself)
 if (watchpoint_paused || read_watchpoint_paused || exception_paused)
  return;

 char msg[512];
 snprintf(msg, sizeof(msg),
  "hit exception type=%s exnum=%u vecnum=0x%02X pc=0x%08X sr=0x%08X "
  "r15=0x%08X pr=0x%08X vbr=0x%08X handler=0x%08X frame=%llu",
  exception_name(exnum), exnum, vecnum, pc, sr, r15, pr, vbr, handler_pc,
  (unsigned long long)frame_counter);

 if (exception_mode == EXC_LOG) {
  // Log mode: write full context to file, don't pause
  if (!exc_log) {
   std::string path = auto_base_dir + "/exception_hits.txt";
   exc_log = fopen(path.c_str(), "w");
   if (exc_log) fprintf(exc_log, "# SH-2 exception log\n");
  }
  if (exc_log) {
   std::string regs = MDFN_IEN_SS::Automation_DumpRegs();
   std::string stack = MDFN_IEN_SS::Automation_CallStack(0x400);
   fprintf(exc_log, "--- %s ---\n%s\n%s\n", msg, regs.c_str(), stack.c_str());
   fflush(exc_log);
  }
  return;
 }

 // Enable mode: ack with context and pause
 std::string full_msg;
 if (run_to_frame_target >= 0) {
  full_msg = "done run_to_frame frame=" + std::to_string(frame_counter)
           + " STOPPED_BY_EXCEPTION " + msg;
  run_to_frame_target = -1;
  frames_to_advance = 0;
 } else if (run_to_cycle_target >= 0) {
  full_msg = "done run_to_cycle STOPPED_BY_EXCEPTION " + std::string(msg);
  run_to_cycle_target = -1;
  frames_to_advance = 0;
 } else {
  full_msg = msg;
  frames_to_advance = 0;
 }

 full_msg += "\n" + MDFN_IEN_SS::Automation_DumpRegs();
 full_msg += "\n" + MDFN_IEN_SS::Automation_CallStack(0x400);
 write_ack(full_msg);

 exception_paused = true;
 while (exception_paused && automation_active) {
#ifdef WIN32
  Sleep(10);
#else
  struct timespec ts = {0, 10000000}; // 10ms
  nanosleep(&ts, NULL);
#endif
  check_action_file();
  check_exit_requested();
 }
}

// Execute all pokes configured for this trigger. For static triggers, writes
// the fixed list. For playback triggers, writes the current row's bytes and
// advances the row cursor (honoring on_end: halt/loop/hold).
// Runs fully inside the debug hook -- no SH-2 cycles intervene between writes.
static void perform_pokes_from_trigger(PokeTrigger& trig, uint32_t trigger_pc)
{
 trig.trigger_hits++;

 if (!trig.is_playback) {
  for (const auto& p : trig.static_pokes) {
   if (p.width == 8) {
    MDFN_IEN_SS::Automation_WriteMem8(p.addr, (uint8_t)(p.value & 0xFF));
   } else if (p.width == 16) {
    MDFN_IEN_SS::Automation_WriteMem8(p.addr,     (uint8_t)((p.value >> 8) & 0xFF));
    MDFN_IEN_SS::Automation_WriteMem8(p.addr + 1, (uint8_t)(p.value & 0xFF));
   } else { // 32
    MDFN_IEN_SS::Automation_WriteMem8(p.addr,     (uint8_t)((p.value >> 24) & 0xFF));
    MDFN_IEN_SS::Automation_WriteMem8(p.addr + 1, (uint8_t)((p.value >> 16) & 0xFF));
    MDFN_IEN_SS::Automation_WriteMem8(p.addr + 2, (uint8_t)((p.value >>  8) & 0xFF));
    MDFN_IEN_SS::Automation_WriteMem8(p.addr + 3, (uint8_t)(p.value & 0xFF));
   }
   trig.pokes_performed++;
  }
  return;
 }

 // Playback mode
 if (trig.done) return;
 if (trig.cur_row >= trig.end_row || trig.cur_row >= trig.rows.size()) return;

 const auto& row = trig.rows[trig.cur_row];
 for (size_t c = 0; c < trig.columns.size(); c++) {
  const auto& col   = trig.columns[c];
  const auto& bytes = row[c];
  uint32_t dst = trig.base_addr + col.offset;
  for (size_t i = 0; i < bytes.size(); i++)
   MDFN_IEN_SS::Automation_WriteMem8(dst + i, bytes[i]);
  trig.pokes_performed++;
 }

 // Advance cursor for the next hit.
 if (trig.cur_row + 1 >= trig.end_row) {
  if (trig.on_end == PPOE_LOOP) {
   trig.cur_row = trig.start_row;
  } else if (trig.on_end == PPOE_HOLD) {
   // Stay on last valid row -- next hit re-pokes the same values
  } else { // PPOE_HALT
   // Park the cursor at end_row (past-the-end) so status reads
   // "row=end/end done=true" unambiguously instead of "end-1/end".
   trig.cur_row = trig.end_row;
   trig.done    = true;
   poke_playback_halt_pending = true;
   poke_playback_halt_pc      = trigger_pc;
  }
 } else {
  trig.cur_row++;
 }
}

bool Automation_DebugHook(uint32_t pc)
{
 // PC trace -- record every instruction's PC to file
 if (pc_trace_active && pc_trace_file) {
  fwrite(&pc, 4, 1, pc_trace_file);
 }

 // Poke triggers fire before any pause logic -- they write memory and
 // continue. Same pc/pc-2 fallback as breakpoints (delayed-branch pipeline
 // quirk: PC arrives as target+2 after JSR/BSR/JMP).
 if (!poke_triggers.empty()) {
  auto it = poke_triggers.find(pc);
  uint32_t poke_tpc = pc;
  if (it == poke_triggers.end()) {
   auto it2 = poke_triggers.find(pc - 2);
   if (it2 != poke_triggers.end()) { it = it2; poke_tpc = pc - 2; }
  }
  if (it != poke_triggers.end())
   perform_pokes_from_trigger(it->second, poke_tpc);
 }

 // Check breakpoints (O(1) lookup via unordered_set)
 // Also check pc-2: after JSR/BSR/JMP/RTS, the SH-2 pipeline fetch stage
 // advances PC past the first instruction at the branch target.
 // UCDelayBranch does FetchIF_ForceIBufferFill() which sets PC = target+2.
 // So when this hook fires, PC is already target+2 and a breakpoint set at
 // the exact branch target ('target') would miss without this fallback.
 bool bp_hit = breakpoints.count(pc) > 0;
 uint32_t bp_addr = pc;
 if (!bp_hit && breakpoints.count(pc - 2) > 0) {
  bp_hit = true;
  bp_addr = pc - 2;
 }

 // Check cycle target
 bool cycle_hit = false;
 if (run_to_cycle_target >= 0 && get_cycle() >= run_to_cycle_target) {
  cycle_hit = true;
  run_to_cycle_target = -1;
 }

 // Instruction step countdown
 if (instructions_to_step > 0)
  instructions_to_step--;

 // Poke playback halt: consumed once, treated as a pause source
 bool poke_halt = poke_playback_halt_pending;
 uint32_t poke_halt_pc_local = poke_playback_halt_pc;
 if (poke_halt) poke_playback_halt_pending = false;

 // Determine if we should pause
 bool should_pause = bp_hit || cycle_hit || (instructions_to_step == 0) || poke_halt;
 if (!should_pause)
  return false;

 // Breakpoint log mode: log full context to file, don't pause
 if (bp_hit && breakpoint_log_mode) {
  if (!bp_log) {
   std::string path = auto_base_dir + "/breakpoint_hits.txt";
   bp_log = fopen(path.c_str(), "w");
   if (bp_log)
    fprintf(bp_log, "# Breakpoint hit log (log mode)\n");
  }
  if (bp_log) {
   std::string regs = MDFN_IEN_SS::Automation_DumpRegs();
   std::string stack = MDFN_IEN_SS::Automation_CallStack(0x400);
   fprintf(bp_log, "--- break pc=0x%08X addr=0x%08X frame=%llu ---\n%s\n%s\n",
    pc, bp_addr, (unsigned long long)frame_counter, regs.c_str(), stack.c_str());
   fflush(bp_log);
  }
  // If ONLY a breakpoint hit (not also cycle/step), don't pause
  if (!cycle_hit && instructions_to_step != 0)
   return false;
 }

 // Pause at instruction level
 instruction_paused = true;
 instructions_to_step = -1;

 // NOTE on PC values:
 // This hook fires BEFORE CPU[0].Step() in RunLoop_INLINE (ss.cpp).
 // At this point, CPU[0].PC is the address of the instruction about to execute,
 // which is the correct value for breakpoint matching and step reporting.
 // For step completion, we use Automation_GetMasterPC() which returns CPU[0].PC.
 uint32_t real_pc = MDFN_IEN_SS::Automation_GetMasterPC();

 char msg[256];
 if (bp_hit)
  snprintf(msg, sizeof(msg), "break pc=0x%08X addr=0x%08X frame=%llu",
   pc, bp_addr, (unsigned long long)frame_counter);
 else if (cycle_hit)
  snprintf(msg, sizeof(msg), "done run_to_cycle pc=0x%08X frame=%llu",
   real_pc, (unsigned long long)frame_counter);
 else if (poke_halt)
  snprintf(msg, sizeof(msg), "done poke_playback pc=0x%08X trigger=0x%08X frame=%llu",
   real_pc, poke_halt_pc_local, (unsigned long long)frame_counter);
 else
  snprintf(msg, sizeof(msg), "done step pc=0x%08X frame=%llu",
   real_pc, (unsigned long long)frame_counter);

 // Auto-context: append registers + call stack to every break event
 std::string full_msg = msg;
 full_msg += "\n" + MDFN_IEN_SS::Automation_DumpRegs();
 full_msg += "\n" + MDFN_IEN_SS::Automation_CallStack(0x400);
 write_ack(full_msg);

 // Spin-wait for commands while paused at instruction level.
 // This blocks the SH-2 CPU loop. Commands like dump_regs, dump_mem,
 // step, run, breakpoint all work during this pause because
 // check_action_file -> process_command handles them.
 while (instruction_paused && automation_active) {
#ifdef WIN32
  Sleep(10);
#else
  struct timespec ts = {0, 10000000}; // 10ms
  nanosleep(&ts, NULL);
#endif
  check_action_file();
  check_exit_requested();
 }

 return false;
}
