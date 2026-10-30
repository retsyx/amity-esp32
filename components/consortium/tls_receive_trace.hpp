// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace amity::consortium::platform {
// One terminal receive failure per link. Numeric diagnostics only; no payload
// or TLS secrets. Capture at failure, emit with the existing closure trace.
struct TlsReceiveDiagnostic {
    enum class Cause {none,progress_timeout,frame_timeout,clock_reversed,tls_eof,tls_error,invalid_length,parser_rejected,wrapper_timeout};
    Cause cause=Cause::none;
    int code=0;
    uint32_t bytes=0,expected=0;
    uint64_t progress_ms=0,total_ms=0,poll_gap_ms=0,read_ms=0;
    const char* name()const{
        switch(cause){
        case Cause::none:return "none";
        case Cause::progress_timeout:return "frame_progress_timeout";
        case Cause::frame_timeout:return "frame_total_timeout";
        case Cause::clock_reversed:return "clock_reversed";
        case Cause::tls_eof:return "tls_eof";
        case Cause::tls_error:return "tls_read_error";
        case Cause::invalid_length:return "invalid_length";
        case Cause::parser_rejected:return "parser_rejected";
        case Cause::wrapper_timeout:return "receive_timeout";
        }
        return "unknown";
    }
};
}
