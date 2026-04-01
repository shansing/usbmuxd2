//
//  WIFIDevice.hpp
//  usbmuxd2
//
//  Created by tihmstar on 30.05.21.
//

#ifndef WIFIDevice_hpp
#define WIFIDevice_hpp

#include "Device.hpp"
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/heartbeat.h>

#include <iostream>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

class WIFIDeviceManager;
class WIFIConnectionSession;
class WIFIDevice : public Device {
    struct DiscoveryInfo {
        std::vector<std::string> ipaddr;
        std::string serviceName;
        uint32_t interfaceIndex;
        uint64_t version;
    };

    WIFIDeviceManager *_parent;
    std::weak_ptr<WIFIDevice> _selfref;
    std::vector<std::string> _ipaddr;
    std::string _serviceName;
    uint32_t _interfaceIndex;
    std::atomic<uint64_t> _discoveryVersion;
    std::shared_ptr<WIFIConnectionSession> _session;
    mutable std::mutex _sessionLck;
    bool _rediscoverOnDestruct;

    bool isPairingDevice() const noexcept;
    DiscoveryInfo snapshotDiscoveryInfo() const;

public:
    WIFIDevice(Muxer *mux, WIFIDeviceManager *parent, std::string uuid, std::vector<std::string> ipaddr, std::string serviceName, uint32_t interfaceIndex = 0);
    WIFIDevice(const WIFIDevice &) =delete; //delete copy constructor
    WIFIDevice(WIFIDevice &&o) = delete; //move constructor
    virtual ~WIFIDevice() override;

    virtual void kill() noexcept override;
    void deconstruct() noexcept;
    void startLoop();
    void ensureSession();
    void stopSession(bool joinThread = true) noexcept;
    void updateDiscoveryInfo(std::vector<std::string> ipaddr, std::string serviceName, uint32_t interfaceIndex);
    void setRediscoverOnDestruct(bool enabled) noexcept;
    virtual void start_connect(uint16_t dport, std::shared_ptr<Client> cli) override;

    friend class Muxer;
    friend class WIFIDeviceManager;
    friend class WIFIConnectionSession;
};

#endif /* WIFIDevice_hpp */
