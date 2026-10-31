#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#include <algorithm>
#include <ArduinoOTA.h>
#include <iostream>
#include <sstream>

#include "DNS.h"
#include "Module.h"
#include "ModuleVersionCheck.h"
#include "NtpTimeProvider.h"

#ifndef ParamNET_mDNS
#define ParamNET_mDNS true
#endif

#if defined(ARDUINO_ARCH_ESP32)
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#define CALLBACK_CLASS Network
#define CALLBACK_EVENT arduino_event_id_t
#else
#define CALLBACK_CLASS WiFi
#define CALLBACK_EVENT WiFiEvent_t
#endif

#if defined(KNX_IP_LAN)
#include <ETH.h>
#define KNX_NETIF ETH
#elif defined(KNX_IP_WIFI)
#define KNX_NETIF WiFi
#else
#pragma warn "Missing KNX_IP_LAN or KNX_IP_WIFI"
#endif
#elif defined(ARDUINO_ARCH_RP2040)
#include <SimpleMDNS.h>

#include <lwip/netif.h>
#if defined(KNX_IP_LAN)
#include "W5500lwIP.h"
#ifdef PIN_ETH_INT
Wiznet5500lwIP KNX_NETIF(PIN_ETH_SS, ETH_SPI_INTERFACE, PIN_ETH_INT);
#else
Wiznet5500lwIP KNX_NETIF(PIN_ETH_SS, ETH_SPI_INTERFACE);
#endif
#elif defined(KNX_IP_WIFI)
#else
#pragma warn "Missing KNX_IP_LAN or KNX_IP_WIFI"
#endif

WiFiUDP Udp;
#else
#pragma warn "Unsupported platform"
#endif

namespace OpenKNX
{
    namespace Network
    {

        const std::string Module::name()
        {
            return "Network";
        }

        // You can also give it a version
        // will be displayed in Command Infos
        const std::string Module::version()
        {
            return MODULE_Network_Version;
        }

        /*
         * Internal use: Developer only
         */
        void Module::resetNetwork()
        {
            logInfoP("Reset network adapter");
            logIndentUp();
            _ipShown = false; // let checkIpStatus() rebuild the KNXnet/IP endpoint after the bounce
            _ipStableSince = 0;

            controlKnxIp(false);

#if defined(KNX_IP_WIFI) && defined(ARDUINO_ARCH_ESP32)
            KNX_NETIF.disconnect(true, true);
            KNX_NETIF.mode(WIFI_OFF);
#else
            KNX_NETIF.end();
#endif
            delay(500);
            initPhy();
            initIp();
            controlKnxIp(true); // ESP32: the pending STOP/START down request closes it again; GOT_IP or the link check re-opens it

            logInfoP("Complete");
            logIndentDown();
        }

        void Module::initPhy()
        {
            logDebugP("Initialize network adapter");
            logIndentUp();
#if defined(PIN_ETH_RES)
            logDebugP("Resetting Ethernet Phy...");
            pinMode(PIN_ETH_RES, OUTPUT);
            digitalWrite(PIN_ETH_RES, LOW);
            delay(600);
            digitalWrite(PIN_ETH_RES, HIGH);
#endif

#if defined(ETH_SPI_INTERFACE)
            // initalize SPI
            ETH_SPI_INTERFACE.setRX(PIN_ETH_MISO);
            ETH_SPI_INTERFACE.setTX(PIN_ETH_MOSI);
            ETH_SPI_INTERFACE.setSCK(PIN_ETH_SCK);
            ETH_SPI_INTERFACE.setCS(PIN_ETH_SS);
            logDebugP("Ethernet SPI GPIO: RX/MISO: %d, TX/MOSI: %d, SCK/SCLK: %d, CSn/SS: %d", PIN_ETH_MISO, PIN_ETH_MOSI, PIN_ETH_SCK, PIN_ETH_SS);
#endif

            logIndentDown();
        }

        void Module::loadSettings()
        {
#ifdef KNX_IP_WIFI
            readWifiSettings();
#endif

            // build default hostname
            memcpy(_hostName, "OpenKNX-", 8);
            memcpy(_hostName + 8, openknx.info.humanSerialNumber().c_str() + 5, 8);
            logTraceP("Default hostname: %s", _hostName);

            if (knx.configured())
            {
                // custom hostname
#ifdef ParamNET_CustomHostname
                if (ParamNET_CustomHostname && strlen((char *)ParamNET_HostName) > 0)
                {
                    logDebugP("Read hostname from parameters");
                    memcpy(_hostName, ParamNET_HostName, 24);
                }
#endif

#if !defined(ParamNET_HostAddress) || !defined(ParamNET_SubnetMask) || !defined(ParamNET_GatewayAddress) || !defined(ParamNET_NameserverAddress) || !defined(ParamNET_StaticIP) || defined(OPENKNX_NETWORK_USEIPPROP)

                logInfoP("Read ip settings from properties");
                _useStaticIP = GetByteProperty(PID_IP_ASSIGNMENT_METHOD) == 1; // see 2.5.6 of 03_08_03
                if (_useStaticIP)
                {
                    _staticGatewayIP = GetIpProperty(PID_DEFAULT_GATEWAY);
                    _staticSubnetMask = GetIpProperty(PID_SUBNET_MASK);
                    _staticLocalIP = GetIpProperty(PID_IP_ADDRESS);
                }
#else
                logInfoP("Read ip settings from parameters");
                _useStaticIP = ParamNET_StaticIP;
                if (_useStaticIP)
                {
                    _staticLocalIP = htonl(ParamNET_HostAddress);
                    _staticSubnetMask = htonl(ParamNET_SubnetMask);
                    _staticGatewayIP = htonl(ParamNET_GatewayAddress);
                    _staticNameServerIP = htonl(ParamNET_NameserverAddress);
                }
#endif
            }
            else
            {
                // PID_FRIENDLY_NAME is used to identify the device over Search Request from ETS. If not configured, PID_FRIENDLY_NAME is empty and so is the Name in the SearchReqest.
                // set PID_FRIENDLY_NAME to the _hostname in this case, so "OpenKNX-XXXXXX" is display in the ETS
                // since the friendlyName can be set while being unconfigured (is set while assigning PA), check if 0
                uint8_t elements = 30;               // request 30 items - update by read
                uint32_t length = 0;                 // update by read
                uint8_t *friendlyNameRead = nullptr; // new init by read
                knx.bau().propertyValueRead(OT_IP_PARAMETER, 0, PID_FRIENDLY_NAME, elements, 1, &friendlyNameRead, length);
                if (elements == 0)
                {
                    uint8_t friendlyNameWrite[30] = {};
                    memcpy(friendlyNameWrite, _hostName, strlen(_hostName));
                    elements = 30;
                    // length is an INPUT here: the bau discards the write if it is shorter than elements * ElementSize().
                    length = sizeof(friendlyNameWrite);
                    knx.bau().propertyValueWrite(OT_IP_PARAMETER, 0, PID_FRIENDLY_NAME, elements, 1, friendlyNameWrite, length);
                }

                delete[] friendlyNameRead; // propertyValueRead() allocates the buffer, we have to free it
            }

            // PID_IP_CAPABILITIES = which AUTOMATIC assignment methods this IP stack can do (03_08_03 2.5.7: bit0 BootP,
            // bit1 DHCP, bit2 AutoIP; manual is always implicit and has no bit). Set UNCONDITIONALLY: capabilities are
            // fixed and do not depend on the current config.
#ifdef OPENKNX_NETWORK_AUTOIP
            // AutoIP build: lwIP runs the DHCP<->AutoIP coop, so the stack advertises DHCP (bit1) AND AutoIP (bit2) = 6.
            SetByteProperty(PID_IP_CAPABILITIES, 6); // DHCP + AutoIP
#else
            // This stack does DHCP only (lwIP, no BootP, no AutoIP) -> value 2.
            SetByteProperty(PID_IP_CAPABILITIES, 2); // DHCP only
#endif

            // PID_CURRENT_IP_ASSIGNMENT_METHOD reflects the FINAL resolved assignment method actually IN USE (override included).
            if (_useStaticIP)
                SetByteProperty(PID_CURRENT_IP_ASSIGNMENT_METHOD, 1); // manual / static
            else
                SetByteProperty(PID_CURRENT_IP_ASSIGNMENT_METHOD, 4); // DHCP (03_08_03 2.5.5: 1=manual, 2=BootP, 4=DHCP, 8=AutoIP)

#ifdef KNX_IP_WIFI
            if (strlen(_wifiSSID) == 0)
                logErrorP("No WiFI Settings found!");
#endif

            logInfoP(_useStaticIP ? "Using static IP" : "Using DHCP");

            // Hostname
            logInfoP("Hostname: %s", _hostName);
#if defined(ARDUINO_ARCH_ESP32)
            KNX_NETIF.setHostname(_hostName);
#else
            KNX_NETIF.hostname(_hostName);
#endif
        }

