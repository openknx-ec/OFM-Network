#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#pragma once
#include "OpenKNX.h"
#include "OpenKNX/Led/FunctionManager.h"
#include "OpenKNX/Network/Ping/Handler.h"
#include "OpenKNX/Network/TelegramJson.h" // defines OPENKNX_TELEGRAMJSON
// buildTelegramJson() converts the decoded value from Latin-15 to UTF-8.
#if defined(OPENKNX_WEBSERVER) || defined(OPENKNX_TELEGRAMJSON)
    #ifndef OPENKNX_CHARSET
        #define OPENKNX_CHARSET
    #endif
#endif
#ifdef OPENKNX_WEBSERVER
#include "OpenKNX/Network/Webserver/Webserver.h"
#endif
#ifdef OPENKNX_WEBCONSOLE
#include "OpenKNX/Network/Webserver/Webconsole.h"
#endif
#ifdef OPENKNX_WEBFS
#include "OpenKNX/Network/Webserver/FileManager.h"
#endif
#ifdef OPENKNX_WEBMONITOR
#include "OpenKNX/Network/Webserver/GroupMonitor.h"
#endif
#ifdef OPENKNX_TELEGRAMJSON
#include "OpenKNX/Network/GATable.h" // DPT lookup for the decoded value
#endif
#ifdef OPENKNX_MQTT
#include "OpenKNX/Network/MQTT/Module.h"
#endif
#ifdef OPENKNX_WEBCLIENT
#include "OpenKNX/Network/Webclient/Handler.h"
#endif
#include "strings.h"
#include <functional>

#if defined(ARDUINO_ARCH_ESP32)
#include <ESPmDNS.h>
#include <Preferences.h>
#include <atomic>
#include <vector>
#if defined(KNX_IP_LAN)
#include <ETH.h>
#define KNX_NETIF ETH
#elif defined(KNX_IP_WIFI)
#include <WiFi.h>
#endif
#elif defined(ARDUINO_ARCH_RP2040)
#ifndef OPENKNX_USB_EXCHANGE_IGNORE
#define HAS_USB
#endif

#ifndef OPENKNX_NET_SPI_SPEED
#define OPENKNX_NET_SPI_SPEED 28000000
#endif

#if defined(KNX_IP_LAN)
#include <W5500lwIP.h>
#include <lwip/dhcp.h>
#elif defined(KNX_IP_WIFI)
#include "LittleFS.h"
#include <WiFi.h>
#endif
#else
#pragma warn "Unsupported platform"
#endif

#ifdef HAS_USB
#include "UsbExchangeModule.h"
#endif

namespace OpenKNX::Led
{
    extern uint32_t g_ipLedActivity;
}

#if MASK_VERSION == 0x07B0 || MASK_VERSION == 0x091A
namespace TPUart
{
    class DataLinkLayer;
}
#endif

typedef std::function<void(bool)> NetworkChangeCallback;

namespace OpenKNX
{
    namespace Network
    {

        class Module : public OpenKNX::Module
        {
          public:
            const std::string name() override;
            const std::string version() override;
            void init() override;
            void loop(bool configured) override;
            void setup(bool configured) override;
            void resetNetwork();

            void savePower() override;
            bool restorePower() override;
            void showInformations() override;
            bool processCommand(const std::string cmd, bool debugKo);
            void showHelp() override;
            void showNetworkHelp(); // 'net ?': every 'net ...' subcommand (the main help only points here)
            void showNetworkInformations(bool console = false);

            bool processFunctionProperty(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength) override;

#ifdef KNX_IP_WIFI
            void saveWifiSettings(const char *ssid, const char *passphrase, bool scheduleRebootToTakeEffect);
            void readWifiSettings();
#endif

#ifdef HAS_USB
            void fillNetworkFile(UsbExchangeFile *file);
#ifdef KNX_IP_WIFI
            void fillWifiFile(UsbExchangeFile *file);
            bool readWifiFile(UsbExchangeFile *file);
#endif
#endif

            void registerCallback(NetworkChangeCallback callback);

            bool connected();
            bool established();
            IPAddress localIP();
            IPAddress subnetMask();
            IPAddress gatewayIP();
            IPAddress nameServerIP();
            std::string phyMode();
            void macAddress(uint8_t *addr);
            const char *hostName() const { return _hostName; }

