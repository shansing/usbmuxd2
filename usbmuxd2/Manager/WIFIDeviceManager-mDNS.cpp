//
//  WIFIDeviceManager-mDNS.cpp
//  usbmuxd2
//
//  Created by tihmstar on 26.09.19.
//  Copyright © 2019 tihmstar. All rights reserved.
//


#include <libgeneral/macros.h>

#ifdef HAVE_WIFI_MDNS
#include "WIFIDeviceManager-mDNS.hpp"
#include "../Devices/WIFIDevice.hpp"
#include "../sysconf/sysconf.hpp"
#include "../Devices/WIFIDevice.hpp"
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>

#pragma mark definitions

#define kDNSServiceInterfaceIndexAny 0
#define kDNSServiceInterfaceIndexLocalOnly ((uint32_t)-1)
#define kDNSServiceInterfaceIndexUnicast   ((uint32_t)-2)
#define kDNSServiceInterfaceIndexP2P       ((uint32_t)-3)
#define kDNSServiceInterfaceIndexBLE       ((uint32_t)-4)


#define kDNSServiceFlagsMoreComing 0x1
#define kDNSServiceFlagsAdd     0x2
#define LONG_TIME 100000000

#define kDNSServiceProtocol_IPv4 0x01
#define kDNSServiceProtocol_IPv6 0x02


#pragma mark callbacks
void getaddr_reply(DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t interfaceIndex, DNSServiceErrorType errorCode, const char *hostname, const struct sockaddr *address, uint32_t ttl, void *context) noexcept{
    int err = 0;
    WIFIDeviceManager *devmgr = (WIFIDeviceManager *)context;

    if (errorCode) {
        error("getaddr_reply failed with DNSService error=%d", errorCode);
        devmgr->request_restart();
        goto error;
    }

    if (!address) {
        error("getaddr_reply received null address");
        devmgr->request_restart();
        goto error;
    }

    std::vector<std::string> &addrs = devmgr->_clientAddrs[sdRef];

    std::string ipaddr;
    ipaddr.resize(INET6_ADDRSTRLEN+1);
    if (address->sa_family == AF_INET6) {
        ipaddr = inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)address)->sin6_addr), ipaddr.data(), (socklen_t)ipaddr.size());
    }else{
        ipaddr = inet_ntop(AF_INET, &(((struct sockaddr_in *)address)->sin_addr), ipaddr.data(), (socklen_t)ipaddr.size());
    }
    addrs.push_back(ipaddr);


    if (!(flags & kDNSServiceFlagsMoreComing)) {
        bool notifyadd = true;
        std::string serviceName = addrs.front();
        addrs.erase(addrs.begin());

        std::string macAddr{serviceName.substr(0,serviceName.find("@"))};
        std::string uuid;
        if (strstr(serviceName.c_str(), "_remotepairing-manual-pairing._tcp")) {
            uuid = "WIFIPAIR-"+serviceName.substr(0,serviceName.find("."));
            macAddr = {};
            if (devmgr->_mux->have_wifi_device_with_ip(addrs)) goto error;
            notifyadd = false;
        }else{
            try{
                uuid = sysconf_udid_for_macaddr(macAddr);
            }catch (tihmstar::exception &e){
                creterror("failed to find uuid for mac=%s with error=%d (%s)",macAddr.c_str(),e.code(),e.what());
            }
            if (auto existing = devmgr->_mux->get_wifi_device_with_serial(uuid)) {
                warning("Updating existing wifi device serial=%s service='%s' ip='%s'", uuid.c_str(), serviceName.c_str(), addrs.size() ? addrs.front().c_str() : "<none>");
                existing->updateDiscoveryInfo(addrs, serviceName, interfaceIndex);
                existing->setRediscoverOnDestruct(true);
                existing->ensureSession();
                goto error;
            }
            devmgr->_mux->delete_wifi_pairing_device_with_ip(addrs);
            notifyadd = true;
        }

        {
            std::shared_ptr<WIFIDevice> dev = nullptr;
            try{
                dev = std::make_shared<WIFIDevice>(devmgr->_mux, devmgr, uuid, addrs, serviceName, interfaceIndex);
                devmgr->device_add(dev, notifyadd); dev = NULL;
            } catch (tihmstar::exception &e){
                creterror("failed to construct device with error=%d (%s)",e.code(),e.what());
            }
        }
    }