        void Module::init()
        {
            logInfoP("Initialize IP stack");
            logIndentUp();
            initPhy();
            loadSettings();
#ifdef ARDUINO_ARCH_ESP32
            // Once only: applyLocalNetworkConfig() re-runs loadSettings(), and a second callback would be appended.
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
            ::Network.onEvent([](arduino_event_id_t event) -> void { openknxNetwork.esp32NetworkEvent(event); });
#else
            WiFi.onEvent([](WiFiEvent_t event) -> void { openknxNetwork.esp32NetworkEvent(event); });
#endif
#endif
            initIp();
            logIndentDown();
        }

#ifdef ARDUINO_ARCH_ESP32
        /**
         * @brief Runs on the "arduino_events" task; KNX-IP changes are only recorded here, loop() applies them.
         */
        void Module::esp32NetworkEvent(CALLBACK_EVENT event)
        {
            switch (event)
            {
                case ARDUINO_EVENT_WIFI_READY:
                case ARDUINO_EVENT_WIFI_STA_START:
                    // do nothing
                    break;
                case ARDUINO_EVENT_ETH_START:
                    logDebugP("Event: Start");
                    // The hostname must be set after the interface is started, but needs
                    // to be set before DHCP, so set it from the event handler thread.
                    KNX_NETIF.setHostname(_hostName);
                    requestKnxIp(false);

                    break;
                case ARDUINO_EVENT_ETH_CONNECTED:
                case ARDUINO_EVENT_WIFI_STA_CONNECTED:
                    logDebugP("Event: Connected");
                    espConnected = true;
                    break;
                case ARDUINO_EVENT_ETH_GOT_IP:
                case ARDUINO_EVENT_WIFI_STA_GOT_IP:
                    logDebugP("Event: Got IP");
                    requestKnxIp(true);
                    break;
                case ARDUINO_EVENT_ETH_LOST_IP:
                case ARDUINO_EVENT_WIFI_STA_LOST_IP:
                    logDebugP("Event: Lost IP");
                    requestKnxIp(false);
                    break;
                case ARDUINO_EVENT_ETH_DISCONNECTED:
                case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
                    logDebugP("Event: Disconnected");
                    requestKnxIp(false);
                    espConnected = false;
                    break;
                case ARDUINO_EVENT_ETH_STOP:
                case ARDUINO_EVENT_WIFI_STA_STOP:
                    logDebugP("Event: Stop");
                    requestKnxIp(false);
                    espConnected = false;
                    break;
                default:
                    logDebugP("Event: Ignored %i", event);
                    break;
            }
        }

        /**
         * @brief Event task: a down event replaces any pending request with DOWN; GOT_IP adds REBIND to it.
         * The last event wins. Router: after a DOWN, the REBIND or the link check in checkLinkStatus() re-joins.
         */
        void Module::requestKnxIp(bool rebind)
        {
            if (rebind)
                _knxIpRequest.fetch_or(KNXIP_REQ_REBIND, std::memory_order_release);
            else
                _knxIpRequest.exchange(KNXIP_REQ_DOWN, std::memory_order_release); // RMW like the consumer's
        }

        /**
         * @brief loop() context: applies the recorded request, DOWN before REBIND (the order they occurred in).
         * A re-bind costs telegrams, so only on an address change or when the endpoint is not up.
         */
        void Module::applyKnxIpRequest()
        {
            const uint32_t req = _knxIpRequest.exchange(0, std::memory_order_acquire);
            if (req & KNXIP_REQ_DOWN) controlKnxIp(false);
            if (!(req & KNXIP_REQ_REBIND)) return;

            const IPAddress ip = localIP();
            if (ip == IPAddress()) return; // address gone again; the next GOT_IP requests the bind
            if (ip != _boundIp || !knxIpEnabled())
            {
                controlKnxIp(false);
                controlKnxIp(true);
                if (knxIpEnabled()) _boundIp = ip;
            }
        }
#endif

        void Module::initIp()
        {
#ifdef ARDUINO_ARCH_ESP32
#ifdef KNX_IP_WIFI
            KNX_NETIF.config(_staticLocalIP, _staticGatewayIP, _staticSubnetMask, _staticNameServerIP);
            KNX_NETIF.mode(WIFI_STA);
            KNX_NETIF.setAutoReconnect(true);
            if (strlen(_wifiSSID) > 0)
                KNX_NETIF.begin(_wifiSSID, _wifiPassphrase);
            else
                KNX_NETIF.begin();
#else
            KNX_NETIF.begin();
            KNX_NETIF.config(_staticLocalIP, _staticGatewayIP, _staticSubnetMask, _staticNameServerIP); // Stupid: Nedd to be after begin an also DHCP!
#endif

#elif ARDUINO_ARCH_RP2040
#ifdef KNX_IP_WIFI
            if (strlen(_wifiSSID) > 0)
            {
                KNX_NETIF.config(_staticLocalIP, _staticNameServerIP, _staticGatewayIP, _staticSubnetMask);
                logInfoP("Connecting to WiFi \"%s\"", _wifiSSID);
                KNX_NETIF.setTimeout(2000); // 2000 ms = 4 seconds (timeout is multiplied by 2)
                KNX_NETIF.begin(_wifiSSID, _wifiPassphrase);
            }
#else

            if (!KNX_NETIF.config(_staticLocalIP, _staticGatewayIP, _staticSubnetMask, _staticNameServerIP))
            {
                logIndentUp();
                logErrorP("Invalid IP settings");
                logIndentDown();
            }

            KNX_NETIF.setSPISpeed(OPENKNX_NET_SPI_SPEED);
            if (!KNX_NETIF.begin())
                openknx.hardware.fatalError(FATAL_NETWORK, "Error communicating with W5500 Ethernet chip");
#endif
#endif
        }

