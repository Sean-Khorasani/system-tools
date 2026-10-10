// opt_functions.h
// SPDX-License-Identifier: Apache-2.0
//
// SHIM: the optimized implementations now live in the product tree at
// wintcp/src/Opt.h so that the A/B bench and the shipped binary compile the
// SAME code. Adding a second copy here would let the two drift, and a number
// measured by the bench would stop being the number the product gets.
//
// This file is kept so every "#include "opt_functions.h"" in the bench
// still resolves; it adds nothing.
#pragma once

#include "Opt.h"
