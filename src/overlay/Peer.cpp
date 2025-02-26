// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/Peer.h"

#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "crypto/Random.h"
#include "database/Database.h"
#include "overlay/StellarXDR.h"
#include "herder/Herder.h"
#include "herder/TxSetFrame.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/LoadManager.h"
#include "overlay/OverlayManager.h"
#include "overlay/PeerAuth.h"
#include "overlay/PeerRecord.h"
#include "util/Logging.h"

#include "medida/metrics_registry.h"
#include "medida/timer.h"
#include "medida/meter.h"

#include "xdrpp/marshal.h"

#include <soci.h>
#include <time.h>

// LATER: need to add some way of docking peers that are misbehaving by sending
// you bad data

namespace stellar
{

using namespace std;
using namespace soci;

medida::Meter&
Peer::getByteReadMeter(Application& app)
{
    return app.getMetrics().NewMeter({"overlay", "byte", "read"}, "byte");
}

medida::Meter&
Peer::getByteWriteMeter(Application& app)
{
    return app.getMetrics().NewMeter({"overlay", "byte", "write"}, "byte");
}

Peer::Peer(Application& app, PeerRole role)
    : mApp(app)
    , mRole(role)
    , mState(role == WE_CALLED_REMOTE ? CONNECTING : CONNECTED)
    , mRemoteOverlayVersion(0)
    , mRemoteListeningPort(0)
    , mIdleTimer(app)
    , mLastRead(app.getClock().now())
    , mLastWrite(app.getClock().now())

    , mMessageRead(
          app.getMetrics().NewMeter({"overlay", "message", "read"}, "message"))
    , mMessageWrite(
          app.getMetrics().NewMeter({"overlay", "message", "write"}, "message"))
    , mByteRead(getByteReadMeter(app))
    , mByteWrite(getByteWriteMeter(app))
    , mErrorRead(
          app.getMetrics().NewMeter({"overlay", "error", "read"}, "error"))
    , mErrorWrite(
          app.getMetrics().NewMeter({"overlay", "error", "write"}, "error"))
    , mTimeoutIdle(
          app.getMetrics().NewMeter({"overlay", "timeout", "idle"}, "timeout"))

    , mRecvErrorTimer(app.getMetrics().NewTimer({"overlay", "recv", "error"}))
    , mRecvHelloTimer(app.getMetrics().NewTimer({"overlay", "recv", "hello"}))
    , mRecvAuthTimer(app.getMetrics().NewTimer({"overlay", "recv", "auth"}))
    , mRecvDontHaveTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "dont-have"}))
    , mRecvGetPeersTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "get-peers"}))
    , mRecvPeersTimer(app.getMetrics().NewTimer({"overlay", "recv", "peers"}))
    , mRecvGetTxSetTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "get-txset"}))
    , mRecvTxSetTimer(app.getMetrics().NewTimer({"overlay", "recv", "txset"}))
    , mRecvTransactionTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "transaction"}))
    , mRecvGetSCPQuorumSetTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "get-scp-qset"}))
    , mRecvSCPQuorumSetTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "scp-qset"}))
    , mRecvSCPMessageTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "scp-message"}))
    , mRecvGetSCPStateTimer(
        app.getMetrics().NewTimer({ "overlay", "recv", "get-scp-state" }))

    , mSendErrorMeter(
          app.getMetrics().NewMeter({"overlay", "send", "error"}, "message"))
    , mSendHelloMeter(
          app.getMetrics().NewMeter({"overlay", "send", "hello"}, "message"))
    , mSendAuthMeter(
          app.getMetrics().NewMeter({"overlay", "send", "auth"}, "message"))
    , mSendDontHaveMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "dont-have"}, "message"))
    , mSendGetPeersMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "get-peers"}, "message"))
    , mSendPeersMeter(
          app.getMetrics().NewMeter({"overlay", "send", "peers"}, "message"))
    , mSendGetTxSetMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "get-txset"}, "message"))
    , mSendTransactionMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "transaction"}, "message"))
    , mSendTxSetMeter(
          app.getMetrics().NewMeter({"overlay", "send", "txset"}, "message"))
    , mSendGetSCPQuorumSetMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "get-scp-qset"}, "message"))
    , mSendSCPQuorumSetMeter(
          app.getMetrics().NewMeter({"overlay", "send", "scp-qset"}, "message"))
    , mSendSCPMessageSetMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "scp-message"}, "message"))
    , mSendGetSCPStateMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "get-scp-state"}, "message"))
    , mDropInConnectHandlerMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "connect-handler"}, "drop"))
    , mDropInRecvMessageDecodeMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-message-decode"}, "drop"))
    , mDropInRecvMessageSeqMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-message-seq"}, "drop"))
    , mDropInRecvMessageMacMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-message-mac"}, "drop"))
    , mDropInRecvMessageUnauthMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-message-unauth"}, "drop"))
    , mDropInRecvHelloUnexpectedMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-hello-unexpected"}, "drop"))
    , mDropInRecvHelloVersionMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-hello-version"}, "drop"))
    , mDropInRecvHelloSelfMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-hello-self"}, "drop"))
    , mDropInRecvHelloPeerIDMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-hello-peerid"}, "drop"))
    , mDropInRecvHelloCertMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-hello-cert"}, "drop"))
    , mDropInRecvHelloNetMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-hello-net"}, "drop"))
    , mDropInRecvHelloPortMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-hello-port"}, "drop"))
    , mDropInRecvAuthUnexpectedMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-auth-unexpected"}, "drop"))
    , mDropInRecvAuthRejectMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-auth-reject"}, "drop"))
    , mDropInRecvErrorMeter(
          app.getMetrics().NewMeter({"overlay", "drop", "recv-error"}, "drop"))
{
    auto bytes = randomBytes(mSendNonce.size());
    std::copy(bytes.begin(), bytes.end(), mSendNonce.begin());
}

