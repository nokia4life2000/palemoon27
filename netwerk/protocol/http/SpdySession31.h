/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_SpdySession31_h
#define mozilla_net_SpdySession31_h

// spdy/3.1

#include "ASpdySession.h"
#include "mozilla/Attributes.h"
#include "nsAHttpConnection.h"
#include "nsClassHashtable.h"
#include "nsDataHashtable.h"
#include "nsDeque.h"
#include "nsHashKeys.h"
#include "zlib.h"

class nsISocketTransport;

namespace mozilla { namespace net {

class SpdyPushedStream31;
class SpdyStream31;
class nsHttpTransaction;

class SpdySession31 final : public ASpdySession
                          , public nsAHttpConnection
                          , public nsAHttpSegmentReader
                          , public nsAHttpSegmentWriter
{
  ~SpdySession31();

public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSAHTTPTRANSACTION
  NS_DECL_NSAHTTPCONNECTION(mConnection)
  NS_DECL_NSAHTTPSEGMENTREADER
  NS_DECL_NSAHTTPSEGMENTWRITER

  explicit SpdySession31(nsISocketTransport *);

  bool AddStream(nsAHttpTransaction *, int32_t,
                 bool, nsIInterfaceRequestor *) override;
  bool CanReuse() override { return !mShouldGoAway && !mClosed; }
  bool RoomForMoreStreams() override;

  // When the connection is active this is called up to once every 1 second
  // return the interval (in seconds) that the connection next wants to
  // have this invoked. It might happen sooner depending on the needs of
  // other connections.
  uint32_t  ReadTimeoutTick(PRIntervalTime now) override;

  // Idle time represents time since "goodput".. e.g. a data or header frame
  PRIntervalTime IdleTime() override;

  // Registering with a newID of 0 means pick the next available odd ID
  uint32_t RegisterStreamID(SpdyStream31 *, uint32_t aNewID = 0);

  const static uint8_t kVersion        = 3;

  const static uint8_t kFlag_Control   = 0x80;

  const static uint8_t kFlag_Data_FIN  = 0x01;
  const static uint8_t kFlag_Data_UNI  = 0x02;

  enum
  {
    CONTROL_TYPE_FIRST = 0,
    CONTROL_TYPE_SYN_STREAM = 1,
    CONTROL_TYPE_SYN_REPLY = 2,
    CONTROL_TYPE_RST_STREAM = 3,
    CONTROL_TYPE_SETTINGS = 4,
    CONTROL_TYPE_NOOP = 5,                        /* deprecated */
    CONTROL_TYPE_PING = 6,
    CONTROL_TYPE_GOAWAY = 7,
    CONTROL_TYPE_HEADERS = 8,
    CONTROL_TYPE_WINDOW_UPDATE = 9,
    CONTROL_TYPE_CREDENTIAL = 10,
    CONTROL_TYPE_LAST = 11
  };

  enum rstReason
  {
    RST_PROTOCOL_ERROR = 1,
    RST_INVALID_STREAM = 2,
    RST_REFUSED_STREAM = 3,
    RST_UNSUPPORTED_VERSION = 4,
    RST_CANCEL = 5,
    RST_INTERNAL_ERROR = 6,
    RST_FLOW_CONTROL_ERROR = 7,
    RST_STREAM_IN_USE = 8,
    RST_STREAM_ALREADY_CLOSED = 9,
    RST_INVALID_CREDENTIALS = 10,
    RST_FRAME_TOO_LARGE = 11
  };

  enum goawayReason
  {
    OK = 0,
    PROTOCOL_ERROR = 1,
    INTERNAL_ERROR = 2,    // sometimes misdocumented as 11
    NUM_STATUS_CODES = 3   // reserved by chromium but undocumented
  };

  enum settingsFlags
  {
    PERSIST_VALUE = 1,
    PERSISTED_VALUE = 2
  };

  enum
  {
    SETTINGS_TYPE_UPLOAD_BW = 1, // kb/s
    SETTINGS_TYPE_DOWNLOAD_BW = 2, // kb/s
    SETTINGS_TYPE_RTT = 3, // ms
    SETTINGS_TYPE_MAX_CONCURRENT = 4, // streams
    SETTINGS_TYPE_CWND = 5, // packets
    SETTINGS_TYPE_DOWNLOAD_RETRANS_RATE = 6, // percentage
    SETTINGS_TYPE_INITIAL_WINDOW = 7,  // bytes for flow control
    SETTINGS_CLIENT_CERTIFICATE_VECTOR_SIZE = 8
  };

  // This should be big enough to hold all of your control packets,
  // but if it needs to grow for huge headers it can do so dynamically.
  // About 1% of responses from SPDY google services seem to be > 1000
  // with all less than 2000 when compression is enabled.
  const static uint32_t kDefaultBufferSize = 2048;

  // kDefaultQueueSize must be >= other queue size constants
  const static uint32_t kDefaultQueueSize =  32768;
  const static uint32_t kQueueMinimumCleanup = 24576;
  const static uint32_t kQueueTailRoom    =  4096;
  const static uint32_t kQueueReserved    =  1024;

  const static uint32_t kMaxStreamID = 0x7800000;

  // This is a sentinel for a deleted stream. It is not a valid
  // 31 bit stream ID.
  const static uint32_t kDeadStreamID = 0xffffdead;

  // below the emergency threshold of local window we ack every received
  // byte. Above that we coalesce bytes into the MinimumToAck size.
  const static int32_t  kEmergencyWindowThreshold = 1024 * 1024;
  const static uint32_t kMinimumToAck = 64 * 1024;

  // The default rwin is 64KB unless updated by a settings frame
  const static uint32_t kDefaultRwin = 64 * 1024;

  static nsresult HandleSynStream(SpdySession31 *);
  static nsresult HandleSynReply(SpdySession31 *);
  static nsresult HandleRstStream(SpdySession31 *);
  static nsresult HandleSettings(SpdySession31 *);
  static nsresult HandleNoop(SpdySession31 *);
  static nsresult HandlePing(SpdySession31 *);
  static nsresult HandleGoAway(SpdySession31 *);
  static nsresult HandleHeaders(SpdySession31 *);
  static nsresult HandleWindowUpdate(SpdySession31 *);
  static nsresult HandleCredential(SpdySession31 *);

  // For writing the SPDY data stream to LOG4
  static void LogIO(SpdySession31 *, SpdyStream31 *, const char *,
                    const char *, uint32_t);

  // an overload of nsAHttpConnection
  void TransactionHasDataToWrite(nsAHttpTransaction *) override;

  // a similar version for SpdyStream31
  void TransactionHasDataToWrite(SpdyStream31 *);

  // an overload of nsAHttpSegementReader
  virtual nsresult CommitToSegmentSize(uint32_t size, bool forceCommitment) override;
  nsresult BufferOutput(const char *, uint32_t, uint32_t *);
  void     FlushOutputQueue();
  uint32_t AmountOfOutputBuffered() { return mOutputQueueUsed - mOutputQueueSent; }

  uint32_t GetServerInitialStreamWindow() { return mServerInitialStreamWindow; }

  bool TryToActivate(SpdyStream31 *stream);
  void ConnectPushedStream(SpdyStream31 *stream);
  void DecrementConcurrent(SpdyStream31 *stream);

  uint64_t Serial() { return mSerial; }

  void     PrintDiagnostics (nsCString &log) override;

  // Streams need access to these
  uint32_t SendingChunkSize() { return mSendingChunkSize; }
  uint32_t PushAllowance() { return mPushAllowance; }
  z_stream *UpstreamZlib() { return &mUpstreamZlib; }
  nsISocketTransport *SocketTransport() { return mSocketTransport; }
  int64_t RemoteSessionWindow() { return mRemoteSessionWindow; }
  void DecrementRemoteSessionWindow (uint32_t bytes) { mRemoteSessionWindow -= bytes; }

  void SendPing() override;

  bool MaybeReTunnel(nsAHttpTransaction *) override;

private:

  enum stateType {
    BUFFERING_FRAME_HEADER,
    BUFFERING_CONTROL_FRAME,
    PROCESSING_DATA_FRAME,
    DISCARDING_DATA_FRAME,
    PROCESSING_COMPLETE_HEADERS,
    PROCESSING_CONTROL_RST_STREAM
  };

  nsresult    ResponseHeadersComplete();
  uint32_t    GetWriteQueueSize();
  void        ChangeDownstreamState(enum stateType);
  void        ResetDownstreamState();
  nsresult    UncompressAndDiscard(uint32_t, uint32_t);
  void        zlibInit();
  void        GeneratePing(uint32_t);
  void        GenerateRstStream(uint32_t, uint32_t);
  void        GenerateGoAway(uint32_t);
  void        CleanupStream(SpdyStream31 *, nsresult, rstReason);
  void        CloseStream(SpdyStream31 *, nsresult);
  void        GenerateSettings();
  void        RemoveStreamFromQueues(SpdyStream31 *);

  void        SetWriteCallbacks();
  void        RealignOutputQueue();

  void        ProcessPending();
  nsresult    SetInputFrameDataStream(uint32_t);
  bool        VerifyStream(SpdyStream31 *, uint32_t);
  void        SetNeedsCleanup();

  void        UpdateLocalRwin(SpdyStream31 *stream, uint32_t bytes);
  void        UpdateLocalStreamWindow(SpdyStream31 *stream, uint32_t bytes);
  void        UpdateLocalSessionWindow(uint32_t bytes);

  bool        RoomForMoreConcurrent();
  void        IncrementConcurrent(SpdyStream31 *stream);
  void        QueueStream(SpdyStream31 *stream);

  // a wrapper for all calls to the nshttpconnection level segment writer. Used
  // to track network I/O for timeout purposes
  nsresult   NetworkRead(nsAHttpSegmentWriter *, char *, uint32_t, uint32_t *);

  static PLDHashOperator ShutdownEnumerator(nsAHttpTransaction *,
                                            nsAutoPtr<SpdyStream31> &,
                                            void *);

  static PLDHashOperator GoAwayEnumerator(nsAHttpTransaction *,
                                          nsAutoPtr<SpdyStream31> &,
                                          void *);

  static PLDHashOperator UpdateServerRwinEnumerator(nsAHttpTransaction *,
                                                    nsAutoPtr<SpdyStream31> &,
                                                    void *);

  static PLDHashOperator RestartBlockedOnRwinEnumerator(nsAHttpTransaction *,
                                                        nsAutoPtr<SpdyStream31> &,
                                                        void *);

  // This is intended to be nsHttpConnectionMgr:nsConnectionHandle taken
  // from the first transaction on this session. That object contains the
  // pointer to the real network-level nsHttpConnection object.
  RefPtr<nsAHttpConnection> mConnection;

  // The underlying socket transport object is needed to propogate some events
  nsISocketTransport         *mSocketTransport;

  // These are temporary state variables to hold the argument to
  // Read/WriteSegments so it can be accessed by On(read/write)segment
  // further up the stack.
  nsAHttpSegmentReader       *mSegmentReader;
  nsAHttpSegmentWriter       *mSegmentWriter;

  uint32_t          mSendingChunkSize;        /* the transmission chunk size */
  uint32_t          mNextStreamID;            /* 24 bits */
  uint32_t          mConcurrentHighWater;     /* max parallelism on session */
  uint32_t          mPushAllowance;           /* rwin for unmatched pushes */

  stateType         mDownstreamState; /* in frame, between frames, etc..  */

  // Maintain 2 indexes - one by stream ID, one by transaction pointer.
  // There are also several lists of streams: ready to write, queued due to
  // max parallelism, streams that need to force a read for push, and the full
  // set of pushed streams.
  // The objects are not ref counted - they get destroyed
  // by the nsClassHashtable implementation when they are removed from
  // the transaction hash.
  nsDataHashtable<nsUint32HashKey, SpdyStream31 *>     mStreamIDHash;
  nsClassHashtable<nsPtrHashKey<nsAHttpTransaction>,
    SpdyStream31>                                     mStreamTransactionHash;

  nsDeque                                             mReadyForWrite;
  nsDeque                                             mQueuedStreams;
  nsDeque                                             mReadyForRead;
  nsTArray<SpdyPushedStream31 *>                      mPushedStreams;

  // Compression contexts for header transport using deflate.
  // SPDY compresses only HTTP headers and does not reset zlib in between
  // frames. Even data that is not associated with a stream (e.g invalid
  // stream ID) is passed through these contexts to keep the compression
  // context correct.
  z_stream            mDownstreamZlib;
  z_stream            mUpstreamZlib;

  // mInputFrameBuffer is used to store received control packets and the 8 bytes
  // of header on data packets
  uint32_t             mInputFrameBufferSize;
  uint32_t             mInputFrameBufferUsed;
  nsAutoArrayPtr<char> mInputFrameBuffer;

  // mInputFrameDataSize/Read are used for tracking the amount of data consumed
  // in a data frame. the data itself is not buffered in spdy
  // The frame size is mInputFrameDataSize + the constant 8 byte header
  uint32_t             mInputFrameDataSize;
  uint32_t             mInputFrameDataRead;
  bool                 mInputFrameDataLast; // This frame was marked FIN

  // When a frame has been received that is addressed to a particular stream
  // (e.g. a data frame after the stream-id has been decoded), this points
  // to the stream.
  SpdyStream31          *mInputFrameDataStream;

  // mNeedsCleanup is a state variable to defer cleanup of a closed stream
  // If needed, It is set in session::OnWriteSegments() and acted on and
  // cleared when the stack returns to session::WriteSegments(). The stream
  // cannot be destroyed directly out of OnWriteSegments because
  // stream::writeSegments() is on the stack at that time.
  SpdyStream31          *mNeedsCleanup;

  // The CONTROL_TYPE value for a control frame
  uint32_t             mFrameControlType;

  // This reason code in the last processed RESET frame
  uint32_t             mDownstreamRstReason;

  // for the conversion of downstream http headers into spdy formatted headers
  // The data here does not persist between frames
  nsCString            mFlatHTTPResponseHeaders;
  uint32_t             mFlatHTTPResponseHeadersOut;

  // when set, the session will go away when it reaches 0 streams. This flag
  // is set when: the stream IDs are running out (at either the client or the
  // server), when DontReuse() is called, a RST that is not specific to a
  // particular stream is received, a GOAWAY frame has been received from
  // the server.
  bool                 mShouldGoAway;

  // the session has received a nsAHttpTransaction::Close()  call
  bool                 mClosed;

  // the session received a GoAway frame with a valid GoAwayID
  bool                 mCleanShutdown;

  // indicates PROCESSING_COMPLETE_HEADERS state was pushed onto the stack
  // over an active PROCESSING_DATA_FRAME, which should be restored when
  // the processed headers are written to the stream
  bool                 mDataPending;

  // If a GoAway message was received this is the ID of the last valid
  // stream. 0 otherwise. (0 is never a valid stream id.)
  uint32_t             mGoAwayID;

  // The limit on number of concurrent streams for this session. Normally it
  // is basically unlimited, but the SETTINGS control message from the
  // server might bring it down.
  uint32_t             mMaxConcurrent;

  // The actual number of concurrent streams at this moment. Generally below
  // mMaxConcurrent, but the max can be lowered in real time to a value
  // below the current value
  uint32_t             mConcurrent;

  // The server rwin for new streams as determined from a SETTINGS frame
  uint32_t             mServerInitialStreamWindow;

  // The Local Session window is how much data the server is allowed to send
  // (across all streams) without getting a window update to stream 0. It is
  // signed because asynchronous changes via SETTINGS can drive it negative.
  int64_t              mLocalSessionWindow;

  // The Remote Session Window is how much data the client is allowed to send
  // (across all streams) without receiving a window update to stream 0. It is
  // signed because asynchronous changes via SETTINGS can drive it negative.
  int64_t              mRemoteSessionWindow;

  // This is a output queue of bytes ready to be written to the SSL stream.
  // When that streams returns WOULD_BLOCK on direct write the bytes get
  // coalesced together here. This results in larger writes to the SSL layer.
  // The buffer is not dynamically grown to accomodate stream writes, but
  // does expand to accept infallible session wide frames like GoAway and RST.
  uint32_t             mOutputQueueSize;
  uint32_t             mOutputQueueUsed;
  uint32_t             mOutputQueueSent;
  nsAutoArrayPtr<char> mOutputQueueBuffer;

  PRIntervalTime       mPingThreshold;
  PRIntervalTime       mLastReadEpoch;     // used for ping timeouts
  PRIntervalTime       mLastDataReadEpoch; // used for IdleTime()
  PRIntervalTime       mPingSentEpoch;
  uint32_t             mNextPingID;

  PRIntervalTime       mPreviousPingThreshold; // backup for the former value
  bool                 mPreviousUsed;          // true when backup is used

  // used as a temporary buffer while enumerating the stream hash during GoAway
  nsDeque  mGoAwayStreamsToRestart;

  // Each session gets a unique serial number because the push cache is correlated
  // by the load group and the serial number can be used as part of the cache key
  // to make sure streams aren't shared across sessions.
  uint64_t        mSerial;

private:
/// connect tunnels
  void DispatchOnTunnel(nsAHttpTransaction *, nsIInterfaceRequestor *);
  void CreateTunnel(nsHttpTransaction *, nsHttpConnectionInfo *, nsIInterfaceRequestor *);
  void RegisterTunnel(SpdyStream31 *);
  void UnRegisterTunnel(SpdyStream31 *);
  uint32_t FindTunnelCount(nsHttpConnectionInfo *);

  nsDataHashtable<nsCStringHashKey, uint32_t> mTunnelHash;
};

} // namespace net
} // namespace mozilla

#endif // mozilla_net_SpdySession31_h
