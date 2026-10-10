// opt_functions.cpp
// SPDX-License-Identifier: Apache-2.0
//
// SHIM: the implementations moved to wintcp/src/Opt.cpp. The bench's static
// library (wintcp_asm_opt.lib) needs the symbols, so this translation unit
// pulls in the product one - one copy of the code, compiled into both
// programs, so a measurement and a shipped build cannot disagree.

#include "Opt.cpp"