// copy/pasted from sendHello2
// (to be removed when HELLO is not used)
void
Peer::sendHello()
{
    CLOG(DEBUG, "Overlay") << "Peer::sendHello to " << toString();
    StellarMessage msg;
    msg.type(HELLO);
    Hello& elo = msg.hello();
    elo.ledgerVersion = mApp.getConfig().LEDGER_PROTOCOL_VERSION;
    elo.overlayVersion = mApp.getConfig().OVERLAY_PROTOCOL_VERSION;
    elo.versionStr = mApp.getConfig().VERSION_STR;
    elo.networkID = mApp.getNetworkID();
    elo.listeningPort = mApp.getConfig().PEER_PORT;
    elo.peerID = mApp.getConfig().NODE_SEED.getPublicKey();
    elo.cert = this->getAuthCert();
    elo.nonce = mSendNonce;
    sendMessage(msg);
}

void
Peer::sendHello2()
{
    CLOG(DEBUG, "Overlay") << "Peer::sendHello to " << toString();
    StellarMessage msg;
    msg.type(HELLO2);
    Hello2& elo = msg.hello2();
    elo.ledgerVersion = mApp.getConfig().LEDGER_PROTOCOL_VERSION;
    elo.overlayMinVersion = mApp.getConfig().OVERLAY_PROTOCOL_MIN_VERSION;
    elo.overlayVersion = mApp.getConfig().OVERLAY_PROTOCOL_VERSION;
    elo.versionStr = mApp.getConfig().VERSION_STR;
    elo.networkID = mApp.getNetworkID();
    elo.listeningPort = mApp.getConfig().PEER_PORT;
    elo.peerID = mApp.getConfig().NODE_SEED.getPublicKey();
    elo.cert = this->getAuthCert();
    elo.nonce = mSendNonce;
    sendMessage(msg);
}

AuthCert
Peer::getAuthCert()
{
    return mApp.getOverlayManager().getPeerAuth().getAuthCert();
}

size_t
Peer::getIOTimeoutSeconds() const
{
    if (isAuthenticated())
    {
        // Normally willing to wait 30s to hear anything
        // from an authenticated peer.
        return 30;
    }
    else
    {
        // We give peers much less timing leeway while
        // performing handshake.
        return 5;
    }
}

void
Peer::receivedBytes(size_t byteCount, bool gotFullMessage)
{
    LoadManager::PeerContext loadCtx(mApp, mPeerID);
    mLastRead = mApp.getClock().now();
    if (gotFullMessage)
        mMessageRead.Mark();
    mByteRead.Mark(byteCount);
}

void
Peer::startIdleTimer()
{
    if (shouldAbort())
    {
        return;
    }

    auto self = shared_from_this();
    mIdleTimer.expires_from_now(std::chrono::seconds(getIOTimeoutSeconds()));
    mIdleTimer.async_wait([self](asio::error_code const& error)
                                       {
                                           self->idleTimerExpired(error);
                                       });
}

void
Peer::idleTimerExpired(asio::error_code const& error)
{
    if (!error)
    {
        auto now = mApp.getClock().now();
        auto timeout = std::chrono::seconds(getIOTimeoutSeconds());
        if (((now - mLastRead) >= timeout) &&
            ((now - mLastWrite) >= timeout))
        {
            CLOG(WARNING, "Overlay") << "idle timeout";
            mTimeoutIdle.Mark();
            drop();
        }
        else
        {
            startIdleTimer();
        }
    }
}