error:
    if (!(flags & kDNSServiceFlagsMoreComing)) {
        devmgr->_clientAddrs.erase(sdRef);
        DNSServiceRef sdResolv = devmgr->_linkedClients[sdRef];
        devmgr->_linkedClients.erase(sdRef);
        devmgr->_removeClients.push_back(sdRef); //idk why, but order is important!
        devmgr->_removeClients.push_back(sdResolv);
    }
    if (err) {
        error("getaddr_reply failed with error=%d",err);
    }
}

void resolve_reply(DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t interfaceIndex, DNSServiceErrorType errorCode, const char *fullname, const char *hosttarget, uint16_t port, uint16_t txtLen, const unsigned char *txtRecord, void *context) noexcept{
    int err = 0;
    WIFIDeviceManager *devmgr = (WIFIDeviceManager *)context;
    DNSServiceErrorType res = 0;
    DNSServiceRef resolvClient = NULL;
    int resolvfd = -1;

    if (errorCode) {
        error("resolve_reply failed with DNSService error=%d", errorCode);
        devmgr->request_restart();
        return;
    }

    cassure(!(res = DNSServiceGetAddrInfo(&resolvClient, 0, kDNSServiceInterfaceIndexAny, kDNSServiceProtocol_IPv4 | kDNSServiceProtocol_IPv6, hosttarget, getaddr_reply, context)));

    cassure((resolvfd = DNSServiceRefSockFD(resolvClient))>0);

    devmgr->_clientAddrs[resolvClient] = {fullname};

    devmgr->_resolveClients.push_back(resolvClient);
    devmgr->_linkedClients[resolvClient] = sdRef;
    devmgr->rebuild_pollfds();

error:
    if (err) {
        error("resolve_reply failed with error=%d",err);
    }
}

void browse_reply(DNSServiceRef sdref, const DNSServiceFlags flags, uint32_t ifIndex, DNSServiceErrorType errorCode, const char *replyName, const char *replyType, const char *replyDomain, void *context) noexcept{
    int err = 0;
    DNSServiceErrorType res = 0;
    WIFIDeviceManager *devmgr = (WIFIDeviceManager *)context;
    DNSServiceRef resolvClient = NULL;
    int resolvfd = -1;

    if (errorCode) {
        error("browse_reply failed with DNSService error=%d", errorCode);
        devmgr->request_restart();
        return;
    }

    if (!(flags & kDNSServiceFlagsAdd)) {
        warning("browse_reply remove/ignore flags=%u ifIndex=%u type=%s name=%s", flags, ifIndex, replyType, replyName);
        if (replyType && strstr(replyType, "_remotepairing-manual-pairing._tcp")) {
            std::string serial = std::string("WIFIPAIR-") + replyName;
            warning("browse_reply pairing remove cleanup serial=%s", serial.c_str());
            devmgr->_mux->delete_wifi_device_with_serial(serial);
        }
        return;
    }

    const char *op = (flags & kDNSServiceFlagsAdd) ? "Add" : "Rmv";
    debug("%s %8X %3d %-20s %-20s %s",
           op, flags, ifIndex, replyDomain, replyType, replyName);

    cassure(!(res = DNSServiceResolve(&resolvClient, 0, kDNSServiceInterfaceIndexAny, replyName, replyType, replyDomain, resolve_reply, context)));

    cassure((resolvfd = DNSServiceRefSockFD(resolvClient))>0);

error:
    if (resolvClient){
        devmgr->_resolveClients.push_back(resolvClient);
        devmgr->rebuild_pollfds();
    }
    if (err) {
        error("browse_reply failed with error=%d",err);
    }
}


#pragma mark WIFIDevice

WIFIDeviceManager::WIFIDeviceManager(Muxer *mux)
: DeviceManager(mux), _client(NULL), _clientPairing(NULL), _dns_sd_fd(-1), _dns_sd_pairing_fd(-1), _wakePipe{-1, -1}
, _shouldRestart(false), _isStopping(false)
{
    debug("WIFIDeviceManager mDNS-client");
    init_mdns();

    _devReaperThread = std::thread([this]{
        reaper_runloop();
    });
}