        void Module::setup(bool configured)
        {
#ifdef HAS_USB
            openknxUsbExchangeModule.onLoad("Network.txt", [this](UsbExchangeFile *file) { this->fillNetworkFile(file); });
#ifdef KNX_IP_WIFI
            openknxUsbExchangeModule.onLoad("Wifi.txt", [this](UsbExchangeFile *file) { this->fillWifiFile(file); });
            openknxUsbExchangeModule.onEject("Wifi.txt", [this](UsbExchangeFile *file) { return this->readWifiFile(file); });
#endif
#endif

            registerCallback([this](bool state) { if (state) this->showNetworkInformations(false); });

            _ipLedFunc = openknx.ledFunctions.get(OPENKNX_LEDFUNC_NET_STATE);

            if (!configured || ParamNET_mDNS)
            {
                logDebugP("Start mDNS");
                if (!MDNS.begin(_hostName)) logErrorP("Hostname not applied (mDNS)");

#ifdef ARDUINO_ARCH_ESP32
                MDNS.addService("openknx", "tcp", -1);
                MDNS.addServiceTxt("openknx", "tcp", "serial", openknx.info.humanSerialNumber().c_str());
                MDNS.addServiceTxt("openknx", "tcp", "version", openknx.info.humanFirmwareVersion().c_str());
                MDNS.addServiceTxt("openknx", "tcp", "firmware", openknx.info.humanFirmwareNumber().c_str());
                MDNS.addServiceTxt("openknx", "tcp", "address", openknx.info.humanIndividualAddress().c_str());
                MDNS.addServiceTxt("openknx", "tcp", "ota", _otaPortString);
                MDNS.addServiceTxt("openknx", "tcp", "configured", knx.configured() ? "1" : "0");
#else
                hMDNSService service = MDNS.addService("openknx", "tcp", -1);
                MDNS.addServiceTxt(service, "serial", openknx.info.humanSerialNumber().c_str());
                MDNS.addServiceTxt(service, "version", openknx.info.humanFirmwareVersion().c_str());
                MDNS.addServiceTxt(service, "firmware", openknx.info.humanFirmwareNumber().c_str());
                MDNS.addServiceTxt(service, "address", openknx.info.humanIndividualAddress().c_str());
                MDNS.addServiceTxt(service, "ota", _otaPortString);
                MDNS.addServiceTxt(service, "configured", (uint8_t)knx.configured());
#endif
                MDNS.enableArduino(_otaPort /* default port for ota */, false /* AUTH true / false */);
            }

#ifdef ParamNET_NTP
            if (ParamNET_NTP)
            {
                openknx.time.setTimeProvider(new NtpTimeProvider());
            }
#endif

#ifdef ARDUINO_ARCH_ESP32
            ArduinoOTA.setMdnsEnabled(false); // handle global
#endif
            ArduinoOTA.setPort(_otaPort);
            ArduinoOTA.setRebootOnSuccess(false);
            ArduinoOTA.onStart([&]() {
                _otaActive = true; // display OTA overlay takes over (SYSTEM priority)
                if (ArduinoOTA.getCommand() == U_FLASH)
                    logInfo("OTA", "Start updating firmware");
                else // U_SPIFFS
                    logInfo("OTA", "Start updating filesystem");
            });
            ArduinoOTA.onEnd([&]() {
                _otaActive = false;
                logIndentUp();
                logInfo("OTA", "Update complete");
                logIndentDown();
                openknx.restart();
            });
            ArduinoOTA.onProgress([&](unsigned int progress, unsigned int total) {
                int percent = (int)progress / (total / 100.0);
                if (percent % 10 == 0 && _otaProgress != percent)
                {
                    logIndentUp();
                    logInfo("OTA", "Progress: %d%%", percent);
                    logIndentDown();
                    _otaProgress = percent;
                }
                openknx.loop();
            });
            ArduinoOTA.onError([&](ota_error_t error) {
                _otaActive = false; // release the display overlay so normal operation resumes
                logIndentUp();
                if (error == OTA_AUTH_ERROR)
                    logError("OTA", "Auth error");
                else if (error == OTA_BEGIN_ERROR)
                    logErrorP("OTA", "Begin error");
                else if (error == OTA_CONNECT_ERROR)
                    logError("OTA", "Connect error");
                else if (error == OTA_RECEIVE_ERROR)
                    logError("OTA", "Receive error");
                else if (error == OTA_END_ERROR)
                    logError("OTA", "End error");
                logIndentDown();
            });

#if defined(OPENKNX_TELEGRAMJSON) && defined(OPENKNX_MQTT)
            // setup() runs once, so registerReceivedFrame() cannot stack up (it appends
            // to a vector). knx.start() comes later; the callback just stays idle until then.
            if (configured && ParamNET_MQTT && ParamNET_MQTTTPRawData)
            {
                _telegramTopic = mqtt.devicePrefix() + "knx/tp/telegramm";
                tpUart().registerReceivedFrame(
                    [](TPUart::Frame &frame) { openknxNetwork.publishTelegram(frame); });
            }
#endif
        }


#ifdef HAS_USB
        void Module::fillNetworkFile(UsbExchangeFile *file)
        {
            writeLineToFile(file, "OpenKNX Network");
            writeLineToFile(file, "-----------------");
            writeLineToFile(file, "");
            writeLineToFile(file, "Hostname: %s", _hostName);
            writeLineToFile(file, "Network: %s", established() ? "Established" : "Disconnected");
            writeLineToFile(file, _useStaticIP ? "Using static IP" : "Using DHCP");
            if (established())
            {
                writeLineToFile(file, "IP-Address: %s", localIP().toString().c_str());
                writeLineToFile(file, "Netmask: %s", subnetMask().toString().c_str());
                writeLineToFile(file, "Gateway: %s", gatewayIP().toString().c_str());
                writeLineToFile(file, "DNS: %s", nameServerIP().toString().c_str());
                // writeLineToFile(file, "Mode: %s", phyMode().c_str()); Currently not supported
            }
        }

#ifdef KNX_IP_WIFI
        void Module::fillWifiFile(UsbExchangeFile *file)
        {
            writeLineToFile(file, "SSID");
            writeLineToFile(file, "Password");
        }

        static std::string trimString(const std::string &str)
        {
            auto start = std::find_if_not(str.begin(), str.end(), ::isspace);
            auto end = std::find_if_not(str.rbegin(), str.rend(), ::isspace).base();
            return (start < end ? std::string(start, end) : "");
        }

        bool Module::readWifiFile(UsbExchangeFile *file)
        {
            openknx.watchdog.loop();
            std::string ssid;
            std::string password;

            char tmp[100] = {};
            if (file->fgets(tmp, 99) > 0) ssid = tmp;

            memset(tmp, 0x0, 100);
            if (file->fgets(tmp, 99) > 0) password = tmp;

            ssid = trimString(ssid);
            password = trimString(password);

            logInfoP("Read Wifi settings from Wifi.txt");
            logIndentUp();
            if (ssid.length() == 0 || ssid.length() > 32 || ssid == "SSID")
            {
                logErrorP("SSID is invalid");
                return false;
            }
            if (password.length() == 0 || password.length() > 63)
            {
                logErrorP("Password is invalid");
                return false;
            }

            logInfoP("SSID: %s", ssid.c_str());
            logIndentDown();

            saveWifiSettings(ssid.c_str(), password.c_str(), true);
            return true;
        }
#endif
#endif