            // Connection age + reconnect history, in uptime() seconds (monotonic; millis() wraps at 49.7 d).
            uint32_t netUptimeSec() const;      // since the link came up; 0 = link down
            uint32_t netDowntimeSec() const;    // since the link went down; 0 = link up or never down
            uint32_t netEstablishedSec() const; // since IP was established; 0 = not established
            uint32_t netLastDowntimeSec() const { return _lastDowntimeSec; }
            uint16_t netReconnects() const { return _reconnects; }
#if MASK_VERSION == 0x091A || MASK_VERSION == 0x57B0
            void setMulticastAddress(IPAddress address, bool rebootToTakeEffect);
#endif
#if MASK_VERSION == 0x07B0 || MASK_VERSION == 0x091A
            // Abstracts the BAU accessor difference: a plain TP device (0x07B0)
            // reaches its TP-UART via getDataLinkLayer(); a TP+IP router (0x091A)
            // via getSecondaryDataLinkLayer() -- getDataLinkLayer() there is the IP side.
            TPUart::DataLinkLayer &tpUart();
#endif

#ifdef OPENKNX_PING
            void ping(IPAddress target, std::function<void(IPAddress, bool, uint32_t)> callback = nullptr,
                      uint32_t timeoutMs = OPENKNX_PING_TIMEOUT);
            void ping(IPAddress target, std::function<void(IPAddress, bool)> callback,
                      uint8_t retries = 2, uint32_t timeoutMs = OPENKNX_PING_TIMEOUT);
            void ping(const std::string &host, std::function<void(IPAddress, bool, uint32_t)> callback = nullptr,
                      uint32_t timeoutMs = OPENKNX_PING_TIMEOUT);
            void ping(const std::string &host, std::function<void(IPAddress, bool)> callback,
                      uint8_t retries = 2, uint32_t timeoutMs = OPENKNX_PING_TIMEOUT);
#endif

#ifdef OPENKNX_WEBSERVER
            OpenKNX::Network::Webserver webserver;
#endif
#ifdef OPENKNX_WEBCONSOLE
            OpenKNX::Network::Webconsole webconsole;
#endif
#ifdef OPENKNX_WEBFS
            OpenKNX::Network::FileManager filemanager;
#endif
#if defined(OPENKNX_TELEGRAMJSON) && defined(OPENKNX_WEBMONITOR)
            OpenKNX::Network::GroupMonitor groupmonitor;
#endif
#ifdef OPENKNX_TELEGRAMJSON
            OpenKNX::Network::GATable gatable;
#endif
#ifdef OPENKNX_MQTT
            OpenKNX::Network::MQTT::Module mqtt;
#endif
#ifdef OPENKNX_WEBCLIENT
            OpenKNX::Network::Webclient::Handler webclient;
#endif

#ifdef ARDUINO_ARCH_ESP32
            void esp32NetworkEvent(arduino_event_id_t event);
#endif

          private:
            IPAddress GetIpProperty(uint8_t PropertyId);
            void SetIpProperty(uint8_t PropertyId, IPAddress IPAddress);
            uint8_t GetByteProperty(uint8_t PropertyId);
            void SetByteProperty(uint8_t PropertyId, uint8_t value);

#if defined(OPENKNX_TELEGRAMJSON) && defined(OPENKNX_MQTT)
            // ParamNET_MQTTTPRawData: every received TP telegram as JSON on
            // <prefix>/knx/tp/telegramm. Lives here, not in MQTT::Module -- that one is
            // the MQTT API and has no business knowing about KNX frames.
            void publishTelegram(TPUart::Frame &frame); // TP-UART callback
            std::string _telegramTopic;                 // absolute, built once in setup()
            OpenKNX::Format::JSON::Writer _telegramJson; // reset() keeps its capacity
#endif

            bool _powerSave = false;
            bool _ipShown = false;
            uint32_t _ipStableSince = 0; // link-established timestamp; debounces the "established" + IP-info output
            bool _useStaticIP = false;
            bool _useMDNS = false;
            bool _otaAllowed = false;
            bool _otaHandle = false;
            uint8_t _ipLedState = 0;
            OpenKNX::Led::FunctionGroup *_ipLedFunc = nullptr;

#ifdef ARDUINO_ARCH_ESP32
            const uint16_t _otaPort = 3232;
            const char *_otaPortString = "3232";
#else
            const uint16_t _otaPort = 2040;
            const char *_otaPortString = "2040";
#endif
            uint8_t _otaProgress = 0;
            bool _otaActive = false;  // true while an OTA update runs (onStart..onEnd/onError) -> display overlay
            IPAddress _staticLocalIP;
            IPAddress _staticSubnetMask;
            IPAddress _staticGatewayIP;
            IPAddress _staticNameServerIP;
            IPAddress _boundIp; // ESP32: address of the last join by REBIND or the link check; a GOT_IP for it skips the rebind while the endpoint is up

#ifdef ARDUINO_ARCH_ESP32
            bool espConnected = false;