WIFIDeviceManager::~WIFIDeviceManager(){
    stopLoop();
    if (_children.size()) {
        debug("waiting for wifi children to die...");
        std::unique_lock<std::mutex> ul(_childrenLck);
        while (size_t s = _children.size()) {
            for (auto c : _children) c->kill();
            uint64_t wevent = _childrenEvent.getNextEvent();
            ul.unlock();
            debug("Need to kill %zu more wifi children",s);
            _childrenEvent.waitForEvent(wevent);
            ul.lock();
        }
    }
    _reapDevices.kill();
    _devReaperThread.join();
    cleanup_mdns();
}

void WIFIDeviceManager::device_add(std::shared_ptr<WIFIDevice> dev, bool notify){
    dev->_selfref = dev;
    {
        std::unique_lock<std::mutex> ul(_childrenLck);
        _children.insert(dev.get());
    }
    _mux->add_device(dev, notify);
}

bool WIFIDeviceManager::loopEvent(){
    while (true) {
        if (!_client || !_clientPairing || _wakePipe[0] < 0 || _wakePipe[1] < 0) {
            try {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                init_mdns();
            } catch (tihmstar::exception &e) {
                if (_isStopping) {
                    return true;
                }
                error("Failed to initialize mDNS discovery with error=%d (%s)", e.code(), e.what());
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
        }

        int res = poll(_pfds.data(), (int)_pfds.size(), -1);
        if (res > 0){
            bool restartRequested = false;
            cleanup([&]{
                for (auto &rc : _removeClients) {
                    const auto target = std::remove(_resolveClients.begin(), _resolveClients.end(), rc);
                    if (target != _resolveClients.end()){
                        DNSServiceRef tgt = *target;
                        _resolveClients.erase(target, _resolveClients.end());
                        DNSServiceRefDeallocate(tgt);
                    }
                }
                _removeClients.clear();
                rebuild_pollfds();
            });
            auto cpy_pfds = _pfds;
            for (auto pfd : cpy_pfds) {
                if (pfd.fd == _wakePipe[0] && (pfd.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
                    char buf[32];
                    while (read(_wakePipe[0], buf, sizeof(buf)) > 0) {}
                    restartRequested = _shouldRestart;
                    continue;
                }

                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    warning("WIFIDeviceManager mDNS fd=%d signaled poll error events=0x%x", pfd.fd, pfd.revents);
                    request_restart();
                    restartRequested = true;
                    continue;
                }

                if (pfd.revents & POLLIN) {
                    DNSServiceErrorType err = 0;
                    if (_client && pfd.fd == DNSServiceRefSockFD(_client)) {
                        err = DNSServiceProcessResult(_client);
                    }else if (_clientPairing && pfd.fd == DNSServiceRefSockFD(_clientPairing)) {
                        err = DNSServiceProcessResult(_clientPairing);
                    }else{
                        for (auto rc : _resolveClients) {
                            int rcfd = DNSServiceRefSockFD(rc);
                            if (rcfd == pfd.fd) {
                                err = DNSServiceProcessResult(rc);
                                break;
                            }
                        }
                    }

                    if (err) {
                        error("DNSServiceProcessResult failed with error=%d", err);
                        request_restart();
                        restartRequested = true;
                    }
                }
            }

            if (_isStopping) {
                debug("WIFIDeviceManager mDNS loop stopping");
                return true;
            }

            if (restartRequested || _shouldRestart) {
                warning("WIFIDeviceManager mDNS discovery restarting");
                cleanup_mdns();
                continue;
            }
        }else if (res != 0){
            warning("poll() returned %d errno %d %s; restarting mDNS discovery", res, errno, strerror(errno));
            request_restart();
            cleanup_mdns();
            continue;
        }
    }
}

void WIFIDeviceManager::stopAction() noexcept{
    _isStopping = true;
    if (_wakePipe[1] >= 0) {
        char wake = 'q';
        (void)write(_wakePipe[1], &wake, 1);
    }
}

void WIFIDeviceManager::reaper_runloop(){
    while (true) {
        std::shared_ptr<WIFIDevice>dev;
        try {
            dev = _reapDevices.wait();
        } catch (...) {
            break;
        }
        //make device go out of scope so it can die in piece
        dev->deconstruct();
    }
}

void WIFIDeviceManager::init_mdns(){
    int err = 0;

    if (_client || _clientPairing || _wakePipe[0] >= 0 || _wakePipe[1] >= 0) {
        cleanup_mdns();
    }

    assure(!(err = DNSServiceBrowse(&_client, 0, kDNSServiceInterfaceIndexAny, "_apple-mobdev2._tcp", "", browse_reply, this)));
    assure(!(err = DNSServiceBrowse(&_clientPairing, 0, kDNSServiceInterfaceIndexAny, "_remotepairing-manual-pairing._tcp", "", browse_reply, this)));

    assure((_dns_sd_fd = DNSServiceRefSockFD(_client))>0);
    assure((_dns_sd_pairing_fd = DNSServiceRefSockFD(_clientPairing))>0);

    assure(!pipe(_wakePipe));
    _shouldRestart = false;
    rebuild_pollfds();
    debug("WIFIDeviceManager created mDNS browsers");
}

void WIFIDeviceManager::cleanup_mdns() noexcept{
    _pfds.clear();
    _linkedClients.clear();
    _clientAddrs.clear();
    _removeClients.clear();
    {
        for (auto rc : _resolveClients)
            safeFreeCustom(rc, DNSServiceRefDeallocate);
        _resolveClients.clear();
    }
    safeFreeCustom(_client, DNSServiceRefDeallocate);
    safeFreeCustom(_clientPairing, DNSServiceRefDeallocate);
    _dns_sd_fd = -1;
    _dns_sd_pairing_fd = -1;
    safeClose(_wakePipe[0]);
    safeClose(_wakePipe[1]);
}

void WIFIDeviceManager::rebuild_pollfds() noexcept{
    _pfds.clear();
    if (_dns_sd_fd >= 0) {
        _pfds.push_back({.fd = _dns_sd_fd, .events = POLLIN});
    }
    if (_dns_sd_pairing_fd >= 0) {
        _pfds.push_back({.fd = _dns_sd_pairing_fd, .events = POLLIN});
    }
    if (_wakePipe[0] >= 0) {
        _pfds.push_back({.fd = _wakePipe[0], .events = POLLIN});
    }
    for (auto c : _resolveClients) {
        int cfd = DNSServiceRefSockFD(c);
        if (cfd != -1){
            _pfds.push_back({.fd = cfd, .events = POLLIN});
        }else{
            _removeClients.push_back(c);
        }
    }
}

void WIFIDeviceManager::request_restart() noexcept{
    _shouldRestart = true;
    if (_wakePipe[1] >= 0) {
        char wake = 'r';
        (void)write(_wakePipe[1], &wake, 1);
    }
}

void WIFIDeviceManager::request_manual_refresh() noexcept{
    if (_isStopping) {
        debug("Ignoring mDNS manual refresh during shutdown");
        return;
    }
    warning("WIFIDeviceManager manual mDNS refresh requested");
    request_restart();
}

void WIFIDeviceManager::request_rediscovery_after_pairing(const char *udid) noexcept{
    if (_isStopping) {
        debug("Ignoring mDNS rediscovery-after-pairing request during shutdown udid=%s", udid ? udid : "<null>");
        return;
    }
    warning("WIFIDeviceManager requesting mDNS rediscovery after new pairing udid=%s", udid ? udid : "<null>");
    request_restart();
}

void WIFIDeviceManager::request_device_rediscovery(const char *serial, const char *serviceName) noexcept{
    if (_isStopping) {
        debug("Ignoring mDNS rediscovery request during shutdown serial=%s service=%s", serial ? serial : "<null>", serviceName ? serviceName : "<null>");
        return;
    }
    warning("WIFIDeviceManager requesting mDNS rediscovery after wifi device loss serial=%s service=%s", serial ? serial : "<null>", serviceName ? serviceName : "<null>");
    request_restart();
}

#endif //HAVE_WIFI_MDNS