        // Establish debounce: only print "Network established" + fire the establish callbacks once the link has
        // genuinely held this long, not on every brief establish during the ETH auto-fallback search / flapping.
        static constexpr uint32_t NET_INFO_STABLE_MS = 3000;

        // Carrier for the link edge logic. On RP2040 + W5500 the read is a PHYCFGR fetch over the SPI bus the
        // driver shares, so a garbled read has to be absorbed. It is debounced DOWNWARDS ONLY: a corrupt read
        // shows up as "not linked", which is what must not drop DHCP, while a read that says "linked" cannot
        // be that kind of glitch. Debouncing both directions kept the accepted state at its initial false for
        // good on a link that flaps faster than the window, and the fallback ladder, which only acts on carrier
        // edges, then never saw one. Everywhere else the platform reports the link from an event.
        bool Module::linkCarrier()
        {
            const bool raw = connected();
#if defined(ARDUINO_ARCH_RP2040) && defined(OPENKNX_ETH_W5500) && defined(KNX_IP_LAN)
            if (raw)
            {
                _carrierSamples = 0;
                _carrierStable = true;
                return true;
            }
            if (!_carrierStable)
            {
                _carrierSamples = 0;
                return false;
            }
            if (++_carrierSamples < LINK_DEBOUNCE_SAMPLES) return true; // still counted as up while in doubt
            _carrierSamples = 0;
            _carrierStable = false;
            return false;
#else
            return raw;
#endif
        }

#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN)
        // arduino-pico's W5500 driver never calls netif_set_link_up/down, so lwIP misses cable changes and
        // DHCP stays on the stale lease. Feed both edges so dhcp_discover() re-runs and DHCP/AutoIP re-arm.

        void Module::setLwipLinkState(bool up)
        {
            netif *intf = KNX_NETIF.getNetIf();
            if (intf == nullptr) return;

            ethernet_arch_lwip_begin();
            if (up)
                netif_set_link_up(intf);
            else
                netif_set_link_down(intf);
            ethernet_arch_lwip_end();
        }
#endif


        // Link-down housekeeping. Also called by the W5500 recovery, which bypasses checkLinkStatus() and
        // would otherwise leave lwIP's link-up flag set and the DHCP/AutoIP handover unarmed.
        void Module::onLinkLost()
        {
            // idempotent: both the down edge and recoverEth() call it; running twice would restart the outage timer.
            if (!_currentLinkState) return;

#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN)
            setLwipLinkState(false); // must run while the netif still exists (before any end())
#endif
            _currentLinkState = false;
            _ipShown = false;
            _ipStableSince = 0; // restart the establish debounce on link loss
            _linkDownSinceSec = uptime();
            _lastLinkUptimeSec = _linkUpSinceSec ? (_linkDownSinceSec - _linkUpSinceSec) : 0;
            _linkUpSinceSec = 0;
            _establishedSinceSec = 0;
            _linkEverDown = true;
            loadCallbacks(false);
        }

        void Module::checkIpStatus()
        {
            if (_ipShown || !established()) return;

            if (_ipStableSince == 0) _ipStableSince = millis();
            if ((uint32_t)(millis() - _ipStableSince) < NET_INFO_STABLE_MS) return;

            _establishedSinceSec = uptime();

            // A multicast membership does not survive every link change; 03_08_02 Core 4.2 requires the
            // server to stay discoverable. The BAU knows whether it owns an endpoint.
            if (!knx.bau().networkChanged(_linkEverDown))
            {
                logErrorP("KNXnet/IP endpoint down: multicast join failed - device is not discoverable");
                _ipStableSince = 0; // leaves _ipShown false: the next tick tries again
                return;
            }

            logBegin();
            logInfoP("Network established");
            logIndentUp();
            loadCallbacks(true);
            logIndentDown();
            logEnd();

#ifdef OPENKNX_NETWORK_AUTOIP
            // AutoIP (RFC 3927) coop fallback: when no DHCP server answered, lwIP self-assigns a link-local
            // 169.254.0.0/16 address after LWIP_DHCP_AUTOIP_COOP_TRIES DISCOVERs. Reflect the method actually IN
            // USE in PID_CURRENT_IP_ASSIGNMENT_METHOD (03_08_03 2.5.5: 8=AutoIP, 4=DHCP) so ETS/ftc report it right.
            // Evaluated here (IP is valid + link-stable), not in loadSettings() where the IP is not yet known; re-runs
            // on each reconnect (_ipShown is cleared on link loss). Only the DHCP case is refined -- a static/manual
            // config keeps the method already resolved in loadSettings().
            if (!_useStaticIP)
            {
                IPAddress ip = localIP();
                SetByteProperty(PID_CURRENT_IP_ASSIGNMENT_METHOD,
                                (ip[0] == 169 && ip[1] == 254) ? 8 : 4); // 8 = AutoIP link-local, 4 = DHCP lease
            }
#endif

            _ipShown = true;
        }

        // 03_08_03 3.5.3 p.20: the IP-fault bit of PID_KNXNETIP_DEVICE_STATE is set once communication to the
        // IP network has failed for five seconds and cleared when it resumes. established() is the accurate
        // source (physical link up AND an address assigned); the current IP address alone would miss a cable
        // pull on a static configuration. Read-modify-write so the KNX-fault bit the stack owns is preserved,
        // and only on a change so the evented M_PropInfo.ind fires on the edge, not every 500 ms.
        void Module::checkKnxIpDeviceState(bool established)
        {
            const uint32_t now = millis();

            if (established)
                _ipFaultSince = 0;
            else if (_ipFaultSince == 0)
                _ipFaultSince = now ? now : 1; // 0 marks "no fault pending", so never store it as a timestamp

            const bool fault = (_ipFaultSince != 0) && (now - _ipFaultSince >= 5000);
            if (fault == _ipFaultReported)
                return; // edge only: GetByteProperty() allocates, and this runs twice a second
            _ipFaultReported = fault;

            const uint8_t current = GetByteProperty(PID_KNXNETIP_DEVICE_STATE);
            const uint8_t next = fault ? (uint8_t)(current | 0x02) : (uint8_t)(current & ~0x02);
            if (next != current)
                SetByteProperty(PID_KNXNETIP_DEVICE_STATE, next);
        }

