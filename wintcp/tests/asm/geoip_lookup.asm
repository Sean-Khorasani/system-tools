; geoip_lookup.asm
; SPDX-License-Identifier: Apache-2.0
; Reference assembly notes for the GeoIP tree-walk optimization.
;
; This file is DOCUMENTATION ONLY: it is not assembled or linked by any
; build (CMakeLists.txt and build.bat compile opt_functions.cpp only).
; The shipped implementation uses compiler intrinsics in
; opt_functions.cpp, which emit equivalent SSE2/BSWAP code with
; maintainable C++ semantics. Do NOT add this file to a vcxproj as
; "Microsoft Macro Assembler": the stub below always returns false.
;
; CONCEPTUAL OPTIMIZATION:
; The original C++ walks one address bit per iteration:
;   for (depth = 0; depth < bitCount; depth++) {
;       bit = (bits[depth/8] >> (7 - (depth%8))) & 1;
;       NodeRecord(node, bit, &record);  // function call overhead
;       // boundary checks and branching
;   }
;
; The intrinsic version instead:
; 1. Replaces division/modulo with shift/mask (depth>>3, depth&7).
; 2. Inlines the 24/28/32-bit record extraction (no call).
; 3. Uses BSWAP (via _byteswap_*) for the 32-bit big-endian reads.
;
; Correct x64 bit extraction for bit 'depth' in array 'bits':
;   mov    eax, depth
;   shr    eax, 3          ; depth / 8  -> byte index
;   movzx  eax, byte ptr [bits + rax]
;   mov    ecx, depth
;   and    ecx, 7          ; depth % 8
;   xor    ecx, 7          ; 7 - (depth % 8)
;   bt     eax, ecx        ; CF = the address bit
;   setc   al              ; al = 0/1  (BT alone only sets CF!)
;
; (An earlier draft of this note showed `bt eax, ecx` without SETC and
; claimed 8 bits per iteration via a lookup table that was never
; provided; both were wrong and are corrected above.)
;
; The intrinsic-based C++ implementation in opt_functions.cpp
; achieves the same result with better maintainability.

Option Strict
Option Prologue:None
Option Epilogue:None

.code

; Placeholder procedure to make the file linkable if assembled
ResolveOffsetSimd PROC
    ; This is a reference stub. The real implementation is in
    ; opt_functions.cpp using compiler intrinsics.
    xor eax, eax
    ret
ResolveOffsetSimd ENDP

END