void
Peer::sendAuth()
{
    StellarMessage msg;
    msg.type(AUTH);
    sendMessage(msg);
}

std::string
Peer::toString()
{
    std::stringstream s;
    s << getIP() << ":" << mRemoteListeningPort;
    return s.str();
}

void
Peer::drop(ErrorCode err, std::string const& msg)
{
    StellarMessage m;
    m.type(ERROR_MSG);
    m.error().code = err;
    m.error().msg = msg;
    sendMessage(m);
    // note: this used to be a post which caused delays in stopping
    // to process read messages.
    // this has no effect wrt the sending queue.
    drop();
}

void
Peer::connectHandler(asio::error_code const& error)
{
    if (error)
    {
        CLOG(WARNING, "Overlay")
            << " connectHandler error: " << error.message();
        mDropInConnectHandlerMeter.Mark();
        drop();
    }
    else
    {
        CLOG(DEBUG, "Overlay") << "connected " << toString();
        connected();
        mState = CONNECTED;
        sendHello();
    }
}

void
Peer::sendDontHave(MessageType type, uint256 const& itemID)
{
    StellarMessage msg;
    msg.type(DONT_HAVE);
    msg.dontHave().reqHash = itemID;
    msg.dontHave().type = type;

    sendMessage(msg);
}

void
Peer::sendSCPQuorumSet(SCPQuorumSetPtr qSet)
{
    StellarMessage msg;
    msg.type(SCP_QUORUMSET);
    msg.qSet() = *qSet;

    sendMessage(msg);
}
void
Peer::sendGetTxSet(uint256 const& setID)
{
    StellarMessage newMsg;
    newMsg.type(GET_TX_SET);
    newMsg.txSetHash() = setID;

    sendMessage(newMsg);
}
void
Peer::sendGetQuorumSet(uint256 const& setID)
{
    CLOG(TRACE, "Overlay") << "Get quorum set: " << hexAbbrev(setID);

    StellarMessage newMsg;
    newMsg.type(GET_SCP_QUORUMSET);
    newMsg.qSetHash() = setID;

    sendMessage(newMsg);
}

void
Peer::sendGetPeers()
{
    CLOG(TRACE, "Overlay") << "Get peers";

    StellarMessage newMsg;
    newMsg.type(GET_PEERS);

    sendMessage(newMsg);
}

void
Peer::sendGetScpState(uint32 ledgerSeq)
{
    CLOG(TRACE, "Overlay") << "Get SCP State for " << ledgerSeq;

    StellarMessage newMsg;
    newMsg.type(GET_SCP_STATE);
    newMsg.getSCPLedgerSeq() = ledgerSeq;

    sendMessage(newMsg);
}

void
Peer::sendPeers()
{
    // send top 50 peers we know about
    vector<PeerRecord> peerList;
    PeerRecord::loadPeerRecords(mApp.getDatabase(), 50, mApp.getClock().now(),
                                peerList);
    StellarMessage newMsg;
    newMsg.type(PEERS);
    newMsg.peers().reserve(peerList.size());
    for (auto const& pr : peerList)
    {
        if (pr.isPrivateAddress() ||
            pr.isSelfAddressAndPort(getIP(), mRemoteListeningPort))
        {
            continue;
        }
        PeerAddress pa;
        pr.toXdr(pa);
        newMsg.peers().push_back(pa);
    }
    sendMessage(newMsg);
}

