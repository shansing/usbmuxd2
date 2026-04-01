//
//  WIFIConnectionSession.cpp
//  usbmuxd2
//

#include <libgeneral/macros.h>

#ifdef HAVE_LIBIMOBILEDEVICE

#include "WIFIConnectionSession.hpp"
#include "WIFIDevice.hpp"
#include "../Muxer.hpp"

#include <libimobiledevice/heartbeat.h>
#include <plist/plist.h>

#include <cstring>
#include <thread>

WIFIConnectionSession::WIFIConnectionSession(std::shared_ptr<WIFIDevice> device)
: _device(device)
, _worker()
, _stopRequested(false)
, _started(false)
, _reconnectRequested(false)
, _pendingDiscoveryVersion(0)
, _appliedDiscoveryVersion(0)
, _state(State::Idle)
, _idev(nullptr)
, _hbclient(nullptr)
{
}

WIFIConnectionSession::~WIFIConnectionSession(){
    stop(true);
    cleanupConnection();
}

void WIFIConnectionSession::cleanupConnection() noexcept{
    safeFreeCustom(_hbclient, heartbeat_client_free);
    safeFreeCustom(_idev, idevice_free);
}

std::chrono::milliseconds WIFIConnectionSession::nextBackoff(size_t &idx) const noexcept{
    static const std::chrono::milliseconds seq[] = {
        std::chrono::seconds(1),
        std::chrono::seconds(2),
        std::chrono::seconds(4),
        std::chrono::seconds(8),
        std::chrono::seconds(16),
        std::chrono::seconds(30),
    };
    auto delay = seq[idx];
    idx = (idx + 1) % (sizeof(seq)/sizeof(seq[0]));
    return delay;
}

bool WIFIConnectionSession::runAttempt(std::shared_ptr<WIFIDevice> dev) noexcept{
    heartbeat_error_t hret = HEARTBEAT_E_SUCCESS;
    auto discoveryInfo = dev->snapshotDiscoveryInfo();
    _appliedDiscoveryVersion = discoveryInfo.version;
    _reconnectRequested = false;

    cleanupConnection();

    warning("[WIFIConnectionSession] starting connection attempt serial=%s service=%s discoveryVersion=%llu",
            dev->_serial,
            discoveryInfo.serviceName.c_str(),
            (unsigned long long)discoveryInfo.version);
    try {
        assure(!idevice_new_with_options(&_idev, dev->_serial, IDEVICE_LOOKUP_NETWORK));
    } catch (tihmstar::exception &e) {
        warning("[WIFIConnectionSession] idevice lookup failed serial=%s error=%d (%s)", dev->_serial, e.code(), e.what());
        cleanupConnection();
        return false;
    }

    if ((hret = heartbeat_client_start_service(_idev, &_hbclient, "usbmuxd2")) != HEARTBEAT_E_SUCCESS) {
        warning("[WIFIConnectionSession] heartbeat service start failed serial=%s error=%d", dev->_serial, hret);
        cleanupConnection();
        return false;
    }

    warning("[WIFIConnectionSession] heartbeat session established serial=%s", dev->_serial);
    return true;
}