        void Module::checkLinkStatus()
        {
            if (!delayCheckMillis(_lastLinkCheck, 500)) return;
            // One carrier read per tick, debounced where it can glitch; everything below derives from it
            // instead of re-reading PHYCFGR, so a single garbled read cannot flip half the tick's decisions.
            const bool carrier = linkCarrier();
            const bool establishedState = carrier && (localIP() != IPAddress());

            // 03_08_03 3.5.3: the IP-fault bit means "IP network cannot be accessed" -- a link with an
            // address but no KNXnet/IP endpoint is exactly that, so it has to feed this too.
            checkKnxIpDeviceState(establishedState && knxIpEnabled());


#ifdef ARDUINO_ARCH_ESP32
            // Level-triggered: follows the link state every tick, so a failing join is retried every 500 ms.
            // Router masks only: on 0x07B0 controlKnxIp() leaves the endpoint to networkChanged().
#if MASK_VERSION == 0x091A || MASK_VERSION == 0x57B0
            const bool knxIpWanted = establishedState;
            if (knxIpWanted != knxIpEnabled())
            {
                const IPAddress ip = localIP();
                controlKnxIp(knxIpWanted);
                if (knxIpWanted && knxIpEnabled()) _boundIp = ip; // a GOT_IP for this address skips the rebind while up
            }
#endif
#endif

            const bool newLinkState = establishedState || carrier;

            if (newLinkState && establishedState)
            {
                if (_ipLedState != 1)
                {
                    _ipLedFunc->color(OpenKNX::Led::Color::Green);
                    _ipLedFunc->activity(OpenKNX::Led::g_ipLedActivity, true);

                    _ipLedState = 1;
                }
            }
            else if (newLinkState)
            {
                if (_ipLedState != 2)
                {
                    _ipLedFunc->color(OpenKNX::Led::Color::Yellow);
                    _ipLedFunc->on(OpenKNX::Led::Capability::COLOR);
                    _ipLedFunc->blinking(1000, OpenKNX::Led::Capability::MONOCHROME);

                    _ipLedState = 2;
                }
            }
            else
            {
#ifdef KNX_IP_WIFI
                // keine vollständigen Zugangsdaten hinterlegt
                if (_wifiSSID[0] == 0 || _wifiPassphrase[0] == 0)
                {
                    if (_ipLedState != 4)
                    {
                        _ipLedFunc->color(OpenKNX::Led::Color::Red);
                        _ipLedFunc->blinking(500);
                        _ipLedState = 4;
                    }
                }
                else
#endif
                    if (_ipLedState != 3)
                {

                    _ipLedFunc->color(OpenKNX::Led::Color::Red);
                    _ipLedFunc->on(OpenKNX::Led::Capability::COLOR);
                    _ipLedFunc->off(OpenKNX::Led::Capability::MONOCHROME);
                    _ipLedState = 3;
                }
            }

            // got link
            if (newLinkState && !_currentLinkState)
            {
#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN)
                setLwipLinkState(true); // DHCP: REBOOTING (reclaims the lease) -> DISCOVER -> AutoIP
#endif
                _linkUpSinceSec = uptime();
                if (_linkEverDown)
                {
                    _lastDowntimeSec = _linkUpSinceSec - _linkDownSinceSec;
                    if (_reconnects < 0xFFFF) _reconnects++;
                    logInfoP("Link connected (down %us, reconnect #%u)",
                             (unsigned)_lastDowntimeSec, (unsigned)_reconnects);
                }
                else
                {
                    logInfoP("Link connected");
                }
                _linkDownSinceSec = 0;
                // #if defined(KNX_IP_W5500)
                // ethernet_arch_lwip_begin();
                // netif_set_link_up(KNX_NETIF.getNetIf());
                // if (_useStaticIP)
                // netif_set_ipaddr(KNX_NETIF.getNetIf(), _staticLocalIP);
                // else
                // dhcp_network_changed_link_up(KNX_NETIF.getNetIf());
                // ethernet_arch_lwip_end();
                // #elif defined(KNX_IP_GENERIC)
                //     if (!established()) KNX_NETIF.maintain();
                // #endif
            }

            // lost link
            else if (!newLinkState && _currentLinkState)
            {
                onLinkLost();
                logInfoP("Link disconnected");
            }

            _currentLinkState = newLinkState;
            _lastLinkCheck = millis();

            if (_currentLinkState) checkIpStatus();

            // set network bit for heartbeat
            if (established())
                openknx.common.extendedHeartbeatValue |= 0b10000000;
            else
                openknx.common.extendedHeartbeatValue &= 0b01111111;
        }

        void Module::loop(bool configured)
        {
#ifdef ARDUINO_ARCH_ESP32
            // Ahead of the _powerSave/_otaActive returns, so KNX-IP follows the network in every state.
            if (_knxIpRequest.load(std::memory_order_relaxed)) applyKnxIpRequest();
#endif
            if (_restartTimer > 0 && delayCheck(_restartTimer, 2000))
            {
                openknx.restart();
            }

            if (_powerSave) return;

            // During OTA the async IRQ pump owns W5500 RX; skip the SPI health-checks (checkEthHealth/
            // checkLinkStatus, and the ping/DNS/webclient loops below) so they don't contend for the SPI bus +
            // lwIP lock in the windows between flash commits. The device is dedicated to the update anyway.
            if (_otaActive) return;
#ifdef OPENKNX_PING
            _pingHandler.loop();
#endif
#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
            OpenKNX::Network::DNS::loop();
#endif
#ifdef OPENKNX_WEBCLIENT
            if (established()) webclient.loop();
#endif

            checkLinkStatus();
            handleOTA();

#ifdef OPENKNX_TELEGRAMJSON
            // Independent of the webserver and of established(): the table lives on
            // LittleFS, already mounted by the time any loop() runs. begin() is
            // idempotent and self-logs, so no extra guard/flag needed here.
            gatable.begin();
#endif

#ifdef OPENKNX_WEBSERVER
            if (ParamNET_HTTP || !knx.configured())
            {
                if (!webserver.isRunning() && established())
                {
                    webserver.setup();
#ifdef OPENKNX_WEBCONSOLE
                    webconsole.setup();
#endif
#ifdef OPENKNX_WEBFS
                    filemanager.setup();
#endif
#if defined(OPENKNX_TELEGRAMJSON) && defined(OPENKNX_WEBMONITOR)
                    groupmonitor.setup();
#endif
                    // Last, not in setup(): only now are all routes, menus and assets
                    // registered. On ESP32 httpd serves from its own task, so a request
                    // landing earlier could iterate a vector while it reallocates.
                    webserver.start();
                }
                if (webserver.isRunning())
                {
                    // Producer vor Transport: webserver.loop() entleert am Ende die
                    // Sendequeue, was diese Module vorher gefüllt haben sollen. In
                    // umgekehrter Reihenfolge ginge alles einen Tick später raus.
#ifdef OPENKNX_WEBCONSOLE
                    webconsole.loop();
#endif
#ifdef OPENKNX_WEBFS
                    filemanager.loop();
#endif
#if defined(OPENKNX_TELEGRAMJSON) && defined(OPENKNX_WEBMONITOR)
                    groupmonitor.loop();
#endif
                    webserver.loop();
                }
            }
#endif
#ifdef OPENKNX_MQTT
            if (established())
            {
                mqtt.setup(configured);
                mqtt.loop(configured);
            }
#endif
        }

#if defined(OPENKNX_TELEGRAMJSON) && defined(OPENKNX_MQTT)
        void Module::publishTelegram(TPUart::Frame &frame)
        {
            // connected() first: while MQTT is down, decoding would be thrown away.
            if (!frame.isFrame() || !mqtt.connected()) return;

            _telegramJson.reset();
            buildTelegramJson(frame, _telegramJson);

            const std::string &json = _telegramJson.str();
            // Queues and returns -- never blocks this callback, see MQTT::Module::publish().
            mqtt.publish(_telegramTopic.c_str(), json.c_str(), json.size());
        }
#endif

