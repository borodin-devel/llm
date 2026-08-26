#include "saturated-tcp-sender.h"

#include "ns3/inet-socket-address.h"
#include "ns3/node.h"
#include "ns3/packet.h"
#include "ns3/socket.h"
#include "ns3/tcp-socket-factory.h"
#include "ns3/uinteger.h"

// The private scenario header shares this core header's basename.
// clang-format off
#include "ns3/log.h"
// clang-format on

#include <utility>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("SaturatedTcpSender");

NS_OBJECT_ENSURE_REGISTERED(SaturatedTcpSender);

TypeId
SaturatedTcpSender::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::SaturatedTcpSender")
            .SetParent<SourceApplication>()
            .SetGroupName("Llm")
            .AddConstructor<SaturatedTcpSender>()
            .AddAttribute("SendSize",
                          "Application bytes offered in each socket send operation.",
                          UintegerValue(512),
                          MakeUintegerAccessor(&SaturatedTcpSender::m_sendSize),
                          MakeUintegerChecker<uint32_t>(1));
    return tid;
}

SaturatedTcpSender::SaturatedTcpSender()
    : SourceApplication(false)
{
}

SaturatedTcpSender::~SaturatedTcpSender() = default;

void
SaturatedTcpSender::SetReadyCallback(Callback<void> callback)
{
    if (callback.IsNull())
    {
        m_readyCallback = Callback<void>();
        return;
    }
    NS_ABORT_MSG_IF(m_applicationStarted,
                    "cannot replace saturated TCP readiness callback after application start");
    m_readyCallback = std::move(callback);
}

void
SaturatedTcpSender::StartTraffic()
{
    NS_ABORT_MSG_IF(!m_ready, "cannot start saturated TCP traffic before connection readiness");
    NS_ABORT_MSG_IF(!m_running, "cannot start saturated TCP traffic after sender stop");
    NS_ABORT_MSG_IF(m_trafficStarted, "saturated TCP traffic started more than once");
    m_trafficStarted = true;
    SendData();
}

void
SaturatedTcpSender::StopTraffic()
{
    if (!m_running)
    {
        return;
    }
    m_running = false;
    CancelEvents();
    CloseSocket();
}

void
SaturatedTcpSender::DoDispose()
{
    StopTraffic();
    SourceApplication::DoDispose();
}

void
SaturatedTcpSender::StartApplication()
{
    NS_ABORT_MSG_IF(m_applicationStarted,
                    "saturated TCP sender application started more than once");
    m_applicationStarted = true;
    m_running = true;
    DoStartApplication();
}

void
SaturatedTcpSender::StopApplication()
{
    DoStopApplication();
}

void
SaturatedTcpSender::DoStartApplication()
{
    NS_ABORT_MSG_IF(m_peer.IsInvalid(), "saturated TCP sender remote address is not configured");
    NS_ABORT_MSG_IF(!InetSocketAddress::IsMatchingType(m_peer),
                    "saturated TCP sender requires an IPv4 TCP endpoint");
    NS_ABORT_MSG_IF(m_readyCallback.IsNull(),
                    "saturated TCP sender readiness callback is not configured");
    if (!m_local.IsInvalid())
    {
        NS_ABORT_MSG_IF(!InetSocketAddress::IsMatchingType(m_local),
                        "saturated TCP sender local endpoint is not IPv4");
    }

    m_socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
    NS_ABORT_MSG_IF(!m_socket, "saturated TCP sender could not create a TCP socket");
    NS_ABORT_MSG_IF(m_socket->GetSocketType() != Socket::NS3_SOCK_STREAM,
                    "saturated TCP sender requires a stream socket");
    m_socket->SetConnectCallback(MakeCallback(&SaturatedTcpSender::ConnectionSucceeded, this),
                                 MakeCallback(&SaturatedTcpSender::ConnectionFailed, this));
    m_socket->SetSendCallback(MakeCallback(&SaturatedTcpSender::DataSend, this));
    m_socket->ShutdownRecv();

    const int bindResult = m_local.IsInvalid() ? m_socket->Bind() : m_socket->Bind(m_local);
    if (bindResult == -1)
    {
        NS_FATAL_ERROR("saturated TCP sender failed to bind local endpoint: socket error "
                       << static_cast<int>(m_socket->GetErrno()));
    }
    m_socket->SetIpTos(m_tos);
    if (m_socket->Connect(m_peer) == -1)
    {
        NS_FATAL_ERROR("saturated TCP sender connection failed immediately for remote "
                       << m_peer << ": socket error " << static_cast<int>(m_socket->GetErrno()));
    }
}

void
SaturatedTcpSender::DoStopApplication()
{
    StopTraffic();
}

void
SaturatedTcpSender::CancelEvents()
{
    m_unsentPacket = nullptr;
    m_readyCallback = Callback<void>();
    if (m_socket)
    {
        m_socket->SetConnectCallback(MakeNullCallback<void, Ptr<Socket>>(),
                                     MakeNullCallback<void, Ptr<Socket>>());
        m_socket->SetRecvCallback(MakeNullCallback<void, Ptr<Socket>>());
        m_socket->SetSendCallback(MakeNullCallback<void, Ptr<Socket>, uint32_t>());
    }
}

void
SaturatedTcpSender::ConnectionSucceeded(Ptr<Socket> socket)
{
    NS_ABORT_MSG_IF(!m_running, "stopped saturated TCP sender received connection success");
    NS_ABORT_MSG_IF(socket != m_socket, "saturated TCP sender received success for another socket");
    NS_ABORT_MSG_IF(m_ready, "saturated TCP sender reported connection success more than once");
    m_connected = true;
    m_ready = true;

    Address local;
    socket->GetSockName(local);
    if (!m_readyCallback.IsNull())
    {
        m_readyCallback();
    }
    m_connectionSuccess(socket, local, m_peer);
}

void
SaturatedTcpSender::ConnectionFailed(Ptr<Socket> socket)
{
    m_connected = false;
    Address local;
    socket->GetSockName(local);
    m_connectionFailure(socket, local, m_peer);
    NS_FATAL_ERROR("saturated TCP sender connection failed from "
                   << local << " to " << m_peer << ": socket error "
                   << static_cast<int>(socket->GetErrno()));
}

void
SaturatedTcpSender::SendData()
{
    while (m_running && m_connected && m_trafficStarted)
    {
        Ptr<Packet> packet = m_unsentPacket ? m_unsentPacket : Create<Packet>(m_sendSize);
        const uint32_t offeredBytes = packet->GetSize();
        const int sentBytes = m_socket->Send(packet);
        if (sentBytes == static_cast<int>(offeredBytes))
        {
            m_unsentPacket = nullptr;
            m_txTrace(packet);
        }
        else if (sentBytes == -1)
        {
            m_unsentPacket = packet;
            return;
        }
        else if (sentBytes > 0 && static_cast<uint32_t>(sentBytes) < offeredBytes)
        {
            m_txTrace(packet->CreateFragment(0, static_cast<uint32_t>(sentBytes)));
            m_unsentPacket =
                packet->CreateFragment(static_cast<uint32_t>(sentBytes), offeredBytes - sentBytes);
            return;
        }
        else
        {
            NS_FATAL_ERROR("saturated TCP socket returned invalid send result "
                           << sentBytes << " for " << offeredBytes << " bytes");
        }
    }
}

void
SaturatedTcpSender::DataSend(Ptr<Socket> socket, uint32_t availableBytes)
{
    static_cast<void>(availableBytes);
    if (m_running && m_connected && m_trafficStarted && socket == m_socket)
    {
        SendData();
    }
}

} // namespace ns3
