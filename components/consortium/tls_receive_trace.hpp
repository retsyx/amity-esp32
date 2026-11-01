// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace amity::consortium::platform {
// Numeric receive diagnostics only; no payload or TLS secrets. Expired
// application frames are rejected and drained on the same TLS stream. Keep
// that observation until a valid frame or a true fatal failure supersedes it.
struct TlsReceiveDiagnostic {
    enum class Cause {none,progress_timeout,frame_timeout,clock_reversed,tls_eof,tls_error,invalid_length,parser_rejected,wrapper_timeout,operation_timeout};
    Cause cause=Cause::none;
    int code=0;
    uint32_t bytes=0,expected=0;
    uint64_t progress_ms=0,total_ms=0,poll_gap_ms=0,read_ms=0;
    bool expired()const{return cause==Cause::progress_timeout||cause==Cause::frame_timeout||cause==Cause::clock_reversed||cause==Cause::operation_timeout;}
    bool fatal()const{return cause==Cause::tls_eof||cause==Cause::tls_error||cause==Cause::invalid_length||cause==Cause::parser_rejected;}
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
        case Cause::operation_timeout:return "frame_receive_timeout";
        }
        return "unknown";
    }
};
}
