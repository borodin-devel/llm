#ifndef SATURATED_TCP_SENDER_H
#define SATURATED_TCP_SENDER_H

#include "ns3/callback.h"
#include "ns3/ptr.h"
#include "ns3/source-application.h"

#include <cstdint>

namespace ns3
{

class Packet;
class Socket;

/**
 * Readiness-gated unlimited TCP source for the saturated benchmark.
 *
 * The application establishes TCP at application start and reports readiness
 * without sending payload. StartTraffic() opens the payload gate and keeps the
 * socket send buffer full until the application stops.
 */
class SaturatedTcpSender : public SourceApplication
{
  public:
    /**
     * Get the object TypeId.
     *
     * @return The object TypeId.
     */
    static TypeId GetTypeId();

    /** Construct an inactive saturated TCP sender. */
    SaturatedTcpSender();
    /** Destroy the saturated TCP sender. */
    ~SaturatedTcpSender() override;

    /**
     * Set the callback invoked exactly once after TCP connection success.
     *
     * Passing a null callback safely clears a previous registration at any
     * lifecycle state. A non-null callback must be set before application start.
     *
     * @param callback Readiness callback, or null to clear it.
     */
    void SetReadyCallback(Callback<void> callback);

    /** Open the payload gate and begin unlimited send-buffer filling. */
    void StartTraffic();

    /** Stop payload production and close the TCP socket. */
    void StopTraffic();

  protected:
    void DoDispose() override;

  private:
    void StartApplication() override;
    void StopApplication() override;
    void DoStartApplication() override;
    void DoStopApplication() override;
    void CancelEvents() override;

    /**
     * Handle successful TCP connection establishment.
     *
     * @param socket Connected TCP socket.
     */
    void ConnectionSucceeded(Ptr<Socket> socket);

    /**
     * Handle failed TCP connection establishment.
     *
     * @param socket TCP socket whose connection failed.
     */
    void ConnectionFailed(Ptr<Socket> socket);

    /** Fill the TCP send buffer while buffer space remains available. */
    void SendData();

    /**
     * Resume send-buffer filling after TCP frees buffer space.
     *
     * @param socket Connected TCP socket.
     * @param availableBytes Newly available send-buffer bytes.
     */
    void DataSend(Ptr<Socket> socket, uint32_t availableBytes);

    uint32_t m_sendSize{512};         ///< Application packet size in bytes.
    Ptr<Packet> m_unsentPacket;       ///< Packet or fragment retained after buffer saturation.
    Callback<void> m_readyCallback;   ///< One-shot TCP readiness notification.
    bool m_applicationStarted{false}; ///< Whether application startup has run.
    bool m_running{false};            ///< Whether the application owns a live socket.
    bool m_ready{false};              ///< Whether TCP connected and readiness was emitted.
    bool m_trafficStarted{false};     ///< Whether the payload gate has opened.
};

} // namespace ns3

#endif // SATURATED_TCP_SENDER_H
