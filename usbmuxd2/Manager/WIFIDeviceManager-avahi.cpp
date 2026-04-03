
//
//  WIFIDeviceManager.cpp
//  usbmuxd2
//
//  Created by tihmstar on 27.05.19.
//  Copyright © 2019 tihmstar. All rights reserved.
//

#include <libgeneral/macros.h>

#ifdef HAVE_WIFI_AVAHI
#include "WIFIDeviceManager-avahi.hpp"

#include <sysconf/sysconf.hpp>

#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/address.h>

#include <chrono>
#include <string.h>

#pragma mark avahi_callback definitions
void avahi_client_callback(AvahiClient *c, AvahiClientState state, void* userdata) noexcept;
void avahi_browse_callback(AvahiServiceBrowser *b, AvahiIfIndex interface, AvahiProtocol protocol, AvahiBrowserEvent event,
       const char *name, const char *type, const char *domain, AvahiLookupResultFlags flags, void* userdata) noexcept;
void avahi_resolve_callback(AvahiServiceResolver *r, AvahiIfIndex interface, AvahiProtocol protocol,
       AvahiResolverEvent event, const char *name, const char *type, const char *domain, const char *host_name,
       const AvahiAddress *address, uint16_t port, AvahiStringList *txt, AvahiLookupResultFlags flags, void* userdata) noexcept;

#pragma mark WIFIDeviceManager

WIFIDeviceManager::WIFIDeviceManager(Muxer *mux)
: DeviceManager(mux)
, _simple_poll(nullptr)
, _avahi_client(nullptr)
, _avahi_sb(nullptr)
, _avahi_sb2(nullptr)
, _shouldRestart(false)
, _isStopping(false)
{
   debug("WIFIDeviceManager avahi-client");
   init_avahi();

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

    cleanup_avahi();
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
        if (!_simple_poll) {
            try {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                init_avahi();
            } catch (tihmstar::exception &e) {
                if (_isStopping) {
                    return true;
                }
                error("Failed to initialize avahi discovery with error=%d (%s)", e.code(), e.what());
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
        }

        _shouldRestart = false;
        int err = avahi_simple_poll_loop(_simple_poll);
        if (_isStopping) {
            debug("WIFIDeviceManager avahi main loop stopping");
            return true;
        }

        if (!_shouldRestart && err == 0) {
            debug("WIFIDeviceManager avahi main loop finished without restart request");
            return true;
        }

        warning("WIFIDeviceManager avahi loop exited err=%d restart=%s; reinitializing discovery", err, _shouldRestart ? "YES" : "NO");
        cleanup_avahi();
    }
}

