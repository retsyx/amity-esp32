/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The CHIP fakes: the CHIP task and its stack lock, System Layer timers, and
// strict seams for the SDK calls that answer questions.
#include "amity_matter_fakes.hpp"
#include "app/server/Server.h"
#include "esp_matter.h"
#include "esp_memory_utils.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "platform/CHIPDeviceLayer.h"
#include "platform/ESP32/NetworkCommissioningDriver.h"
#include "seams.hpp"
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace {

using chip::DeviceLayer::AsyncWorkFunct;

[[noreturn]] void fail(const std::string &message) { amity_test::fail(__FILE__, __LINE__, message.c_str()); }

std::mutex state;
amity_fake::ChipWork work_mode = amity_fake::ChipWork::queued;
unsigned scheduling_failures = 0;
CHIP_ERROR scheduling_error = CHIP_ERROR_NO_MEMORY;
unsigned accepted = 0;
std::deque<std::pair<AsyncWorkFunct, intptr_t>> work;
std::deque<amity_fake::ChipTimer> pending_timers;
unsigned timer_cancels = 0;
std::function<void()> lock_hook;
unsigned scoped_locks = 0;

// The stack lock and the thread holding it.
std::mutex stack;
std::atomic<std::thread::id> owner{};
thread_local bool chip_task = false;

bool held() { return owner.load() == std::this_thread::get_id(); }

void take_stack() {
    stack.lock();
    owner.store(std::this_thread::get_id());
}

void release_stack() {
    owner.store(std::thread::id());
    stack.unlock();
}

}  // namespace

namespace amity_fake {

void chip_work(ChipWork mode) {
    std::lock_guard<std::mutex> lock(state);
    work_mode = mode;
}
void fail_chip_scheduling(unsigned count, CHIP_ERROR error) {
    std::lock_guard<std::mutex> lock(state);
    scheduling_failures = count;
    scheduling_error = error;
}
unsigned scheduled_chip_work() {
    std::lock_guard<std::mutex> lock(state);
    return accepted;
}
std::size_t queued_chip_work() {
    std::lock_guard<std::mutex> lock(state);
    return work.size();
}
bool run_chip_work() {
    std::pair<AsyncWorkFunct, intptr_t> item;
    {
        std::lock_guard<std::mutex> lock(state);
        if (work.empty()) return false;
        item = work.front();
        work.pop_front();
    }
    run_as_chip_task([&] { item.first(item.second); });
    return true;
}
std::size_t run_all_chip_work() {
    std::size_t ran = 0;
    while (run_chip_work()) ++ran;
    return ran;
}

void run_as_chip_task(const std::function<void()> &fn) {
    if (held()) fail("the CHIP task ran on a thread that holds the CHIP stack lock");
    take_stack();
    chip_task = true;
    fn();
    chip_task = false;
    release_stack();
}
bool on_chip_task() { return chip_task; }
bool chip_stack_locked() { return held(); }
void on_chip_stack_lock(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(state);
    lock_hook = std::move(hook);
}
unsigned chip_stack_locks() {
    std::lock_guard<std::mutex> lock(state);
    return scoped_locks;
}

std::vector<ChipTimer> chip_timers() {
    std::lock_guard<std::mutex> lock(state);
    return {pending_timers.begin(), pending_timers.end()};
}
unsigned chip_timer_cancels() {
    std::lock_guard<std::mutex> lock(state);
    return timer_cancels;
}
bool fire_chip_timer() {
    ChipTimer timer;
    {
        std::lock_guard<std::mutex> lock(state);
        if (pending_timers.empty()) return false;
        timer = pending_timers.front();
        pending_timers.pop_front();
    }
    run_as_chip_task([&] { timer.callback(&chip::DeviceLayer::SystemLayer(), timer.context); });
    return true;
}

}  // namespace amity_fake

namespace esp_matter {
namespace lock {

ScopedChipStackLock::ScopedChipStackLock(uint32_t) : locked(false) {
    if (held()) return;
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lock(state);
        hook = lock_hook;
        ++scoped_locks;
    }
    if (hook) hook();
    take_stack();
    locked = true;
}

ScopedChipStackLock::~ScopedChipStackLock() {
    if (locked) release_stack();
}

}  // namespace lock
}  // namespace esp_matter

