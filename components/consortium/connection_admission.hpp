// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/limits.hpp"
#include <mutex>
#include <utility>

namespace amity::consortium::platform {
// TLS buffers remain charged until their owner is destroyed, including after
// runtime reaps a cancelled link whose worker is still unwinding.
class ConnectionAdmission {
public:
    class Reservation {
    public:
        Reservation() = default;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        Reservation(Reservation&& other) noexcept
            : owner_(std::exchange(other.owner_,nullptr)), pending_(other.pending_) {}
        Reservation& operator=(Reservation&& other) noexcept {
            if(this!=&other){release();owner_=std::exchange(other.owner_,nullptr);pending_=other.pending_;}
            return *this;
        }
        ~Reservation(){release();}
        explicit operator bool()const{return owner_!=nullptr;}
        void authenticated(){
            if(!owner_||!pending_)return;
            std::lock_guard<std::mutex> guard(owner_->mutex_);
            --owner_->pending_;pending_=false;
        }
    private:
        friend class ConnectionAdmission;
        explicit Reservation(ConnectionAdmission& owner):owner_(&owner),pending_(true){}
        void release(){
            if(!owner_)return;
            std::lock_guard<std::mutex> guard(owner_->mutex_);
            if(pending_)--owner_->pending_;
            --owner_->total_;owner_=nullptr;
        }
        ConnectionAdmission* owner_=nullptr;
        bool pending_=false;
    };
    Reservation reserve(){
        std::lock_guard<std::mutex> guard(mutex_);
        if(pending_>=maximum_pending_connections||total_>=maximum_connection_slots)return {};
        ++pending_;++total_;return Reservation(*this);
    }
private:
    std::mutex mutex_;
    size_t pending_=0,total_=0;
};
inline ConnectionAdmission& connection_admission(){static ConnectionAdmission admission;return admission;}
}
