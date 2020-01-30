#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>

#include <net/if.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <linux/can.h>
#include <linux/can/raw.h>

#include <boost/asio.hpp>

using namespace boost;

class AsioSocketCanChannel
{
    public:
        AsioSocketCanChannel(asio::io_service& ios)
            : _stream { ios }
        {            
        }

        bool Open(const std::string& device)
        {
            struct sockaddr_can addr;
            struct ifreq ifr;

            int sock = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
            if (sock < 0)
            {
                perror("create socket");
                return false;
            }

            strcpy(ifr.ifr_name, device.c_str());
            if (ioctl(sock, SIOCGIFINDEX, &ifr))
            {
                perror("get interface");
                ::close(sock);
                return false;
            }

            {
                std::lock_guard guard(_subscriptionLock);
                UpdateFilters(sock);
            }

            addr.can_family = AF_CAN;
            addr.can_ifindex = ifr.ifr_ifindex;
            if (::bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) 
            {
                perror("Error in socket bind");
                ::close(sock);
                return false;
            }
            _stream.assign(sock);

            AsyncRead();

            return true;
        }

        void Close()
        {
            _stream.close();
        }

        void Write(unsigned canId, bool isExtendedId, unsigned dlc, const void* data)
        {
            struct can_frame txFrame;
            if (isExtendedId)
            {
                txFrame.can_id = canId | CAN_EFF_FLAG;
            }
            else
            {
                txFrame.can_id = canId;
            }
            txFrame.can_dlc = dlc;
            ::memcpy(txFrame.data, data, std::min(sizeof(txFrame.data), static_cast<size_t>(dlc)));
            _stream.write_some(asio::buffer(&txFrame, sizeof(can_frame)));
        }

        using FrameReceivedCallback = std::function<void(AsioSocketCanChannel& channel, unsigned canId, bool isExtendedId, unsigned dlc, const unsigned char* data)>;
        using SubscriptionId = int;

        SubscriptionId Subscribe(unsigned canId, bool isExtendedId, const FrameReceivedCallback& callback)
        {
            Subscription sub;
            sub.Id = GenerateSubscriptionId();
            sub.CanId = canId;
            if (isExtendedId)
            {
                sub.CanId = canId | CAN_EFF_FLAG;
                sub.FilterFlags = (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_EFF_MASK);
            }
            else
            {
                sub.CanId = canId;
                sub.FilterFlags = (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_SFF_MASK);
            }
            sub.Callback = callback;
            
            std::lock_guard guard(_subscriptionLock);
            _subscriptions.push_back(sub);

            if (_stream.is_open())
            {
                UpdateFilters(_stream.native_handle());
            }

            return sub.Id;
        }

        void Unsubscribe(SubscriptionId id)
        {
            std::lock_guard guard(_subscriptionLock);
            _subscriptions.erase(
                std::remove_if(
                    _subscriptions.begin(),
                    _subscriptions.end(),
                    [id](const Subscription& sub)
                    {
                        return sub.Id == id;
                    }
                ),
                _subscriptions.end()
            );
            if (_stream.is_open())
            {
                UpdateFilters(_stream.native_handle());
            }
        }

    private:
        struct Subscription 
        {
            SubscriptionId Id;
            unsigned CanId;
            unsigned FilterFlags;
            FrameReceivedCallback Callback;
        };

        std::mutex _subscriptionLock;
        std::atomic<SubscriptionId> _nextSubscriptionId { 0 };
        std::vector<Subscription> _subscriptions;

        asio::posix::stream_descriptor _stream;
        struct can_frame _recvFrame;

        SubscriptionId GenerateSubscriptionId()
        {
            return _nextSubscriptionId.fetch_add(1, std::memory_order_relaxed);
        }

        void AsyncRead()
        {
            _stream.async_read_some(
                asio::buffer(&_recvFrame, sizeof(_recvFrame)),
                std::bind(&AsioSocketCanChannel::OnFrameReceived, this, std::placeholders::_1)
            );
        }

        void OnFrameReceived(boost::system::error_code ec)
        {
            if (ec) { return; }

            auto canId = _recvFrame.can_id & CAN_EFF_MASK;

            std::lock_guard guard(_subscriptionLock);
            for (auto& sub: _subscriptions)
            {
                if ((sub.CanId & sub.FilterFlags) == (canId & sub.FilterFlags))
                {
                    sub.Callback(*this, canId, (_recvFrame.can_id & CAN_EFF_FLAG) != 0, _recvFrame.can_dlc, _recvFrame.data);
                }
            }
        
            AsyncRead();
        }

        void UpdateFilters(int sock)
        {
            std::vector<can_filter> filters;
            filters.reserve(_subscriptions.size());
            for (auto &sub: _subscriptions)
            {
                filters.push_back({ sub.CanId, sub.FilterFlags});
            }
            int err = ::setsockopt(sock, SOL_CAN_RAW, CAN_RAW_FILTER, filters.data(), sizeof(can_filter)*filters.size());
            if (err)
            {
                perror("filters");
            }
        }
};

int main()
{
    boost::asio::io_service ios;
    AsioSocketCanChannel can(ios);

    AsioSocketCanChannel::SubscriptionId sub_id;
    sub_id = can.Subscribe(0x123, false, [&sub_id](auto& sender, unsigned canId, bool isExtendedId, unsigned dlc, const unsigned char* data) {
        printf("frame received! id=0x%x\n", canId);
        sender.Unsubscribe(sub_id);
    });

    can.Subscribe(0x345, false, [&sub_id](auto& sender, unsigned canId, bool isExtendedId, unsigned dlc, const unsigned char* data) {
        printf("frame received! id=0x%x\n", canId);
    });

    can.Open("vcan0");
    can.Write(0x123, false, 5, "Hallo");

    ios.run();
    return 0;
}