namespace chip {

CriticalFailure::CriticalFailure(CHIP_ERROR error) {
    if (error != CHIP_NO_ERROR) fail("a critical CHIP failure, which aborts the target");
}

namespace System {

CriticalFailure Layer::StartTimer(Clock::Timeout aDelay, TimerCompleteCallback aComplete, void *aAppState) {
    if (!held()) fail("System Layer StartTimer without the CHIP stack lock");
    std::lock_guard<std::mutex> lock(state);
    for (auto timer = pending_timers.begin(); timer != pending_timers.end(); ++timer) {
        if (timer->callback == aComplete && timer->context == aAppState) {
            pending_timers.erase(timer);
            break;
        }
    }
    pending_timers.push_back({aDelay, aComplete, aAppState});
    return CHIP_NO_ERROR;
}

void Layer::CancelTimer(TimerCompleteCallback aOnComplete, void *aAppState) {
    if (!held()) fail("System Layer CancelTimer without the CHIP stack lock");
    std::lock_guard<std::mutex> lock(state);
    ++timer_cancels;
    for (auto timer = pending_timers.begin(); timer != pending_timers.end(); ++timer) {
        if (timer->callback == aOnComplete && timer->context == aAppState) {
            pending_timers.erase(timer);
            return;
        }
    }
}

}  // namespace System

namespace DeviceLayer {

CHIP_ERROR PlatformManager::ScheduleWork(AsyncWorkFunct workFunct, intptr_t arg) {
    bool immediate;
    {
        std::lock_guard<std::mutex> lock(state);
        if (scheduling_failures) {
            --scheduling_failures;
            return scheduling_error;
        }
        ++accepted;
        immediate = work_mode == amity_fake::ChipWork::immediate;
        if (!immediate) work.emplace_back(workFunct, arg);
    }
    if (immediate) amity_fake::run_as_chip_task([&] { workFunct(arg); });
    return CHIP_NO_ERROR;
}

bool PlatformManager::IsChipStackLockedByCurrentThread() const { return held(); }

PlatformManager &PlatformMgr() {
    static PlatformManager instance;
    return instance;
}

CHIP_ERROR ConnectivityManager::SetBLEAdvertisingEnabled(bool val) {
    return AMITY_SEAM(chip::DeviceLayer::ConnectivityManager::SetBLEAdvertisingEnabled, *this, val);
}

ConnectivityManager &ConnectivityMgr() {
    static ConnectivityManager instance;
    return instance;
}

chip::System::Layer &SystemLayer() {
    static chip::System::Layer instance;
    return instance;
}

namespace NetworkCommissioning {

NetworkIterator *ESPWiFiDriver::GetNetworks() {
    return AMITY_SEAM(chip::DeviceLayer::NetworkCommissioning::ESPWiFiDriver::GetNetworks, *this);
}
CHIP_ERROR ESPWiFiDriver::CommitConfiguration() {
    return AMITY_SEAM(chip::DeviceLayer::NetworkCommissioning::ESPWiFiDriver::CommitConfiguration, *this);
}
CHIP_ERROR ESPWiFiDriver::RevertConfiguration() {
    return AMITY_SEAM(chip::DeviceLayer::NetworkCommissioning::ESPWiFiDriver::RevertConfiguration, *this);
}
Status ESPWiFiDriver::RemoveNetwork(ByteSpan networkId, MutableCharSpan &outDebugText, uint8_t &outNetworkIndex) {
    return AMITY_SEAM(chip::DeviceLayer::NetworkCommissioning::ESPWiFiDriver::RemoveNetwork, *this, networkId,
                      outDebugText, outNetworkIndex);
}
void ESPWiFiDriver::ConnectNetwork(ByteSpan networkId, ConnectCallback *callback) {
    AMITY_SEAM(chip::DeviceLayer::NetworkCommissioning::ESPWiFiDriver::ConnectNetwork, *this, networkId, callback);
}
Status ESPWiFiDriver::AddOrUpdateNetwork(ByteSpan ssid, ByteSpan credentials, MutableCharSpan &outDebugText,
                                         uint8_t &outNetworkIndex) {
    return AMITY_SEAM(chip::DeviceLayer::NetworkCommissioning::ESPWiFiDriver::AddOrUpdateNetwork, *this, ssid,
                      credentials, outDebugText, outNetworkIndex);
}

}  // namespace NetworkCommissioning
}  // namespace DeviceLayer

CHIP_ERROR FabricTable::Delete(FabricIndex fabricIndex) {
    return AMITY_SEAM(chip::FabricTable::Delete, *this, fabricIndex);
}
const FabricInfo *FabricTable::FindFabricWithIndex(FabricIndex fabricIndex) const {
    return AMITY_SEAM(chip::FabricTable::FindFabricWithIndex, *this, fabricIndex);
}
CHIP_ERROR FabricTable::AddFabricDelegate(Delegate *delegate) {
    return AMITY_SEAM(chip::FabricTable::AddFabricDelegate, *this, delegate);
}
void FabricTable::RemoveFabricDelegate(Delegate *delegate) {
    AMITY_SEAM(chip::FabricTable::RemoveFabricDelegate, *this, delegate);
}
const FabricInfo *FabricTable::begin() const { return AMITY_SEAM(chip::FabricTable::begin, *this); }
const FabricInfo *FabricTable::end() const { return AMITY_SEAM(chip::FabricTable::end, *this); }
FabricIndex FabricTable::GetPendingNewFabricIndex() const {
    return AMITY_SEAM(chip::FabricTable::GetPendingNewFabricIndex, *this);
}

namespace app {

bool FailSafeContext::IsFailSafeFullyDisarmed() const {
    return AMITY_SEAM(chip::app::FailSafeContext::IsFailSafeFullyDisarmed, *this);
}

}  // namespace app

Server &Server::GetInstance() {
    static Server instance;
    return instance;
}

}  // namespace chip

extern "C" {

esp_err_t esp_wifi_get_config(wifi_interface_t interface, wifi_config_t *conf) {
    return AMITY_SEAM(esp_wifi_get_config, interface, conf);
}
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *if_key) {
    return AMITY_SEAM(esp_netif_get_handle_from_ifkey, if_key);
}
bool esp_netif_is_netif_up(esp_netif_t *esp_netif) { return AMITY_SEAM(esp_netif_is_netif_up, esp_netif); }
esp_err_t esp_netif_get_ip_info(esp_netif_t *esp_netif, esp_netif_ip_info_t *ip_info) {
    return AMITY_SEAM(esp_netif_get_ip_info, esp_netif, ip_info);
}
bool esp_ptr_internal(const void *p) { return AMITY_SEAM(esp_ptr_internal, p); }

}  // extern "C"