void
Peer::sendMessage(StellarMessage const& msg)
{
    CLOG(TRACE, "Overlay") << "("
                           << mApp.getConfig().toShortString(
                                  mApp.getConfig().NODE_SEED.getPublicKey())
                           << ") send: " << msg.type()
                           << " to : " << mApp.getConfig().toShortString(mPeerID);

    switch (msg.type())
    {
    case ERROR_MSG:
        mSendErrorMeter.Mark();
        break;
    case HELLO:
    case HELLO2:
        mSendHelloMeter.Mark();
        break;
    case AUTH:
        mSendAuthMeter.Mark();
        break;
    case DONT_HAVE:
        mSendDontHaveMeter.Mark();
        break;
    case GET_PEERS:
        mSendGetPeersMeter.Mark();
        break;
    case PEERS:
        mSendPeersMeter.Mark();
        break;
    case GET_TX_SET:
        mSendGetTxSetMeter.Mark();
        break;
    case TX_SET:
        mSendTxSetMeter.Mark();
        break;
    case TRANSACTION:
        mSendTransactionMeter.Mark();
        break;
    case GET_SCP_QUORUMSET:
        mSendGetSCPQuorumSetMeter.Mark();
        break;
    case SCP_QUORUMSET:
        mSendSCPQuorumSetMeter.Mark();
        break;
    case SCP_MESSAGE:
        mSendSCPMessageSetMeter.Mark();
        break;
    case GET_SCP_STATE:
        mSendGetSCPStateMeter.Mark();
        break;
    };

    AuthenticatedMessage amsg;
    amsg.v0().message = msg;
    if (msg.type() != HELLO && msg.type() != ERROR_MSG)
    {
        amsg.v0().sequence = mSendMacSeq;
        amsg.v0().mac =
            hmacSha256(mSendMacKey, xdr::xdr_to_opaque(mSendMacSeq, msg));
        ++mSendMacSeq;
    }
    xdr::msg_ptr xdrBytes(xdr::xdr_to_msg(amsg));
    this->sendMessage(std::move(xdrBytes));
}

void
Peer::recvMessage(xdr::msg_ptr const& msg)
{
    if (shouldAbort())
    {
        return;
    }

    LoadManager::PeerContext loadCtx(mApp, mPeerID);

    CLOG(TRACE, "Overlay") << "received xdr::msg_ptr";
    try
    {
        AuthenticatedMessage am;
        xdr::xdr_from_msg(msg, am);
        recvMessage(am);
    }
    catch (xdr::xdr_runtime_error& e)
    {
        CLOG(ERROR, "Overlay") << "received corrupt xdr::msg_ptr " << e.what();
        mDropInRecvMessageDecodeMeter.Mark();
        drop();
        return;
    }
}

bool
Peer::isConnected() const
{
    return mState != CONNECTING && mState != CLOSING;
}

bool
Peer::isAuthenticated() const
{
    return mState == GOT_AUTH;
}

bool
Peer::shouldAbort() const
{
    return (mState == CLOSING) || mApp.getOverlayManager().isShuttingDown();
}

void
Peer::recvMessage(AuthenticatedMessage const& msg)
{
    if (shouldAbort())
    {
        return;
    }

    if (mState >= GOT_HELLO && msg.v0().message.type() != ERROR_MSG)
    {
        if (msg.v0().sequence != mRecvMacSeq)
        {
            CLOG(ERROR, "Overlay") << "Unexpected message-auth sequence";
            mDropInRecvMessageSeqMeter.Mark();
            ++mRecvMacSeq;
            drop(ERR_AUTH, "unexpected auth sequence");
            return;
        }

        if (!hmacSha256Verify(
            msg.v0().mac, mRecvMacKey,
            xdr::xdr_to_opaque(msg.v0().sequence, msg.v0().message)))
        {
            CLOG(ERROR, "Overlay") << "Message-auth check failed";
            mDropInRecvMessageMacMeter.Mark();
            ++mRecvMacSeq;
            drop(ERR_AUTH, "unexpected MAC");
            return;
        }
        ++mRecvMacSeq;
    }
    recvMessage(msg.v0().message);
}

