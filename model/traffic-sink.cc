#include "traffic-sink.h"

#include "ns3/inet-socket-address.h"
#include "ns3/inet6-socket-address.h"
#include "ns3/log.h"
#include "ns3/nstime.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/socket.h"
#include "ns3/tcp-socket-factory.h"
#include "ns3/trace-source-accessor.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TrafficSink");

NS_OBJECT_ENSURE_REGISTERED(TrafficSink);

TypeId
TrafficSink::GetTypeId()
{
    static TypeId tid = TypeId("ns3::TrafficSink")
                            .SetParent<SinkApplication>()
                            .SetGroupName("Applications")
                            .AddConstructor<TrafficSink>()
                            .AddAttribute("Protocol",
                                          "The type id of the socket to use for this application.",
                                          TypeIdValue(TcpSocketFactory::GetTypeId()),
                                          MakeTypeIdAccessor(&TrafficSink::m_protocolTid),
                                          MakeTypeIdChecker())
                            .AddTraceSource("RxCustom",
                                            "A packet has been received.",
                                            MakeTraceSourceAccessor(&TrafficSink::m_rxTraceCustom),
                                            "ns3::TrafficSink::RxCallback");
    return tid;
}

TrafficSink::TrafficSink()
    : m_lastPacketTime(Seconds(0))
{
    NS_LOG_FUNCTION(this);
}

TrafficSink::~TrafficSink()
{
    NS_LOG_FUNCTION(this);
}

void
TrafficSink::DoDispose()
{
    NS_LOG_FUNCTION(this);
    SinkApplication::DoDispose();
}

void
TrafficSink::DoStartApplication()
{
    NS_LOG_FUNCTION(this);

    Address localAddress;
    m_socket->GetSockName(localAddress);
    const InetSocketAddress localSocketAddress = InetSocketAddress::ConvertFrom(localAddress);
    const Ipv4Address localIp = localSocketAddress.GetIpv4();

    NS_LOG_INFO("TrafficSink::DoStartApplication " << localIp);

    // Socket is already created by SinkApplication::StartApplication().
    // Bind and listen.
    if (m_local.IsInvalid())
    {
        m_local = InetSocketAddress(Ipv4Address::GetAny(), m_port);
    }

    if (InetSocketAddress::IsMatchingType(m_local))
    {
        const InetSocketAddress bindAddress = InetSocketAddress::ConvertFrom(m_local);
        if (m_socket->Bind(bindAddress) == -1)
        {
            NS_FATAL_ERROR("Failed to bind TCP sink to " << bindAddress);
        }
        m_socket->Listen();
        m_socket->SetAcceptCallback(MakeNullCallback<bool, Ptr<Socket>, const Address&>(),
                                    MakeCallback(&TrafficSink::HandleAccept, this));
        m_socket->SetCloseCallbacks(MakeCallback(&TrafficSink::HandlePeerClose, this),
                                    MakeCallback(&TrafficSink::HandleError, this));
    }
    else if (Inet6SocketAddress::IsMatchingType(m_local))
    {
        const Inet6SocketAddress bindAddress = Inet6SocketAddress::ConvertFrom(m_local);
        if (m_socket6 && m_socket6->Bind(bindAddress) == -1)
        {
            NS_FATAL_ERROR("Failed to bind TCP sink6 to " << bindAddress);
        }
        if (m_socket6)
        {
            m_socket6->Listen();
            m_socket6->SetAcceptCallback(MakeNullCallback<bool, Ptr<Socket>, const Address&>(),
                                         MakeCallback(&TrafficSink::HandleAccept, this));
            m_socket6->SetCloseCallbacks(MakeCallback(&TrafficSink::HandlePeerClose, this),
                                         MakeCallback(&TrafficSink::HandleError, this));
        }
    }
    else
    {
        if (m_socket->Bind() == -1)
        {
            NS_FATAL_ERROR("Failed to bind TCP sink");
        }
        m_socket->Listen();
        m_socket->SetAcceptCallback(MakeNullCallback<bool, Ptr<Socket>, const Address&>(),
                                    MakeCallback(&TrafficSink::HandleAccept, this));
        m_socket->SetCloseCallbacks(MakeCallback(&TrafficSink::HandlePeerClose, this),
                                    MakeCallback(&TrafficSink::HandleError, this));
    }

    NS_LOG_INFO("TrafficSink listening on port " << m_port);
}