void WIFIDeviceManager::stopAction() noexcept{
    _isStopping = true;
    if (_simple_poll) {
        avahi_simple_poll_quit(_simple_poll);
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

void WIFIDeviceManager::init_avahi(){
    int err = 0;

    assure(_simple_poll = avahi_simple_poll_new());
    retassure(_avahi_client = avahi_client_new(avahi_simple_poll_get(_simple_poll), (AvahiClientFlags)0, avahi_client_callback, this, &err),
        "Failed to start avahi_client with error=%d. Is the daemon running?",err);
    assure(!err);

    assure(_avahi_sb = avahi_service_browser_new(_avahi_client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, "_apple-mobdev2._tcp", NULL, (AvahiLookupFlags)0, avahi_browse_callback, this));
    assure(_avahi_sb2 = avahi_service_browser_new(_avahi_client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, "_remotepairing-manual-pairing._tcp", NULL, (AvahiLookupFlags)0, avahi_browse_callback, this));
    debug("WIFIDeviceManager created avahi service_browser for wifi rediscovery");
}

void WIFIDeviceManager::cleanup_avahi() noexcept{
    safeFreeCustom(_avahi_sb,avahi_service_browser_free);
    safeFreeCustom(_avahi_sb2,avahi_service_browser_free);
    safeFreeCustom(_avahi_client,avahi_client_free);
    safeFreeCustom(_simple_poll,avahi_simple_poll_free);
}

void WIFIDeviceManager::request_restart() noexcept{
    _shouldRestart = true;
    if (_simple_poll) {
        avahi_simple_poll_quit(_simple_poll);
    }
}

void WIFIDeviceManager::request_manual_refresh() noexcept{
    if (_isStopping) {
        debug("Ignoring avahi manual refresh during shutdown");
        return;
    }
    warning("WIFIDeviceManager manual avahi refresh requested");
    request_restart();
}

void WIFIDeviceManager::request_rediscovery_after_pairing(const char *udid) noexcept{
    if (_isStopping) {
        debug("Ignoring avahi rediscovery-after-pairing request during shutdown udid=%s", udid ? udid : "<null>");
        return;
    }
    warning("WIFIDeviceManager requesting avahi rediscovery after new pairing udid=%s", udid ? udid : "<null>");
    request_restart();
}

void WIFIDeviceManager::request_device_rediscovery(const char *serial, const char *serviceName) noexcept{
    if (_isStopping) {
        debug("Ignoring avahi rediscovery request during shutdown serial=%s service=%s", serial ? serial : "<null>", serviceName ? serviceName : "<null>");
        return;
    }
    warning("WIFIDeviceManager requesting avahi rediscovery after wifi device loss serial=%s service=%s", serial ? serial : "<null>", serviceName ? serviceName : "<null>");
    request_restart();
}

#pragma mark avahi_callback implementations

void avahi_client_callback(AvahiClient *c, AvahiClientState state, void *userdata) noexcept{
   WIFIDeviceManager *devmgr = (WIFIDeviceManager*)userdata;
   /* Called whenever the client or server state changes */
   if (state == AVAHI_CLIENT_FAILURE) {
       warning("Server connection failure: %s", avahi_strerror(avahi_client_errno(c)));
       devmgr->request_restart();
   }
}

void avahi_browse_callback(AvahiServiceBrowser *b, AvahiIfIndex interface, AvahiProtocol protocol, AvahiBrowserEvent event,
       const char *name, const char *type, const char *domain, AvahiLookupResultFlags flags, void* userdata) noexcept{
   WIFIDeviceManager *devmgr = (WIFIDeviceManager*)userdata;

   switch (event) {
   case AVAHI_BROWSER_FAILURE:
       warning("(Browser) %s", avahi_strerror(avahi_client_errno(avahi_service_browser_get_client(b))));
       devmgr->request_restart();
       return;
   case AVAHI_BROWSER_NEW:
       info("(Browser) NEW: service '%s' of type '%s' in domain '%s' interface=%d proto=%d", name, type, domain, interface, protocol);
       /* We ignore the returned resolver object. In the callback
          function we free it. If the server is terminated before
          the callback function is called the server will free
          the resolver for us. */
       if (!(avahi_service_resolver_new(devmgr->_avahi_client, interface, protocol, name, type, domain, AVAHI_PROTO_UNSPEC, (AvahiLookupFlags)0, avahi_resolve_callback, userdata)))
           debug("Failed to resolve service '%s': %s\n", name, avahi_strerror(avahi_client_errno(devmgr->_avahi_client)));
       break;
   case AVAHI_BROWSER_REMOVE:
        warning("(Browser) REMOVE: service '%s' of type '%s' in domain '%s' interface=%d proto=%d", name, type, domain, interface, protocol);
        if (type && strstr(type, "_remotepairing-manual-pairing._tcp")) {
            std::string serial = std::string("WIFIPAIR-") + name;
            warning("(Browser) REMOVE pairing service cleanup serial=%s", serial.c_str());
            devmgr->_mux->delete_wifi_device_with_serial(serial);
        }
        break;
   case AVAHI_BROWSER_ALL_FOR_NOW:
   case AVAHI_BROWSER_CACHE_EXHAUSTED:
       info("(Browser) %s\n", event == AVAHI_BROWSER_CACHE_EXHAUSTED ? "CACHE_EXHAUSTED" : "ALL_FOR_NOW");
       break;
   }
}

void avahi_resolve_callback(AvahiServiceResolver *r, AvahiIfIndex interface, AvahiProtocol protocol,
        AvahiResolverEvent event, const char *name, const char *type, const char *domain, const char *host_name,
        const AvahiAddress *address, uint16_t port, AvahiStringList *txt, AvahiLookupResultFlags flags, void* userdata) noexcept{
    char addr[AVAHI_ADDRESS_STR_MAX] = {};
    int err = 0;
    WIFIDeviceManager *devmgr = (WIFIDeviceManager*)userdata;
    char *t = NULL;
    std::shared_ptr<WIFIDevice> dev = nullptr;

    /* Called whenever a service has been resolved successfully or timed out */
    switch (event) {
        case AVAHI_RESOLVER_FAILURE:
            warning("(Resolver) Failed to resolve service '%s' of type '%s' in domain '%s': %s", name, type, domain, avahi_strerror(avahi_client_errno(avahi_service_resolver_get_client(r))));
            break;
        case AVAHI_RESOLVER_FOUND: {
            // TODO: inform muxer about devices leaving
            avahi_address_snprint(addr, sizeof(addr), address);
            t = avahi_string_list_to_string(txt);
            info("(Resolver) FOUND service '%s' type='%s' domain='%s' host='%s' addr='%s' port=%u interface=%d protocol=%d flags=0x%x txt='%s'",
                    name ? name : "<null>",
                    type ? type : "<null>",
                    domain ? domain : "<null>",
                    host_name ? host_name : "<null>",
                    addr,
                    port,
                    interface,
                    protocol,
                    flags,
                    t ? t : "<null>");
            std::string serviceName{name};
            std::string macAddr{serviceName.substr(0,serviceName.find("@"))};
            std::string uuid;
            serviceName += ".";
            serviceName += type;


            std::vector<std::string> addrs{addr};
            bool notifyadd = true;
            if (strstr(serviceName.c_str(), "_remotepairing-manual-pairing._tcp")) {
                uuid = "WIFIPAIR-"+serviceName.substr(0,serviceName.find("."));
                macAddr = {};
                if (devmgr->_mux->have_wifi_device_with_ip(addrs)) {
                    debug("Skipping pairing wifi device rediscovery for service='%s' ip='%s' because device with ip already exists", serviceName.c_str(), addrs.size() ? addrs.front().c_str() : "<none>");
                    goto error;
                }
                notifyadd = false;
            }else{
                try{
                    uuid = sysconf_udid_for_macaddr(macAddr);
                }catch (tihmstar::exception &e){
                    warning("failed to find uuid for mac=%s with error=%d (%s)",macAddr.c_str(),e.code(),e.what());
                    break;
                }

                if (auto existing = devmgr->_mux->get_wifi_device_with_serial(uuid)) {
                    warning("Updating existing wifi device serial=%s service='%s' ip='%s'", uuid.c_str(), serviceName.c_str(), addrs.size() ? addrs.front().c_str() : "<none>");
                    existing->updateDiscoveryInfo(addrs, serviceName, interface);
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
                    warning("Adding wifi device from avahi discovery serial=%s service='%s' ip='%s' notify=%s", uuid.c_str(), serviceName.c_str(), addrs.size() ? addrs.front().c_str() : "<none>", notifyadd ? "YES" : "NO");
                    dev = std::make_shared<WIFIDevice>(devmgr->_mux, devmgr, uuid, addrs, serviceName, interface);
                    devmgr->device_add(dev, notifyadd); dev = NULL;
                } catch (tihmstar::exception &e){
                    creterror("failed to construct device with error=%d (%s)",e.code(),e.what());
                }
            }

            break;
        }
        default:
            error("unknown event=%d",event);
            break;
    }

error:
    avahi_service_resolver_free(r);
    if (t){
        avahi_free(t);
    }
    if (dev){
        dev->kill();
    }
}

#endif //HAVE_WIFI_SUPPORT