        void Module::handleOTA()
        {
            bool allowed = true;
            // An unconfigured device has no ETS params -> ParamNET_OTAUpdate reads erased param memory. Honouring
            // it here can lock OTA out permanently (you would need ETS to enable the very mechanism you want to
            // flash with). Only apply the ETS gate once configured; an unconfigured device keeps OTA open.
            if (knx.configured())
            {
                if (ParamNET_OTAUpdate == 2) allowed = false;
                if (ParamNET_OTAUpdate == 0) allowed = knx.progMode();
            }

            if (_otaAllowed != allowed) // allowed changed
            {
                _otaAllowed = allowed;
                if (_otaAllowed)
#ifdef ARDUINO_ARCH_ESP32
                    ArduinoOTA.begin();
#else
                    ArduinoOTA.begin(false);
#endif
                else
                    ArduinoOTA.end();
            }

            if (_otaAllowed && !_otaHandle)
            {
                _otaHandle = true; // prevent recursion
                ArduinoOTA.handle();
                _otaHandle = false;
            }
        }

        IPAddress Module::GetIpProperty(uint8_t PropertyId)
        {
            uint8_t NoOfElem = 1;
            uint8_t *data;
            uint32_t length;
            knx.bau().propertyValueRead(OT_IP_PARAMETER, 0, PropertyId, NoOfElem, 1, &data, length);
            IPAddress ret = (length == 4) ? (data[3] << 24) + (data[2] << 16) + (data[1] << 8) + data[0] : 0;
            delete[] data;
            return ret;
        }

        void Module::SetIpProperty(uint8_t PropertyId, IPAddress IPAddress)
        {
            uint8_t NoOfElem = 1;
            uint8_t data[4];

            data[0] = IPAddress[0];
            data[1] = IPAddress[1];
            data[2] = IPAddress[2];
            data[3] = IPAddress[3];

            // length is the payload we pass; the bau rejects a write shorter than NoOfElem*ElementSize() (0 = silent drop).
            knx.bau().propertyValueWrite(OT_IP_PARAMETER, 0, PropertyId, NoOfElem, 1, data, sizeof(data));
        }

        uint8_t Module::GetByteProperty(uint8_t PropertyId)
        {
            uint8_t NoOfElem = 1;
            uint8_t *data = nullptr;
            uint32_t length = 0;
            knx.bau().propertyValueRead(OT_IP_PARAMETER, 0, PropertyId, NoOfElem, 1, &data, length);
            uint8_t ret = (data != nullptr && length >= 1) ? data[0] : 0;
            delete[] data;
            return ret;
        }

        void Module::SetByteProperty(uint8_t PropertyId, uint8_t value)
        {
            uint8_t NoOfElem = 1;
            uint8_t data[1];

            data[0] = value;

            // see SetIpProperty: the payload length is an INPUT the bau bounds the write against, not an output.
            knx.bau().propertyValueWrite(OT_IP_PARAMETER, 0, PropertyId, NoOfElem, 1, data, sizeof(data));
        }

        void Module::registerCallback(NetworkChangeCallback cb)
        {
            _callback.push_back(cb);
        }

        void Module::loadCallbacks(bool state)
        {
            for (int i = 0; i < _callback.size(); i++)
            {
                _callback[i](state);
            }
        }

        void Module::showInformations()
        {
            openknx.logger.logWithPrefixAndValues("Hostname", "%s", _hostName);
            if (established())
                openknx.logger.logWithPrefixAndValues("Network", "Established (%s)", localIP().toString().c_str());
            else
                openknx.logger.logWithPrefix("Network", "Disconnected");
        }

        bool Module::processCommand(const std::string cmd, bool debugKo)
        {
            if (debugKo) return false;

            if (!debugKo && (cmd == "n" || cmd == "net"))
            {
                showNetworkInformations(true);
                return true;
            }

            else if (cmd == "net ?" || cmd == "net help")
            {
                showNetworkHelp();
                return true;
            }

#if MASK_VERSION == 0x091A || MASK_VERSION == 0x57B0
            else if (cmd == "net mc")
            {
                IPAddress address = GetIpProperty(PID_ROUTING_MULTICAST_ADDRESS);
                openknx.logger.logWithPrefixAndValues("Network", "Multicast address is set to %s", address.toString().c_str());
                return true;
            }

            else if (cmd.compare(0, 7, "net mc ") == 0)
            {
                IPAddress new_address;
                std::string new_address_str = cmd.length() >= 7 ? cmd.substr(7) : "reset";

                if (new_address_str == "reset") new_address_str = "0.0.0.0";

                new_address.fromString(new_address_str.c_str());

                setMulticastAddress(new_address, true);
                return true;
            }
#endif

            else if (cmd == "net reset")
            {
                resetNetwork();
                return true;
            }

#if defined(OPENKNX_WEBSERVER) && defined(OPENKNX_CONSOLE_DEV)
            else if (cmd == "webserver log")
            {
                bool enabled = !webserver.accessLog();
                webserver.accessLog(enabled);
                logInfoP("Webserver access log %s", enabled ? "enabled" : "disabled (4xx/5xx only)");
                return true;
            }
#endif

#ifdef KNX_IP_WIFI
            else if (cmd == "erase wifi")
            {
                saveWifiSettings("", "", true); // triggers the restart timer
                logInfoP("Wifi settings erased");
                return true;
            }
            else if (cmd.compare(0, 4, "wifi") == 0 && cmd.length() > 6)
            {
                // Command: "wifi<SEP><ssid><SEP><psk>" with <SEP>=" " compatible to previous command
                char delimiter = cmd[4];
                std::string ssid_psk = cmd.substr(5);
                size_t pos = ssid_psk.find(delimiter);

                if (pos != std::string::npos)
                {
                    std::string ssid = ssid_psk.substr(0, pos);
                    std::string psk = ssid_psk.substr(pos + 1);

                    // Grenzen wie in readWifiFile() und Network.share.xml
                    if (ssid.length() > 32 || psk.length() > 63)
                    {
                        logErrorP("SSID max. 32, PSK max. 63 Zeichen");
                        return false;
                    }

                    if (ssid.empty())
                        logInfoP("Wifi settings erased");
                    else
                        logInfoP("WLAN SSID: %s", ssid.c_str());
                    saveWifiSettings(ssid.c_str(), psk.c_str(), true); // triggers the restart timer

                    return true;
                }
                else
                {
                    logErrorP("Expected format: wifi<Sep><SSID><Sep><PSK> with <Sep> single char not in <SSID>");
                    return false;
                }
            }

// else if (cmd == "net recon" && strlen(_wifiSSID) > 0)
// {
//     logInfoP("Connecting to WiFi \"%s\"", _wifiSSID);
//     KNX_NETIF.disconnect();
//     KNX_NETIF.begin(_wifiSSID, _wifiPassphrase);
//     return true;
// }
// #else
// else if (!_useStaticIP && cmd == "net renew")
// {
//     if (!connected())
//     {
//         logErrorP("not connected");
//         return true;
//     }

//     #ifdef KNX_IP_GENERIC
//     KNX_NETIF.maintain();
//     #elif defined(ARDUINO_ARCH_ESP32)
//             // TODO
//     #else
//     dhcp_renew(KNX_NETIF.getNetIf());
//     #endif
//     return true;
// }
#endif

#ifdef OPENKNX_PING
            else if (cmd.compare(0, 5, "ping ") == 0)
            {
                ping(cmd.substr(5), [](IPAddress ip, bool reachable, uint32_t rttMs) {
                    if (reachable)
                        openknx.logger.logWithPrefixAndValues("Network", "Reply from %s in %lu ms", ip.toString().c_str(), rttMs);
                    else
                        openknx.logger.logWithPrefixAndValues("Network", "No reply from %s", ip.toString().c_str());
                });
                return true;
            }
#endif

            return false;
        }

#if MASK_VERSION == 0x091A || MASK_VERSION == 0x57B0