void
TrafficSink::DoStopApplication()
{
    NS_LOG_FUNCTION(this);

    const auto layer3PacketCount = static_cast<uint32_t>(m_iatSamplesUs.size());

    // A TrafficSink is installed on every STA, but not every STA is
    // necessarily used by the distribution algorithm.
    //
    // In particular, low-contention placement may intentionally put several
    // agents on the same STA and leave other STAs completely unused.
    //
    // An unused STA never accepts a TCP connection, therefore
    // m_acceptedSockets can legitimately be empty here.
    if (!m_acceptedSockets.empty() && m_acceptedSockets.front())
    {
        Address address;
        m_acceptedSockets.front()->GetSockName(address);

        if (InetSocketAddress::IsMatchingType(address))
        {
            const InetSocketAddress inetLocal = InetSocketAddress::ConvertFrom(address);

            NS_LOG_WARN("[Received Stats] " << inetLocal.GetIpv4() << ":" << inetLocal.GetPort()
                                            << " L3_packets=" << layer3PacketCount);
        }
        else
        {
            NS_LOG_WARN("[Received Stats] " << "L3_packets=" << layer3PacketCount);
        }
    }
    else
    {
        // This is a valid state, not an error:
        // no sender was mapped to this sink/STA.
        NS_LOG_WARN("[Received Stats] " << "no accepted TCP connections"
                                        << ", sinkPort=" << m_port
                                        << ", L3_packets=" << layer3PacketCount);
    }

    // Close every connection that was actually accepted.
    for (auto& socket : m_acceptedSockets)
    {
        if (socket)
        {
            socket->Close();
        }
    }

    m_acceptedSockets.clear();

    // Let SinkApplication close its listening sockets as well.
    CloseAllSockets();
}

void
TrafficSink::HandleAccept(Ptr<Socket> socket, const Address& from)
{
    NS_LOG_FUNCTION(this << socket << from);

    socket->SetRecvCallback(MakeCallback(&TrafficSink::HandleRead, this));
    m_acceptedSockets.push_back(socket);

    Address localAddress;
    socket->GetSockName(localAddress);
    const InetSocketAddress localSocketAddress = InetSocketAddress::ConvertFrom(localAddress);
    const Ipv4Address localIp = localSocketAddress.GetIpv4();

    const Ipv4Address remoteIp = InetSocketAddress::ConvertFrom(from).GetIpv4();

    NS_LOG_INFO("Accepted " << localIp << " TCP connection from " << remoteIp);
}

void
TrafficSink::HandleRead(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);

    Address from;
    while (auto packet = socket->RecvFrom(from))
    {
        if (packet->GetSize() == 0)
        {
            break;
        }

        const uint32_t receivedBytes = packet->GetSize();
        m_rxTraceCustom(receivedBytes, from);

        RecordInterArrivalTime(Simulator::Now());
    }
}

void
TrafficSink::HandlePeerClose(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    NS_LOG_INFO("Peer closed TCP connection");
}

void
TrafficSink::HandleError(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    NS_LOG_ERROR("TCP error on socket");
}

void
TrafficSink::RecordInterArrivalTime(Time receiveTime)
{
    if (m_lastPacketTime != Seconds(0))
    {
        const int64_t interArrivalUs = (receiveTime - m_lastPacketTime).GetMicroSeconds();
        m_iatSamplesUs.push_back(static_cast<double>(interArrivalUs));
        NS_LOG_DEBUG("[IAT] Inter-arrival time: " << interArrivalUs << " us");
    }
    m_lastPacketTime = receiveTime;
}

} // namespace ns3
