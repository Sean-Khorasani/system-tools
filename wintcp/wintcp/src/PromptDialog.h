// PromptDialog.h
// A one-line text prompt (name a preset, type a bookmark note).
//
// WHY IT EXISTS. Three separate features need the same thing - "ask the user
// for a short string, then act on it": saving a preset, naming a bookmark, and
// editing a note. Writing that as three near-identical dialog procs is how
// they drift apart (one forgets to strip whitespace, one forgets the
// length cap, one forgets the message loop), and it is why this is one
// function rather than three.
//
// It is a dialog, not a MessageBox, for one concrete reason: MessageBox has no
// text input, and the three near-misses people reach for instead are all
// worse. SHGetFilePath gets the wrong answer, a custom MessageBox subclass
// cannot host an edit control reliably, and reusing a stray GetOpenFileName
// for a name is a user-hostile joke.
//
// No dependency on DetailsDialog: this is a modal utility, and borrowing the
// details window's lifetime and model plumbing would couple two things that
// have nothing to do with each other.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>

#include <string>

namespace wintcp {

// Show a modal single-line prompt. Returns true when the user confirmed,
// with the typed text in '*out'. Returns false on Cancel or on failure, in
// which case '*out' is untouched - so a caller cannot act on a stale value
// from a previous prompt.
//
// 'initial' is pre-filled and selected, so typing replaces it (the common
// case when re-naming) rather than appending to it.
//
// 'maxChars' bounds the edit control AND the returned string. It is a
// parameter rather than a constant because the three callers have genuinely
// different limits (preset names, notes) and silently sharing one number
// would either truncate a legitimate note or allow a preset name that the
// preset store will later reject.
bool PromptForText(HWND owner, const wchar_t* title, const wchar_t* label,
                   const wchar_t* initial, size_t maxChars,
                   std::wstring* out);

}  // namespace wintcp