void
Peer::recvMessage(StellarMessage const& stellarMsg)
{
    if (shouldAbort())
    {
        return;
    }

    CLOG(TRACE, "Overlay") << "("
                           << mApp.getConfig().toShortString(
                                  mApp.getConfig().NODE_SEED.getPublicKey())
                           << ") recv: " << stellarMsg.type()
                           << " from:" << mApp.getConfig().toShortString(mPeerID);

    if (!isAuthenticated() && (stellarMsg.type() != HELLO) &&
        (stellarMsg.type() != AUTH) && (stellarMsg.type() != ERROR_MSG))
    {
        CLOG(WARNING, "Overlay") << "recv: " << stellarMsg.type()
                                 << " before completed handshake";
        mDropInRecvMessageUnauthMeter.Mark();
        drop();
        return;
    }

    assert(isAuthenticated() || stellarMsg.type() == HELLO ||
           stellarMsg.type() == AUTH || stellarMsg.type() == ERROR_MSG);

    switch (stellarMsg.type())
    {
    case ERROR_MSG:
    {
        auto t = mRecvErrorTimer.TimeScope();
        recvError(stellarMsg);
    }
    break;

    case HELLO:
    {
        auto t = mRecvHelloTimer.TimeScope();
        this->recvHello(stellarMsg.hello());
    }
    break;
    case HELLO2:
    {
        auto t = mRecvHelloTimer.TimeScope();
        this->recvHello2(stellarMsg.hello2());
    }
    break;

    case AUTH:
    {
        auto t = mRecvAuthTimer.TimeScope();
        this->recvAuth(stellarMsg);
    }
    break;

    case DONT_HAVE:
    {
        auto t = mRecvDontHaveTimer.TimeScope();
        recvDontHave(stellarMsg);
    }
    break;

    case GET_PEERS:
    {
        auto t = mRecvGetPeersTimer.TimeScope();
        recvGetPeers(stellarMsg);
    }
    break;

    case PEERS:
    {
        auto t = mRecvPeersTimer.TimeScope();
        recvPeers(stellarMsg);
    }
    break;

    case GET_TX_SET:
    {
        auto t = mRecvGetTxSetTimer.TimeScope();
        recvGetTxSet(stellarMsg);
    }
    break;

    case TX_SET:
    {
        auto t = mRecvTxSetTimer.TimeScope();
        recvTxSet(stellarMsg);
    }
    break;

    case TRANSACTION:
    {
        auto t = mRecvTransactionTimer.TimeScope();
        recvTransaction(stellarMsg);
    }
    break;

    case GET_SCP_QUORUMSET:
    {
        auto t = mRecvGetSCPQuorumSetTimer.TimeScope();
        recvGetSCPQuorumSet(stellarMsg);
    }
    break;

    case SCP_QUORUMSET:
    {
        auto t = mRecvSCPQuorumSetTimer.TimeScope();
        recvSCPQuorumSet(stellarMsg);
    }
    break;

    case SCP_MESSAGE:
    {
        auto t = mRecvSCPMessageTimer.TimeScope();
        recvSCPMessage(stellarMsg);
    }
    break;

    case GET_SCP_STATE:
    {
        auto t = mRecvGetSCPStateTimer.TimeScope();
        recvGetSCPState(stellarMsg);
    }
    break;
    }
}

void
Peer::recvDontHave(StellarMessage const& msg)
{
    mApp.getHerder().peerDoesntHave(msg.dontHave().type, msg.dontHave().reqHash,
                                    shared_from_this());
}

void
Peer::recvGetTxSet(StellarMessage const& msg)
{
    auto self = shared_from_this();
    if (auto txSet = mApp.getHerder().getTxSet(msg.txSetHash()))
    {
        StellarMessage newMsg;
        newMsg.type(TX_SET);
        txSet->toXDR(newMsg.txSet());

        self->sendMessage(newMsg);
    }
    else
    {
        sendDontHave(TX_SET, msg.txSetHash());
    }
}

void
Peer::recvTxSet(StellarMessage const& msg)
{
    TxSetFrame frame(mApp.getNetworkID(), msg.txSet());
    mApp.getHerder().recvTxSet(frame.getContentsHash(), frame);
}

void
Peer::recvTransaction(StellarMessage const& msg)
{
    TransactionFramePtr transaction = TransactionFrame::makeTransactionFromWire(
        mApp.getNetworkID(), msg.transaction());
    if (transaction)
    {
        // add it to our current set
        // and make sure it is valid
        auto recvRes = mApp.getHerder().recvTransaction(transaction);

        if (recvRes == Herder::TX_STATUS_PENDING || recvRes == Herder::TX_STATUS_DUPLICATE)
        {
            // record that this peer sent us this transaction
            mApp.getOverlayManager().recvFloodedMsg(msg, shared_from_this());

            if (recvRes == Herder::TX_STATUS_PENDING)
            {
                // if it's a new transaction, broadcast it
                mApp.getOverlayManager().broadcastMessage(msg);
            }
        }
    }
}

void
Peer::recvGetSCPQuorumSet(StellarMessage const& msg)
{
    SCPQuorumSetPtr qset = mApp.getHerder().getQSet(msg.qSetHash());

    if (qset)
    {
        sendSCPQuorumSet(qset);
    }
    else
    {
        CLOG(TRACE, "Overlay")
            << "No quorum set: " << hexAbbrev(msg.qSetHash());
        sendDontHave(SCP_QUORUMSET, msg.qSetHash());
        // do we want to ask other people for it?
    }
}
void
Peer::recvSCPQuorumSet(StellarMessage const& msg)
{
    Hash hash = sha256(xdr::xdr_to_opaque(msg.qSet()));
    mApp.getHerder().recvSCPQuorumSet(hash, msg.qSet());
}

void
Peer::recvSCPMessage(StellarMessage const& msg)
{
    SCPEnvelope const& envelope = msg.envelope();
    CLOG(TRACE, "Overlay") << "recvSCPMessage node: "
                           << mApp.getConfig().toShortString(
                                  msg.envelope().statement.nodeID);

    mApp.getOverlayManager().recvFloodedMsg(msg, shared_from_this());

    mApp.getHerder().recvSCPEnvelope(envelope);
}

