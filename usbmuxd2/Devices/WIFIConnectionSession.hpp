//
//  WIFIConnectionSession.hpp
//  usbmuxd2
//

#ifndef WIFIConnectionSession_hpp
#define WIFIConnectionSession_hpp

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/heartbeat.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class WIFIDevice;

class WIFIConnectionSession {
public:
    enum class State {
        Idle,
        Connecting,
        Connected,
        Retrying,
        Stopping,
        Stopped,
    };

private:
    std::weak_ptr<WIFIDevice> _device;
    std::thread _worker;
    std::mutex _stateLck;
    std::mutex _wakeLck;
    std::condition_variable _wakeCv;
    std::atomic<bool> _stopRequested;
    std::atomic<bool> _started;
    std::atomic<bool> _reconnectRequested;
    std::atomic<uint64_t> _pendingDiscoveryVersion;
    uint64_t _appliedDiscoveryVersion;
    std::atomic<State> _state;
    idevice_t _idev;
    heartbeat_client_t _hbclient;

    bool runAttempt(std::shared_ptr<WIFIDevice> dev) noexcept;
    void runloop() noexcept;
    std::chrono::milliseconds nextBackoff(size_t &idx) const noexcept;
    void cleanupConnection() noexcept;

public:
    explicit WIFIConnectionSession(std::shared_ptr<WIFIDevice> device);
    ~WIFIConnectionSession();

    void start();
    void stop(bool joinThread = true) noexcept;
    void requestReconnectNow() noexcept;
    void notifyDiscoveryUpdate(uint64_t version, bool reconnectNeeded) noexcept;
    State state() const noexcept;
    bool hasHeartbeat() const noexcept;
    heartbeat_client_t heartbeatClient() const noexcept;
};

#endif /* WIFIConnectionSession_hpp */