            // KNX-IP requests from the network event task, applied by loop(): IpDataLinkLayer is not thread-safe.
            static constexpr uint32_t KNXIP_REQ_DOWN = 0x01;   // disable (ETH START, LOST_IP, DISCONNECTED, STOP)
            static constexpr uint32_t KNXIP_REQ_REBIND = 0x02; // GOT_IP: re-bind on an address change or a closed endpoint
            std::atomic<uint32_t> _knxIpRequest{0};
            void requestKnxIp(bool rebind);
            void applyKnxIpRequest();
#endif

            char _hostName[25] = {};

            char *_mDNSHttpServiceName = nullptr;
            char *_mDNSDeviceServiceName = nullptr;
            char *_mDNSDeviceServiceNameTXT = nullptr;
            bool _currentLinkState = false;
            bool linkCarrier(); // carrier for the link edge logic, debounced where the read can glitch
#if defined(ARDUINO_ARCH_RP2040) && defined(OPENKNX_ETH_W5500) && defined(KNX_IP_LAN)
            // The carrier comes from a PHYCFGR read over the same SPI bus the driver uses. A single garbled
            // read was taken at face value and tore the lwIP link down, which drops the DHCP binding -- the
            // device then sat without an address until a reboot while the PHY never lost carrier.
            // checkEthHealth() debounces VERSIONR for exactly this reason; this path did not.
            bool _carrierStable = false; // accepted carrier state
            uint8_t _carrierSamples = 0; // consecutive reads disagreeing with _carrierStable
            static constexpr uint8_t LINK_DEBOUNCE_SAMPLES = 3; // 3 x 500ms tick -> 1.5s to accept a change
#endif
            uint32_t _lastLinkCheck = false;
            uint32_t _ipFaultSince = 0; // millis() the IP network first looked unreachable; 0 = it does not
            bool _ipFaultReported = false; // last value written to the IP-fault bit; edge-only, keeps the 2 Hz tick allocation-free
            uint32_t _restartTimer = 0;

            // uptime() seconds; up/down comes from _currentLinkState/_ipShown, not a zero stamp.
            uint32_t _linkUpSinceSec = 0;      // last link-up edge
            uint32_t _linkDownSinceSec = 0;    // last link-down edge
            uint32_t _establishedSinceSec = 0; // moment "Network established" was reached
            uint32_t _lastDowntimeSec = 0;     // length of the last outage
            uint32_t _lastLinkUptimeSec = 0;   // how long the link had been up when it last went down
            uint16_t _reconnects = 0;          // link-up edges after the first one
            bool _linkEverDown = false;        // the first link-up is a connect, not a reconnect
#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN)
            // Feed PHY link edges to lwIP; arduino-pico's W5500 driver doesn't, leaving DHCP on a stale lease.
            void setLwipLinkState(bool up);
#endif
            // Link-loss bookkeeping, shared by the normal down edge and the W5500 recovery.
            void onLinkLost();
            void initPhy();
            void initIp();
            void loadSettings();
            void checkLinkStatus();
            void checkIpStatus();
            void loadCallbacks(bool state);
            void handleMDNS();
            void handleOTA();
            void controlKnxIp(bool state);
            void checkKnxIpDeviceState(bool established); // feeds the IP-fault bit of PID_KNXNETIP_DEVICE_STATE
            bool knxIpEnabled();     // is the KNXnet/IP datalink layer currently enabled?

#ifdef OPENKNX_PING
            OpenKNX::Network::Ping::Handler _pingHandler;
#endif

#ifdef KNX_IP_WIFI
            char _wifiSSID[33] = {};
            char _wifiPassphrase[64] = {};
#endif

            std::vector<NetworkChangeCallback> _callback;
        };

    } // namespace Network
} // namespace OpenKNX

using NetworkModule = OpenKNX::Network::Module;
extern NetworkModule openknxNetwork;
#endif