void
Peer::recvGetSCPState(StellarMessage const& msg)
{
    uint32 seq = msg.getSCPLedgerSeq();
    CLOG(TRACE, "Overlay") << "get SCP State " << seq;
    mApp.getHerder().sendSCPStateToPeer(seq, shared_from_this());
}

void
Peer::recvError(StellarMessage const& msg)
{
    std::string codeStr = "UNKNOWN";
    switch (msg.error().code)
    {
    case ERR_MISC:
        codeStr = "ERR_MISC";
        break;
    case ERR_DATA:
        codeStr = "ERR_DATA";
        break;
    case ERR_CONF:
        codeStr = "ERR_CONF";
        break;
    case ERR_AUTH:
        codeStr = "ERR_AUTH";
        break;
    case ERR_LOAD:
        codeStr = "ERR_LOAD";
        break;
    default:
        break;
    }
    CLOG(WARNING, "Overlay") << "Received error (" << codeStr
                             << "): " << msg.error().msg;
    mDropInRecvErrorMeter.Mark();
    drop();
}

void
Peer::noteHandshakeSuccessInPeerRecord()
{
    auto pr = PeerRecord::loadPeerRecord(mApp.getDatabase(), getIP(),
                                         getRemoteListeningPort());
    if (pr)
    {
        pr->resetBackOff(mApp.getClock());
    }
    else
    {
        pr = make_optional<PeerRecord>(getIP(), mRemoteListeningPort,
                                       mApp.getClock().now());
    }
    CLOG(INFO, "Overlay") << "successful handshake with "
                          << mApp.getConfig().toShortString(mPeerID) << "@"
                          << pr->toString();
    pr->storePeerRecord(mApp.getDatabase());
}

// this is a copy/pasted from recvHello2
// only differences are
// force mRemoteOverlayMinVersion to protocol version
// sendHello (we can't assume that the remote understands sendHello2)

void
Peer::recvHello(Hello const& elo)
{
    using xdr::operator==;

    if (mState >= GOT_HELLO)
    {
        CLOG(ERROR, "Overlay") << "received unexpected HELLO";
        mDropInRecvHelloUnexpectedMeter.Mark();
        drop();
        return;
    }

    auto& peerAuth = mApp.getOverlayManager().getPeerAuth();
    if (!peerAuth.verifyRemoteAuthCert(elo.peerID, elo.cert))
    {
        CLOG(ERROR, "Overlay") << "failed to verify remote peer auth cert";
        mDropInRecvHelloCertMeter.Mark();
        drop();
        return;
    }

    mRemoteListeningPort =
        static_cast<unsigned short>(elo.listeningPort);
    mRemoteOverlayMinVersion = elo.overlayVersion;   /// This is the only difference
    mRemoteOverlayVersion = elo.overlayVersion;
    mRemoteVersion = elo.versionStr;
    mPeerID = elo.peerID;
    mRecvNonce = elo.nonce;
    mSendMacSeq = 0;
    mRecvMacSeq = 0;
    mSendMacKey = peerAuth.getSendingMacKey(elo.cert.pubkey, mSendNonce,
                                            mRecvNonce, mRole);
    mRecvMacKey = peerAuth.getReceivingMacKey(elo.cert.pubkey,
                                              mSendNonce, mRecvNonce, mRole);

    mState = GOT_HELLO;
    CLOG(DEBUG, "Overlay") << "recvHello from " << toString();

    if (mRole == REMOTE_CALLED_US)
    {
        // Send a HELLO back, even if it's going to be followed
        // immediately by ERROR, because ERROR is an authenticated
        // message type and the caller won't decode it right if
        // still waiting for an unauthenticated HELLO.
        sendHello();
    }

    if (mRemoteOverlayMinVersion > mRemoteOverlayVersion ||
        mRemoteOverlayVersion < mApp.getConfig().OVERLAY_PROTOCOL_MIN_VERSION ||
        mRemoteOverlayMinVersion > mApp.getConfig().OVERLAY_PROTOCOL_VERSION)
    {
        CLOG(ERROR, "Overlay")
            << "connection from peer with incompatible overlay protocol version";
        CLOG(DEBUG, "Overlay")
            << "Protocol = [" << mRemoteOverlayMinVersion << "," << mRemoteOverlayVersion
            << "] expected: [" << mApp.getConfig().OVERLAY_PROTOCOL_VERSION
            << "," << mApp.getConfig().OVERLAY_PROTOCOL_VERSION << "]";
        mDropInRecvHelloVersionMeter.Mark();
        drop(ERR_CONF, "wrong protocol version");
        return;
    }

    if (elo.peerID == mApp.getConfig().NODE_SEED.getPublicKey())
    {
        CLOG(WARNING, "Overlay") << "connecting to self";
        mDropInRecvHelloSelfMeter.Mark();
        drop(ERR_CONF, "connecting to self");
        return;
    }

    if (elo.networkID != mApp.getNetworkID())
    {
        CLOG(WARNING, "Overlay")
            << "connection from peer with different NetworkID";
        CLOG(DEBUG, "Overlay")
            << "NetworkID = " << hexAbbrev(elo.networkID)
            << " expected: " << hexAbbrev(mApp.getNetworkID());
        mDropInRecvHelloNetMeter.Mark();
        drop(ERR_CONF, "wrong network passphrase");
        return;
    }

    for (auto const& p : mApp.getOverlayManager().getPeers())
    {
        if (&(p->mPeerID) == &mPeerID)
        {
            continue;
        }
        if (p->getPeerID() == mPeerID)
        {
            CLOG(WARNING, "Overlay")
                << "connection from already-connected peerID "
                << mApp.getConfig().toShortString(mPeerID);
            mDropInRecvHelloPeerIDMeter.Mark();
            drop(ERR_CONF, "connecting already-connected peer");
            return;
        }
    }

    if (elo.listeningPort <= 0 ||
        elo.listeningPort > UINT16_MAX)
    {
        CLOG(WARNING, "Overlay") << "bad port in recvHello";
        mDropInRecvHelloPortMeter.Mark();
        drop(ERR_CONF, "bad port number");
        return;
    }

    if (mRole == WE_CALLED_REMOTE)
    {
        sendAuth();
    }
}

