// StreamCapture.h
// Drives pktmon to capture one TCP connection, converts the ETL to pcapng,
// parses it and reassembles the stream. This is the only part of
// the follow-stream feature that touches the filesystem or spawns processes.
//
// WHAT IT DOES, IN ORDER:
//   1. pktmon filter remove        (the filter set is global and persistent)
//   2. pktmon filter add ... -t TCP -p <localport> -p <remoteport> [-i <ip>]
//   3. pktmon start --capture --pkt-size 0 --file-name <tmp>.etl
//   4. [caller shows a "capturing" window, the user browses for a few seconds]
//   5. pktmon stop                (flushes and merges the ETL)
//   6. pktmon etl2pcap <tmp>.etl --out <tmp>.pcapng
//   7. read + ParsePcapng + ReassembleStream
//   8. delete both temp files
//
// --pkt-size 0 is not optional: the default truncates each packet and a
// truncated stream cannot be reassembled. See todo.md 3.0.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <string>

#include "Connection.h"
#include "TcpReasm.h"

namespace wintcp {

// One connection to follow.
struct CaptureTarget {
    unsigned char localAddr[16] = {0};
    unsigned char remoteAddr[16] = {0};
    uint16_t localPort = 0;
    uint16_t remotePort = 0;
    bool ipV4 = true;
    // Display string for the window title, e.g. "10.0.0.92:52417".
    std::wstring label;
};

// Everything the follow-stream window needs to render honestly.
struct CaptureResult {
    bool ok = false;
    std::wstring error;

    ReasmResult toServer;
    ReasmResult toClient;
    ReasmStats stats;

    // Raw counters, so the window can say what happened rather than showing
    // an unexplained blank pane.
    size_t packetsParsed = 0;
    size_t flowRecords = 0;
    size_t blocksSeen = 0;
    size_t fileBytes = 0;
    unsigned linkType = 0;
    bool elevated = false;      // false => pktmon refused; see error
    bool complete = true;       // false => SYN and FIN both seen

    // Diagnostic line naming the stage that failed, for the "why is this
    // empty" case: "pktmon start", "etl2pcap", "parse", "reassemble".
    std::wstring stage;

    // Endpoint strings for the two reassembly directions, in the SAME order as
    // toServer / toClient - so dir1Label is the peer whose bytes are in
    // toServer.
    //
    // Filled here because this is where the order is decided: ReassembleStream
    // canonicalises the pair (lower address first, ties broken on the lower
    // port) and that sort is internal to TcpReasm. A caller cannot derive it,
    // and guessing would label the two halves of every stream the wrong way
    // round - which is the one mistake a stream dump must not make.
    std::wstring dir1Label;
    std::wstring dir2Label;
};

// True when this process can drive pktmon at all. Checks the token rather
// than trying, so the UI can disable the menu item up front instead of
// failing after the user has waited.
bool CaptureAvailable(std::wstring* whyNot = nullptr);

// Build the target for a connection row. Takes the addresses from the
// BINARY fields, not the printable ones: the endpoint strings carry the
// port ("93.184.216.34:443", "[...]:443"), so parsing them rejects every
// row. The binary fields are what TcpTable fills and what SetTcpEntry and
// the other per-connection actions use; there is no string form that can
// fail to parse. IPv4 lands raw in the first 4 bytes, matching the layout
// ParseIpTcp produces for v4 packets, so reassembly keys agree.
CaptureTarget MakeCaptureTarget(const Connection& c);

// Install a filter and start capturing. Returns false and sets 'error' if
// pktmon could not be started. On success the capture is running and
// StopCapture() must be called even if the window is closed early.
bool StartCapture(const CaptureTarget& target, std::wstring* error);

// Stop, convert, parse, reassemble, and clean up. Safe to call even if
// StartCapture failed. Always clears any pktmon filter it installed.
CaptureResult StopCapture(const CaptureTarget& target);

// As above, but ALSO writes the converted capture to 'savePcapngAs' before the
// temp files are deleted, and reports the outcome in CaptureResult::error if
// that write failed.
//
// WHY A PARAMETER AND NOT A BYTES-FIELD ON THE RESULT. The capture is read into
// memory in one piece and may be up to 512 MB (kMaxCaptureBytes). Returning
// those bytes as a std::string on the result would double peak memory for every
// caller, including the ones that only want counters. Writing the file here -
// while the buffer is alive and before RemoveIfPresent - costs one copy and
// keeps the memory policy in exactly one place.
//
// The file written is the pktmon-converted pcapng, byte-for-byte, so it opens in
// Wireshark or tshark with no conversion by the reader.
CaptureResult StopCapture(const CaptureTarget& target,
                          const std::wstring* savePcapngAs);

// A hex dump of one reassembled direction, in the conventional layout: an
// 8-digit offset, 16 bytes as hex, then the same bytes as printable ASCII with
// '.' for everything else. Offsets are the direction's OWN stream offsets, so
// they match the TCP sequence base when one was seen.
//
// Pure: bytes in, text out, no capture machinery. Exposed so the formatting
// can be pinned by the selftest - the ASCII column in particular is the kind of
// detail that is wrong in a way nobody notices by eye.
std::string FormatStreamHex(const std::string& bytes, size_t bytesPerLine = 16);

// Which side(s) of a stream to print. The two halves are named for their
// endpoint rather than "client"/"server" for the reason given on ReasmResult:
// pktmon does not reliably report which end sent the SYN, so a client/server
// name would be an assertion the data does not support.
enum class CaptureDir : unsigned {
    kBoth = 0,
    kFirst,        // toServer  - dir1Label
    kSecond,       // toClient  - dir2Label
};

// Parse a --dir value. Accepts both / server / client / a / b / 1 / 2, in any
// case, because the point of the switch is to be typed quickly and the meaning
// is obvious from the accepted names. False on anything else - an unrecognised
// direction must be an error, never a silent both.
bool ParseCaptureDir(const std::wstring& value, CaptureDir* out);

// Remove any filter this tool installed. Called from both paths and from the
// destructor of the object that owns the flow, so an abandoned filter can
// never skew an unrelated later capture.
void ClearCaptureFilter();

}  // namespace wintcp
