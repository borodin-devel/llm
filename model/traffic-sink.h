#ifndef TRAFFIC_SINK_H
#define TRAFFIC_SINK_H

#include "ns3/sink-application.h"
#include "ns3/traced-callback.h"

#include <cstdint>
#include <vector>

namespace ns3
{

class Socket;
class Packet;

/**
 * @ingroup applications
 *
 * TCP traffic sink that records received-packet timing.
 */
class TrafficSink : public SinkApplication
{
  public:
    /**
     * Get the registered type identifier.
     *
     * @return Application TypeId.
     */
    static TypeId GetTypeId();

    TrafficSink();
    ~TrafficSink() override;

  private:
    void DoDispose() override;
    void DoStartApplication() override;
    void DoStopApplication() override;

    /**
     * Consume received TCP payload.
     *
     * @param socket Socket with available data.
     */
    void HandleRead(Ptr<Socket> socket);

    /**
     * Configure one accepted TCP connection.
     *
     * @param socket Accepted socket.
     * @param from Remote peer address.
     */
    void HandleAccept(Ptr<Socket> socket, const Address& from);

    /** @param socket Socket closed by its peer. */
    void HandlePeerClose(Ptr<Socket> socket);

    /** @param socket Socket reporting an error. */
    void HandleError(Ptr<Socket> socket);

    std::vector<Ptr<Socket>> m_acceptedSockets;        ///< Accepted TCP sockets.
    TracedCallback<uint64_t, Address> m_rxTraceCustom; ///< Received-payload trace.
};

} // namespace ns3

#endif // TRAFFIC_SINK_H