void
Peer::recvHello2(Hello2 const& elo)
{
    using xdr::operator==;

    if (mState >= GOT_HELLO)
    {
        CLOG(ERROR, "Overlay")
            << "received unexpected HELLO";
        mDropInRecvHelloUnexpectedMeter.Mark();
        drop();
        return;
    }

    auto& peerAuth = mApp.getOverlayManager().getPeerAuth();
    if (!peerAuth.verifyRemoteAuthCert(elo.peerID, elo.cert))
    {
        CLOG(ERROR, "Overlay") << "failed to verify remote peer auth cert";
        mDropInRecvHelloCertMeter.Mark();
        drop();
        return;
    }

    mRemoteListeningPort =
        static_cast<unsigned short>(elo.listeningPort);
    mRemoteOverlayMinVersion = elo.overlayMinVersion;
    mRemoteOverlayVersion = elo.overlayVersion;
    mRemoteVersion = elo.versionStr;
    mPeerID = elo.peerID;
    mRecvNonce = elo.nonce;
    mSendMacSeq = 0;
    mRecvMacSeq = 0;
    mSendMacKey = peerAuth.getSendingMacKey(elo.cert.pubkey, mSendNonce,
                                            mRecvNonce, mRole);
    mRecvMacKey = peerAuth.getReceivingMacKey(elo.cert.pubkey,
                                              mSendNonce, mRecvNonce, mRole);

    mState = GOT_HELLO;
    CLOG(DEBUG, "Overlay") << "recvHello from " << toString();

    if (mRole == REMOTE_CALLED_US)
    {
        // Send a HELLO2 back, even if it's going to be followed
        // immediately by ERROR, because ERROR is an authenticated
        // message type and the caller won't decode it right if
        // still waiting for an unauthenticated HELLO2.
        sendHello2();
    }

    if (mRemoteOverlayMinVersion > mRemoteOverlayVersion ||
        mRemoteOverlayVersion < mApp.getConfig().OVERLAY_PROTOCOL_MIN_VERSION ||
        mRemoteOverlayMinVersion > mApp.getConfig().OVERLAY_PROTOCOL_VERSION)
    {
        CLOG(ERROR, "Overlay")
            << "connection from peer with incompatible overlay protocol version";
        CLOG(DEBUG, "Overlay")
            << "Protocol = [" << mRemoteOverlayMinVersion << "," << mRemoteOverlayVersion
            << "] expected: [" << mApp.getConfig().OVERLAY_PROTOCOL_VERSION
            << "," << mApp.getConfig().OVERLAY_PROTOCOL_VERSION << "]";
        mDropInRecvHelloVersionMeter.Mark();
        drop(ERR_CONF, "wrong protocol version");
        return;
    }

    if (elo.peerID == mApp.getConfig().NODE_SEED.getPublicKey())
    {
        CLOG(WARNING, "Overlay") << "connecting to self";
        mDropInRecvHelloSelfMeter.Mark();
        drop(ERR_CONF, "connecting to self");
        return;
    }

    if (elo.networkID != mApp.getNetworkID())
    {
        CLOG(WARNING, "Overlay")
            << "connection from peer with different NetworkID";
        CLOG(DEBUG, "Overlay")
            << "NetworkID = " << hexAbbrev(elo.networkID)
            << " expected: " << hexAbbrev(mApp.getNetworkID());
        mDropInRecvHelloNetMeter.Mark();
        drop(ERR_CONF, "wrong network passphrase");
        return;
    }

    for (auto const& p : mApp.getOverlayManager().getPeers())
    {
        if (&(p->mPeerID) == &mPeerID)
        {
            continue;
        }
        if (p->getPeerID() == mPeerID)
        {
            CLOG(WARNING, "Overlay")
                << "connection from already-connected peerID "
                << mApp.getConfig().toShortString(mPeerID);
            mDropInRecvHelloPeerIDMeter.Mark();
            drop(ERR_CONF, "connecting already-connected peer");
            return;
        }
    }

    if (elo.listeningPort <= 0 ||
        elo.listeningPort > UINT16_MAX)
    {
        CLOG(WARNING, "Overlay") << "bad port in recvHello";
        mDropInRecvHelloPortMeter.Mark();
        drop(ERR_CONF, "bad port number");
        return;
    }

    if (mRole == WE_CALLED_REMOTE)
    {
        sendAuth();
    }
}