void WIFIConnectionSession::runloop() noexcept{
    size_t backoffIdx = 0;

    try {
    while (!_stopRequested) {
        auto dev = _device.lock();
        if (!dev) {
            break;
        }

        _state = State::Connecting;
        bool connected = runAttempt(dev);
        if (connected) {
            _state = State::Connected;
            backoffIdx = 0;
            bool sleepyTimeReceived = false;
            plist_t hbrsp = nullptr;
            plist_t hbeat = nullptr;
            assure(hbrsp = plist_new_dict());
            plist_dict_set_item(hbrsp, "Command", plist_new_string("Polo"));
            while (!_stopRequested) {
                if (_pendingDiscoveryVersion.load() != _appliedDiscoveryVersion || _reconnectRequested.load()) {
                    warning("[WIFIConnectionSession] discovery update requires reconnect serial=%s pendingVersion=%llu appliedVersion=%llu",
                            dev->_serial,
                            (unsigned long long)_pendingDiscoveryVersion.load(),
                            (unsigned long long)_appliedDiscoveryVersion);
                    break;
                }
                heartbeat_error_t hret = heartbeat_receive_with_timeout(_hbclient, &hbeat, 15000);
                if (hret != HEARTBEAT_E_SUCCESS) {
                    if (sleepyTimeReceived) {
                        warning("[WIFIConnectionSession] device sleeping, will reconnect later serial=%s", dev->_serial);
                    } else {
                        warning("[WIFIConnectionSession] heartbeat receive failed serial=%s error=%d", dev->_serial, hret);
                    }
                    break;
                }

                // Detect if the device is entering sleep mode
                plist_t cmdNode = hbeat ? plist_dict_get_item(hbeat, "Command") : nullptr;
                if (cmdNode) {
                    const char *cmdVal = plist_get_string_ptr(cmdNode, nullptr);
                    if (cmdVal && strcmp(cmdVal, "SleepyTime") == 0) {
                        sleepyTimeReceived = true;
                    } else {
                        sleepyTimeReceived = false;
                    }
                }

                hret = heartbeat_send(_hbclient, hbrsp);
                safeFreeCustom(hbeat, plist_free);
                if (hret != HEARTBEAT_E_SUCCESS) {
                    warning("[WIFIConnectionSession] heartbeat send failed serial=%s error=%d", dev->_serial, hret);
                    break;
                }
            }
            safeFreeCustom(hbeat, plist_free);
            safeFreeCustom(hbrsp, plist_free);

            if (_stopRequested) {
                break;
            }

            if (_reconnectRequested.exchange(false) || _pendingDiscoveryVersion.load() != _appliedDiscoveryVersion) {
                warning("[WIFIConnectionSession] reconnecting immediately after discovery change serial=%s pendingVersion=%llu appliedVersion=%llu",
                        dev->_serial,
                        (unsigned long long)_pendingDiscoveryVersion.load(),
                        (unsigned long long)_appliedDiscoveryVersion);
                continue;
            }

            // Device entered sleep normally: skip kill/backoff, wait then reconnect
            if (sleepyTimeReceived) {
                _state = State::Retrying;
                static constexpr auto sleepReconnectDelay = std::chrono::seconds(60);
                warning("[WIFIConnectionSession] device entered sleep, waiting %llds before reconnect serial=%s",
                        (long long)std::chrono::duration_cast<std::chrono::seconds>(sleepReconnectDelay).count(), dev->_serial);
                std::unique_lock<std::mutex> ul(_wakeLck);
                _wakeCv.wait_for(ul, sleepReconnectDelay, [this]{
                    return _stopRequested.load() || _reconnectRequested.load() || _pendingDiscoveryVersion.load() != _appliedDiscoveryVersion;
                });
                continue;
            }

            if (!dev->_mux->allowHeartlessWifi()) {
                warning("[WIFIConnectionSession] removing wifi device after heartbeat failure serial=%s because allowHeartlessWifi=NO", dev->_serial);
                dev->kill();
                break;
            }
        } else if (!dev->_mux->allowHeartlessWifi()) {
            warning("[WIFIConnectionSession] removing wifi device after startup failure serial=%s because allowHeartlessWifi=NO", dev->_serial);
            dev->kill();
            break;
        }

        if (!dev->_mux->retryWifiSession()) {
            warning("[WIFIConnectionSession] session retry disabled serial=%s", dev->_serial);
            break;
        }

        _state = State::Retrying;
        auto delay = nextBackoff(backoffIdx);
        warning("[WIFIConnectionSession] retrying wifi session serial=%s in %lld ms", dev->_serial, (long long)delay.count());
        std::unique_lock<std::mutex> ul(_wakeLck);
        _wakeCv.wait_for(ul, delay, [this]{
            return _stopRequested.load() || _reconnectRequested.load() || _pendingDiscoveryVersion.load() != _appliedDiscoveryVersion;
        });
    }
    } catch (tihmstar::exception &e) {
        error("[WIFIConnectionSession] session thread aborted with error=%d (%s)", e.code(), e.what());
    }

    cleanupConnection();
    _state = State::Stopped;
    _started = false;
}

void WIFIConnectionSession::start(){
    if (_worker.joinable()) {
        if (_started.load()) {
            return;
        }
        _worker.join();
    }
    bool expected = false;
    if (!_started.compare_exchange_strong(expected, true)) {
        return;
    }
    if (auto dev = _device.lock()) {
        _pendingDiscoveryVersion = dev->snapshotDiscoveryInfo().version;
        _appliedDiscoveryVersion = _pendingDiscoveryVersion.load();
    }
    _stopRequested = false;
    _reconnectRequested = false;
    _worker = std::thread([this]{ runloop(); });
}

void WIFIConnectionSession::stop(bool joinThread) noexcept{
    _stopRequested = true;
    _reconnectRequested = false;
    _state = State::Stopping;
    _wakeCv.notify_all();
    if (joinThread && _worker.joinable()) {
        _worker.join();
        _started = false;
    }
}

void WIFIConnectionSession::notifyDiscoveryUpdate(uint64_t version, bool reconnectNeeded) noexcept{
    _pendingDiscoveryVersion = version;
    if (reconnectNeeded) {
        _reconnectRequested = true;
    }
    _wakeCv.notify_all();
}

WIFIConnectionSession::State WIFIConnectionSession::state() const noexcept{
    return _state.load();
}

bool WIFIConnectionSession::hasHeartbeat() const noexcept{
    return _hbclient != nullptr;
}

heartbeat_client_t WIFIConnectionSession::heartbeatClient() const noexcept{
    return _hbclient;
}

#endif //HAVE_LIBIMOBILEDEVICE