        void Module::setMulticastAddress(IPAddress address, bool rebootToTakeEffect)
        {
            // set default if address is 0.0.0.0
            if ((uint32_t)address == 0) address = (uint32_t)0x0C1700E0; // 224.0.23.12

            openknx.logger.logWithPrefixAndValues("Network", "Set multicast address to %s", address.toString().c_str());
            SetIpProperty(PID_ROUTING_MULTICAST_ADDRESS, address);
            knx.bau().writeMemory();

            if (rebootToTakeEffect)
                openknx.restart();
        }
#endif

        void Module::showNetworkInformations(bool console)
        {
            openknx.common.skipLooptimeWarning();
            logBegin();
            if (console)
            {
                logInfoP("Hostname: %s", _hostName);
                logInfoP("Connection: %s", established() ? "Established" : "Disconnected");
                logIndentUp();
            }

            if (established())
            {
                logInfoP(_useStaticIP ? "Using static IP" : "Using DHCP");
                logInfoP("IP-Address: %s", localIP().toString().c_str());
                logInfoP("Netmask: %s", subnetMask().toString().c_str());
                logInfoP("Gateway: %s", gatewayIP().toString().c_str());
                logInfoP("DNS: %s", nameServerIP().toString().c_str());
                // logInfoP("Mode: %s", phyMode().c_str()); currently not supported

#ifdef KNX_IP_WIFI
                std::string wifiInfo = std::string(_wifiSSID) + " (" + std::to_string(KNX_NETIF.RSSI()) + "dBm)";
                logInfoP("Wifi: %s", wifiInfo.c_str());
#endif
            }

            if (console)
            {
                if (_currentLinkState)
                {
                    // counts from the carrier edge, shown even while disconnected (the DHCP/AutoIP wait state).
                    logInfoP("Link-Uptime: %s", humanDuration(netUptimeSec()).c_str());
                    // only when the IP arrived noticeably after the link (slow DHCP / late renew).
                    const uint32_t established = netEstablishedSec();
                    if (established > 0 && (netUptimeSec() - established) >= 5)
                        logInfoP("IP established: %s", humanDuration(established).c_str());
                }
                else if (_linkEverDown)
                {
                    logInfoP("Down since: %s", humanDuration(netDowntimeSec()).c_str());
                }

                if (_reconnects > 0)
                    logInfoP("Reconnects: %u (last down %s)", (unsigned)_reconnects,
                             humanDuration(_lastDowntimeSec).c_str());

                logIndentDown();
            }
            logEnd();
        }

        // Main console help: only the entry points. Every 'net ...' subcommand lives in 'net ?' so the
        // general help stays readable on products that pull in half a dozen network features.
        void Module::showHelp()
        {
            openknx.console.printHelpLine("net, n", "Show network informations");
            openknx.console.printHelpLine("net ?", "Network commands (link, diagnostics, reset)");

#ifdef KNX_IP_WIFI

            openknx.console.printHelpLine("wifi SSID PSK", "Set SSID and PSK");
            openknx.console.printHelpLine("erase wifi", "Erase WiFi settings");

// if (strlen(_wifiSSID) > 0) openknx.console.printHelpLine("net recon", "Reconnect to network");
#else
// if (!_useStaticIP) openknx.console.printHelpLine("net renew", "Renew DHCP Address");
#endif
#if MASK_VERSION == 0x091A || MASK_VERSION == 0x57B0
            // openknx.console.printHelpLine("net mc [address|reset]", "Get/Set multicast address");
#endif
#if defined(OPENKNX_WEBSERVER) && defined(OPENKNX_CONSOLE_DEV)
            openknx.console.printHelpLine("webserver log", "Toggle webserver access log (2xx/3xx)");
#endif
#ifdef OPENKNX_PING
            openknx.console.printHelpLine("ping x.x.x.x", "Ping an IP address");
#endif
        }

        // 'net ?' -- every 'net ...' subcommand this build actually has.
        void Module::showNetworkHelp()
        {
            openknx.console.printHelpLine("net, n", "Show network informations");
            openknx.console.printHelpLine("net reset", "Reset network adapter");
#if MASK_VERSION == 0x091A || MASK_VERSION == 0x57B0
            openknx.console.printHelpLine("net mc [address|reset]", "Get/Set multicast address");
#endif
        }

        // Link status
        bool Module::connected()
        {
            if (_powerSave) return false;

#if defined(ARDUINO_ARCH_RP2040)
#if defined(KNX_IP_LAN)
            return KNX_NETIF.isLinked();
#elif defined(KNX_IP_WIFI)
            return KNX_NETIF.isConnected();
#endif
#elif defined(ARDUINO_ARCH_ESP32)
            return espConnected;
#endif
        }

        // IP Status
        bool Module::established()
        {
            if (!connected()) return false;

            return localIP() != IPAddress();
        }

        uint32_t Module::netUptimeSec() const
        {
            return _currentLinkState ? (uptime() - _linkUpSinceSec) : 0;
        }

        uint32_t Module::netDowntimeSec() const
        {
            return (!_currentLinkState && _linkEverDown) ? (uptime() - _linkDownSinceSec) : 0;
        }

        uint32_t Module::netEstablishedSec() const
        {
            return _ipShown ? (uptime() - _establishedSinceSec) : 0;
        }

        IPAddress Module::localIP()
        {
            return KNX_NETIF.localIP();
        }

        IPAddress Module::subnetMask()
        {
            return KNX_NETIF.subnetMask();
        }

        IPAddress Module::gatewayIP()
        {
            return KNX_NETIF.gatewayIP();
        }

        IPAddress Module::nameServerIP()
        {
#if defined(ARDUINO_ARCH_RP2040)
            return IPAddress(dns_getserver(0));
#elif defined(ARDUINO_ARCH_ESP32)
            return KNX_NETIF.dnsIP();
#endif
        }

        inline std::string Module::phyMode()
        {
            return "Auto";
        }

        void Module::savePower()
        {
            _powerSave = true;
#if defined(PIN_ETH_RES)
            digitalWrite(PIN_ETH_RES, LOW);
#endif
        }

        void Module::macAddress(uint8_t *addr)
        {
            KNX_NETIF.macAddress(addr);
        }

        bool Module::restorePower()
        {
// TODO check all platforms
#if defined(PIN_ETH_RES)
            digitalWrite(PIN_ETH_RES, HIGH);
#endif
            _powerSave = false;
            return true;
        }

#ifdef KNX_IP_WIFI
        // Kopiert höchstens dstSize-1 Bytes und terminiert immer
        static void copyBounded(char *dst, size_t dstSize, const char *src, size_t len)
        {
            if (dstSize == 0) return;
            if (len > dstSize - 1) len = dstSize - 1;
            memcpy(dst, src, len);
            dst[len] = '\0';
        }