void
Peer::recvAuth(StellarMessage const& msg)
{
    if (isAuthenticated())
    {
        CLOG(ERROR, "Overlay") << "Unexpected AUTH message";
        mDropInRecvAuthUnexpectedMeter.Mark();
        drop(ERR_MISC, "out-of-order AUTH message");
        return;
    }

    noteHandshakeSuccessInPeerRecord();
    mState = GOT_AUTH;

    auto self = shared_from_this();

    if (!mApp.getOverlayManager().isPeerAccepted(self))
    {
        CLOG(WARNING, "Overlay") << "New peer rejected, all slots taken";
        mDropInRecvAuthRejectMeter.Mark();
        drop(ERR_LOAD, "peer rejected");
        return;
    }

    if (mRole == REMOTE_CALLED_US)
    {
        sendAuth();
        sendPeers();
    }

    // send SCP State
    mApp.getHerder().sendSCPStateToPeer(0, self);
}

void
Peer::recvGetPeers(StellarMessage const& msg)
{
    sendPeers();
}

void
Peer::recvPeers(StellarMessage const& msg)
{
    const uint32 NEW_PEER_WINDOW_SECONDS = 10;

    for (auto const& peer : msg.peers())
    {
        if (peer.port == 0 || peer.port > UINT16_MAX)
        {
            CLOG(WARNING, "Overlay") << "ignoring received peer with bad port "
                                     << peer.port;
            continue;
        }
        if (peer.ip.type() == IPv6)
        {
            CLOG(WARNING, "Overlay") << "ignoring received IPv6 address"
                                     << " (not yet supported)";
            continue;
        }
        // randomize when we'll try to connect to this peer next if we don't
        // know it
        auto defaultNextAttempt = mApp.getClock().now() + std::chrono::seconds(
            std::rand() % NEW_PEER_WINDOW_SECONDS);

        stringstream ip;
        ip << (int)peer.ip.ipv4()[0] << "." << (int)peer.ip.ipv4()[1] << "."
           << (int)peer.ip.ipv4()[2] << "." << (int)peer.ip.ipv4()[3];
        // don't use peer.numFailures here as we may have better luck
        // (and we don't want to poison our failure count)
        PeerRecord pr{ip.str(), static_cast<unsigned short>(peer.port),
                      defaultNextAttempt, 0 };

        if (pr.isPrivateAddress())
        {
            CLOG(WARNING, "Overlay") << "ignoring received private address "
                                     << pr.toString();
        }
        else if (pr.isSelfAddressAndPort(getIP(), mApp.getConfig().PEER_PORT))
        {
            CLOG(WARNING, "Overlay") << "ignoring received self-address "
                                     << pr.toString();
        }
        else if (pr.isLocalhost() && !mApp.getConfig().ALLOW_LOCALHOST_FOR_TESTING)
        {
            CLOG(WARNING, "Overlay") << "ignoring received localhost";
        }
        else
        {
            pr.insertIfNew(mApp.getDatabase());
        }
    }
}
}
