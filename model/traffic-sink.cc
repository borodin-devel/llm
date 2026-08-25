#include "traffic-sink.h"

#include "ns3/inet-socket-address.h"
#include "ns3/inet6-socket-address.h"
#include "ns3/log.h"
#include "ns3/packet.h"
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

    for (auto& socket : m_acceptedSockets)
    {
        if (socket)
        {
            socket->Close();
        }
    }

    m_acceptedSockets.clear();

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
        EmitReceive(receivedBytes, from);
    }
}

void
TrafficSink::EmitReceive(uint64_t receivedBytes, const Address& from)
{
    m_rxTraceCustom(receivedBytes, from);
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

} // namespace ns3
