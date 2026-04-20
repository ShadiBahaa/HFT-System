#pragma once

#include <cstdint>
#include <cstring>
#include <functional>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace hft::feed {

    // === Network Receiver Interface ===
    //
    // Abstract interface for different networking backends.
    // Compile-time feature flags select the backend:
    // - Default: PosixReceiver (standard sockets, portable)
    // - HFT_USE_EFVI: EfViReceiver (Solarflare ef_vi, lowest latency)
    // - HFT_USE_DPDK: DPDKReceiver (DPDK, multi-vendor NIC support)

    using PacketCallback = std::function<void(const uint8_t* data, size_t len)>;

    class NetworkReceiver {
    public:
        virtual ~NetworkReceiver() = default;
        virtual bool init(const char* interface, uint16_t port) = 0;
        virtual int poll(uint8_t* buf, size_t buf_size) = 0;
        virtual void close() = 0;
        [[nodiscard]] virtual bool is_open() const noexcept = 0;
    };

    // === PosixReceiver — Standard UDP Socket (Portable Fallback) ===

    class PosixReceiver : public NetworkReceiver {
#ifdef _WIN32
        using SocketType = SOCKET;
        static constexpr SocketType INVALID_SOCK = INVALID_SOCKET;
#else
        using SocketType = int;
        static constexpr SocketType INVALID_SOCK = -1;
#endif
        SocketType fd_{INVALID_SOCK};
        bool initialized_{false};

    public:
        PosixReceiver() = default;

        ~PosixReceiver() override {
            close();
        }

        bool init(const char* multicast_group, uint16_t port) override {
#ifdef _WIN32
            WSADATA wsa;
            WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
            fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (fd_ == INVALID_SOCK) return false;

            // Allow address reuse
            int reuse = 1;
#ifdef _WIN32
            setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
            setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

            // Bind to port
            struct sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = htonl(INADDR_ANY);

            if (bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
                close();
                return false;
            }

            // Join multicast group if specified
            if (multicast_group && multicast_group[0] != '\0') {
                struct ip_mreq mreq{};
                mreq.imr_multiaddr.s_addr = inet_addr(multicast_group);
                mreq.imr_interface.s_addr = htonl(INADDR_ANY);
#ifdef _WIN32
                setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                           reinterpret_cast<const char*>(&mreq), sizeof(mreq));
#else
                setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
#endif
            }

            // Set non-blocking for poll-style receive
#ifdef _WIN32
            unsigned long nonblocking = 1;
            ioctlsocket(fd_, FIONBIO, &nonblocking);
#else
            int flags = fcntl(fd_, F_GETFL, 0);
            fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
#endif
            initialized_ = true;
            return true;
        }

        int poll(uint8_t* buf, size_t buf_size) override {
            if (fd_ == INVALID_SOCK) return -1;
#ifdef _WIN32
            int n = recv(fd_, reinterpret_cast<char*>(buf), static_cast<int>(buf_size), 0);
#else
            ssize_t n = recv(fd_, buf, buf_size, 0);
#endif
            return static_cast<int>(n);
        }

        void close() override {
            if (fd_ != INVALID_SOCK) {
#ifdef _WIN32
                closesocket(fd_);
                WSACleanup();
#else
                ::close(fd_);
#endif
                fd_ = INVALID_SOCK;
            }
            initialized_ = false;
        }

        [[nodiscard]] bool is_open() const noexcept override {
            return fd_ != INVALID_SOCK;
        }
    };

    // === ef_vi Receiver Stub (Solarflare — Linux Only) ===

#ifdef HFT_USE_EFVI
    class EfViReceiver : public NetworkReceiver {
        // ef_vi requires: etherfabric/vi.h, etherfabric/pd.h, etherfabric/memreg.h
        // Only compiles with Solarflare OpenOnload SDK installed

        static constexpr size_t RX_BUF_SIZE = 2048;
        static constexpr size_t NUM_BUFS = 1024;

        // ef_vi handles would go here
        bool initialized_{false};

    public:
        bool init(const char* interface, uint16_t port) override {
            // ef_driver_open, ef_pd_alloc_by_name, ef_vi_alloc_from_pd
            // ef_memreg_alloc, ef_filter_spec_init, ef_vi_filter_add
            (void)interface;
            (void)port;
            initialized_ = true;
            return true;
        }

        int poll(uint8_t* buf, size_t buf_size) override {
            // ef_eventq_poll → get_rx_buf → copy or return pointer
            (void)buf;
            (void)buf_size;
            return 0;
        }

        void close() override { initialized_ = false; }
        [[nodiscard]] bool is_open() const noexcept override { return initialized_; }
    };
#endif

    // === DPDK Receiver Stub (Multi-Vendor — Linux Only) ===

#ifdef HFT_USE_DPDK
    class DPDKReceiver : public NetworkReceiver {
        // DPDK requires: rte_ethdev.h, rte_mbuf.h
        // Only compiles with DPDK SDK installed + hugepages configured

        uint16_t port_id_{0};
        uint16_t queue_id_{0};
        bool initialized_{false};

    public:
        bool init(const char* interface, uint16_t port) override {
            // rte_eal_init, rte_eth_dev_configure, rte_eth_rx_queue_setup
            (void)interface;
            (void)port;
            initialized_ = true;
            return true;
        }

        int poll(uint8_t* buf, size_t buf_size) override {
            // rte_eth_rx_burst → rte_pktmbuf_mtod → copy
            (void)buf;
            (void)buf_size;
            return 0;
        }

        void close() override { initialized_ = false; }
        [[nodiscard]] bool is_open() const noexcept override { return initialized_; }
    };
#endif

    // Default receiver type based on compile-time flags
#if defined(HFT_USE_EFVI)
    using DefaultReceiver = EfViReceiver;
#elif defined(HFT_USE_DPDK)
    using DefaultReceiver = DPDKReceiver;
#else
    using DefaultReceiver = PosixReceiver;
#endif

} // namespace hft::feed