        void Module::saveWifiSettings(const char *ssid, const char *passphrase, bool scheduleRebootToTakeEffect)
        {
            logDebugP("Write WiFi settings");

#ifdef ARDUINO_ARCH_RP2040
            if (!scheduleRebootToTakeEffect)
                KNX_NETIF.disconnect();

            File file = LittleFS.open("/WIFI.TXT", "w");
            file.println(ssid);
            file.println(passphrase);
            file.close();

#elif ARDUINO_ARCH_ESP32
            if (!scheduleRebootToTakeEffect)
                KNX_NETIF.disconnect(true, true);

            Preferences preferences;
            preferences.begin("WIFI", false);
            preferences.putString("SSID", ssid);
            preferences.putString("PSK", passphrase);
            preferences.end();

#endif
            if (scheduleRebootToTakeEffect)
                _restartTimer = delayTimerInit();
        }

        void Module::readWifiSettings()
        {
            logDebugP("Read WiFi settings");
            uint8_t found = 0;

#ifdef ARDUINO_ARCH_RP2040
            logInfoP("Read Wifi settings from WIFI.TXT");
            logIndentUp();

            File file = LittleFS.open("/WIFI.TXT", "r");
            if (!file)
            {
                logErrorP("File /WIFI.TXT not found!");
                logIndentDown();
                return;
            }

            int pos = 0;
            char buffer[100] = {};
            while (file.available())
            {
                uint8_t value = 0;
                file.read(&value, 1);

                if (value != '\n' && value != '\r')
                {
                    if (pos < (int)sizeof(buffer) - 1)
                        buffer[pos++] = value;
                }

                if (value == '\n' || value == '\r' || !file.available())
                {
                    if (pos > 0)
                    {
                        found++;
                        if (found == 1) copyBounded(_wifiSSID, sizeof(_wifiSSID), buffer, pos);
                        if (found == 2) copyBounded(_wifiPassphrase, sizeof(_wifiPassphrase), buffer, pos);
                        logTraceP("%i -> %i: %s", found, pos, buffer);

                        // Reset buffer
                        memset(buffer, 0x0, 100);
                        pos = 0;
                    }
                }

                // ssid and passphrase found
                if (found >= 2) break;
            }

            logIndentDown();
#elif ARDUINO_ARCH_ESP32

            Preferences preferences;
            if (preferences.begin("WIFI", true))
            {
                auto ssid = preferences.getString("SSID");
                auto passphrase = preferences.getString("PSK");
                if (ssid.length() > 0 && passphrase.length() > 0)
                {
                    copyBounded(_wifiSSID, sizeof(_wifiSSID), ssid.c_str(), ssid.length());
                    copyBounded(_wifiPassphrase, sizeof(_wifiPassphrase), passphrase.c_str(), passphrase.length());
                    found = 2;
                }
                preferences.end();
            }
#endif

            if (found > 0) logDebugP("SSID: %s", _wifiSSID);
            if (found == 2) logDebugP("Passphrase with %i chars", strlen(_wifiPassphrase));
            if (found == 1) logErrorP("No wifi passphrase found");
        }
#endif

        bool Module::processFunctionProperty(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
        {
            if (!knx.configured()) return false;
            if (objectIndex != 160) return false;
            if (propertyId != 5) return false;

            if (length < 1) return false;

            logHexTraceP(data, length);

            switch (data[0])
            {
                case 1:
                {
#ifdef KNX_IP_WIFI
                    // [1]=SSID-Länge, [2]=PSK-Länge, ab [3] SSID, Trennbyte, PSK
                    if (length < 3)
                    {
                        logErrorP("Wifi settings: payload too short (%u)", length);
                        resultData[0] = 1;
                        resultLength = 1;
                        return false;
                    }

                    const uint8_t ssidLen = data[1];
                    const uint8_t pskLen = data[2];
                    const uint16_t needed = (uint16_t)3 + ssidLen + 1 + pskLen;
                    if (ssidLen == 0 || ssidLen > 32 || pskLen > 63 || needed > length)
                    {
                        logErrorP("Wifi settings: invalid lengths (ssid=%u psk=%u payload=%u)", ssidLen, pskLen, length);
                        resultData[0] = 1;
                        resultLength = 1;
                        return false;
                    }

                    // Bus-Daten sind nicht zwingend NUL-terminiert
                    char ssid[33] = {};
                    char psk[64] = {};
                    memcpy(ssid, data + 3, ssidLen);
                    memcpy(psk, data + 3 + ssidLen + 1, pskLen);

                    logInfoP("Received wifi settings for %s", ssid);
                    saveWifiSettings(ssid, psk, true);
                    resultData[0] = 0;
#else
                    logErrorP("Unsupported: Received wifi settings");
                    resultData[0] = 2;
#endif
                    resultLength = 1;
                    return true;
                }
            }

            resultData[0] = 1;
            resultLength = 1;
            return false;
        }

        void Module::controlKnxIp(bool enable)
        {
#if MASK_VERSION == 0x091A || MASK_VERSION == 0x07B0 || MASK_VERSION == 0x57B0
            // Compared against the OBSERVED state, never against an intent flag: an intent that drifts from
            // reality never heals, while this retries exactly when the datalink is not where it should be.
            if (enable == knxIpEnabled()) return;
#endif
            // Not on 0x07B0: Bau07B0IP owns the endpoint and networkChanged() rebuilds it on the link edge; toggling
            // it here could drop live tunnels. Router masks only.
#if MASK_VERSION == 0x091A
            knx.bau().getPrimaryDataLinkLayer()->enabled(enable);
#elif MASK_VERSION == 0x57B0
            knx.bau().getDataLinkLayer()->enabled(enable);
#endif
            if (!enable) _boundIp = IPAddress(); // no address recorded
        }

#if MASK_VERSION == 0x07B0 || MASK_VERSION == 0x091A
        TPUart::DataLinkLayer &Module::tpUart()
        {
#if MASK_VERSION == 0x07B0
            return knx.bau().getDataLinkLayer()->getTPUart();
#else
            return knx.bau().getSecondaryDataLinkLayer()->getTPUart();
#endif
        }
#endif
        // True while the KNXnet/IP datalink layer is enabled ("KNX-IP is running", distinct from the
        // physical Ethernet/IP link). Counterpart to controlKnxIp().
        bool Module::knxIpEnabled()
        {
#if MASK_VERSION == 0x091A || (MASK_VERSION == 0x07B0 && defined(KNX_TUNNELING)) // primary datalink = KNXnet/IP (091A router, 07B0 interface Bau07B0IP)
            return knx.bau().getPrimaryDataLinkLayer()->enabled();
#elif MASK_VERSION == 0x57B0
            return knx.bau().getDataLinkLayer()->enabled();
#else
            return false;
#endif
        }

#ifdef OPENKNX_PING
        void Module::ping(IPAddress target, std::function<void(IPAddress, bool, uint32_t)> callback, uint32_t timeoutMs)
        {
            _pingHandler.ping(target, callback, timeoutMs);
        }

        void Module::ping(IPAddress target, std::function<void(IPAddress, bool)> callback, uint8_t retries, uint32_t timeoutMs)
        {
            _pingHandler.ping(target, callback, retries, timeoutMs);
        }

        void Module::ping(const std::string &host, std::function<void(IPAddress, bool, uint32_t)> callback, uint32_t timeoutMs)
        {
            _pingHandler.ping(host, callback, timeoutMs);
        }

        void Module::ping(const std::string &host, std::function<void(IPAddress, bool)> callback, uint8_t retries, uint32_t timeoutMs)
        {
            _pingHandler.ping(host, callback, retries, timeoutMs);
        }
#endif

    } // namespace Network
} // namespace OpenKNX

NetworkModule openknxNetwork;
#endif
