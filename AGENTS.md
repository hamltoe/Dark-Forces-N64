# Project Summary
Software port of "Star Wars Dark Forces" 1995 PC game ported to original N64 game console

# Porting Project guidelines
- Convert existing files in place
- Instead of adding to codebase, seek to modify existing functionality
- No need to define if "N64" since this will always be target platform
- Avoid use of "shims" when possible,instead modify original code to work on N64 hardware

# Building Project
WSL (Windows Subsystem for LInux) instead of developing natively on Linux. Use "WSL" command in terminal to enter linux subsystem. Once buld runs on N64, debug.log file is created in project root

## Build Flags
- DEBUG=1 — Debug mode allows on screen printing and writes logging data to debug.log file to be generated
- UPLOAD=1 — Invoke upload script to load built ROM onto N64 Flash Cart (not yet implemented)

## Desired Debug Build Instructions
To compile with debug support:
1. Open WSL terminal
2. Navigate to project directory: `cd /mnt/c/Users/ton_o\Dark-Forces-N64`
3. Run make with debug flag: `make DEBUG=1`
4. build will generate debug.log file in project root when running on N64
5. Debug output includes screen printing and logging information for troubleshooting

# Target Platform Hardware
N64 game console with 4MB (8MB wth expansion pak) RAM, RISC based MIPS R4300i-series processor. 8KB of L1 cache, no L2 cache. Lacks branch prediction and will always executes first instruction after any IF or IF ELSE statement, so make first instruction simple, no calling into DRAM
Graphic processors: SGI 62.5 MHz 64-bit RCP (Reality Co-Processor), with 2 sub-processors:
- RSP (Reality Signal Processor) Controls 3D graphics and audio functionalities
- RDP (Reality Drawing Processor) Rasterizer handles all pixel drawing operations in hardware

## Platform Bottleneck
Bottleneck is 250MB/s memory bandwidth of Rambus DRAM (RDRAM). Latency of around 640ns for single call. Use Direct Memory Accees (DMA), bandwidth can be as high as 562.5 MB/s

## Data Type Considerations
N64 FPU flushes denormalized floats to zero instead of following IEEE 754. Small values near zero get silently discarded. This causes division by zero, NaN propagation, or values snapping unexpectedly, especially in physics and audio calculations. fix is to clamp small values, add epsilon guards before division, or use fixed-point math instead

## Key Project Paths
- `TheForceEngine` — Source project files for original Dark Forces engine that we are porting tothe N64
- `GOBs/` — Original game asset files neeeded by Force Engine
- `LFDs`  — Original game asset files with .LFD extension

# Coding Libraries
- `Libdragon/` — https://github.com/DragonMinded/libdragon.git

## Reference examples for best practice
- `libdragon/examples/` — Audio, input, rendering, filesystem demos
*Never include any example content in main project*

# "Sparse Speaker" Mode
## Core Rules
Respond like you "Sparse Speaker". Cut articles, fillers, pleasantries. But keep all technical substance

## Grammar rules
- Drop all articles (a, an, the)
- Drop fillers (just, really, basically, actually, simply)
- Drop pleasantries (sure, basically, actually, simply)
- Strip leading self-correction or hesitation marker (e.g. "wait," "actually," "scratch that")
- Short synonyms (big not extensive, fix not "implement solution for")
- Code blocks unchanged. "Sparse Speaker" speak around code, not in code
- Error message quoted exactly. "Sparse Speaker" only for explanation
- Fragments fine. No need full sentence
- Be brief when explaining. Never shorten code
- Short answers, full code. No placeholder comments
- Skip recaps and summaries. Show complete code