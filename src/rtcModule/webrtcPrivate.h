#ifndef WEBRTCSFU_H
#define WEBRTCSFU_H

#include <logger.h>
#include <rtcModule/webrtcAdapter.h>
#include <rtcModule/webrtc.h>
#include <rtcModule/rtcStats.h>
#include <sfu.h>
#include <IVideoRenderer.h>

#include <map>

namespace rtcModule
{
#ifndef KARERE_DISABLE_WEBRTC
class RtcModuleSfu;
class Call;
class Session;

class AudioLevelMonitor : public webrtc::AudioTrackSinkInterface, public karere::DeleteTrackable
{
    public:
    AudioLevelMonitor(Call &call, void* appCtx, int32_t cid = -1);
    void OnData(const void *audio_data,
                        int bits_per_sample,
                        int sample_rate,
                        size_t number_of_channels,
                        size_t number_of_frames) override;
    bool hasAudio();
    void onAudioDetected(bool audioDetected);

private:
    time_t mPreviousTime = 0;
    Call &mCall;
    bool mAudioDetected = false;

    // Note that currently max CID allowed by this class is 65535
    int32_t mCid;
    void* mAppCtx;
};

/**
 * This class represent a generic instance to manage webrtc Transceiver
 * A Transceiver is an element used to send or receive datas
 */
class Slot
{
public:
    virtual ~Slot();
    webrtc::RtpTransceiverInterface* getTransceiver() { return mTransceiver.get(); }
    IvStatic_t getIv() const { return mIv; }
    uint32_t getTransceiverMid() const;
protected:
    Call &mCall;
    IvStatic_t mIv = 0;
    rtc::scoped_refptr<webrtc::RtpTransceiverInterface> mTransceiver;

    Slot(Call& call, rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver);
};

/**
 * This class represent a webrtc transceiver for local audio and low resolution video
 */
class LocalSlot : public Slot
{
public:
    LocalSlot(Call& call, rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver);
    void createEncryptor();
    void generateRandomIv();
};

/**
 * This class represent a generic instance to manage remote webrtc Transceiver
 */
class RemoteSlot : public Slot
{
public:
    virtual ~RemoteSlot() {}
    virtual void createDecryptor(Cid_t cid, IvStatic_t iv);
    virtual void release();
    Cid_t getCid() const { return mCid; }

protected:
    Cid_t mCid = 0;
    void* mAppCtx;
    RemoteSlot(Call& call, rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver, void* appCtx);
    void assign(Cid_t cid, IvStatic_t iv);

private:
    void enableTrack(bool enable, TrackDirection direction);
};

class VideoSink : public rtc::VideoSinkInterface<webrtc::VideoFrame>, public karere::DeleteTrackable
{
public:
    VideoSink(void* appCtx);
    virtual ~VideoSink();
    void setVideoRender(IVideoRenderer* videoRenderer);
    virtual void OnFrame(const webrtc::VideoFrame& frame) override;
private:
    std::unique_ptr<IVideoRenderer> mRenderer;
    void* mAppCtx;
};

/**
 * This class represent a generic instance to manage removte video webrtc Transceiver
 */
class RemoteVideoSlot : public RemoteSlot, public VideoSink
{
public:
    RemoteVideoSlot(Call& call, rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver, void* appCtx);
    ~RemoteVideoSlot();
    void enableTrack();
    void assignVideoSlot(Cid_t cid, IvStatic_t iv, VideoResolution videoResolution);
    void release() override;
    VideoResolution getVideoResolution() const;
    bool hasTrack();

private:
    VideoResolution mVideoResolution = kUndefined;
};

/**
 * This class represent a generic instance to manage remote audio webrtc Transceiver
 */
class RemoteAudioSlot : public RemoteSlot
{
public:
    RemoteAudioSlot(Call& call, rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver, void* appCtx);
    void assignAudioSlot(Cid_t cid, IvStatic_t iv);
    void enableAudioMonitor(bool enable);
    void createDecryptor(Cid_t cid, IvStatic_t iv) override;
    void release() override;

private:
    std::unique_ptr<AudioLevelMonitor> mAudioLevelMonitor;
    bool mAudioLevelMonitorEnabled = false;
};

/**
 * @brief The Session class
 *
 * A session is used to manage the slots available for a peer
 * in a all. It implements the ISession interface, and provides
 * callbacks through the SessionHandler.
 *
 * The Session itself is created right before the CallHandler::onNewSession()
 */
class Session : public ISession
{
public:
    Session(const sfu::Peer& peer);
    ~Session();

    const sfu::Peer &getPeer() const;
    void setVThumSlot(RemoteVideoSlot* slot);
    void setHiResSlot(RemoteVideoSlot* slot);
    void setAudioSlot(RemoteAudioSlot *slot);
    void addKey(Keyid_t keyid, const std::string& key);
    void setAvFlags(karere::AvFlags flags);

    RemoteAudioSlot* getAudioSlot();
    RemoteVideoSlot* getVthumSlot();
    RemoteVideoSlot* getHiResSlot();

    void disableAudioSlot();
    void setSpeakRequested(bool requested);
    void setAudioDetected(bool audioDetected);    
    void notifyHiResReceived();
    void notifyLowResReceived();
    void disableVideoSlot(VideoResolution videoResolution);

    // ISession methods (called from intermediate layer, upon SessionHandler callbacks and others)
    karere::Id getPeerid() const override;
    Cid_t getClientid() const override;
    SessionState getState() const override;
    karere::AvFlags getAvFlags() const override;
    bool isAudioDetected() const override;
    bool hasRequestSpeak() const override;
    void setSessionHandler(SessionHandler* sessionHandler) override;
    void setVideoRendererVthumb(IVideoRenderer *videoRenderer) override;
    void setVideoRendererHiRes(IVideoRenderer *videoRenderer) override;
    bool hasHighResolutionTrack() const override;
    bool hasLowResolutionTrack() const override;

private:
    // Data about the partipant in the call relative to this session
    sfu::Peer mPeer;

    // ---- SLOTs -----
    // Ownership is kept by the Call

    RemoteVideoSlot* mVthumSlot = nullptr;
    RemoteVideoSlot* mHiresSlot = nullptr;
    RemoteAudioSlot* mAudioSlot = nullptr;

    // To notify events about the session to the app (intermediate layer)
    std::unique_ptr<SessionHandler> mSessionHandler = nullptr;

    bool mHasRequestSpeak = false;
    bool mAudioDetected = false;

    // Session starts directly in progress: the SFU sends the tracks immediately from new peer
    SessionState mState = kSessStateInProgress;
};

/**
 * @brief Configure scalable video coding based on webrtc stats
 *
 * It's only applied to high resolution video
 */
class SvcDriver
{
public:
    static const uint8_t kMaxQualityIndex = 6;
    static const int kMinTimeBetweenSwitches = 6;   // minimum period between SVC switches in seconds

    // boundaries for switching to lower/higher quality.
    // if rtt moving average goes outside of these boundaries, switching occurs.
    static const int kRttLowerHeadroom = 30;
    static const int kRttUpperHeadroom = 250;

    SvcDriver();
    bool setSvcLayer(int8_t delta, int8_t &rxSpt, int8_t &rxTmp, int8_t &rxStmp, int8_t &txSpt);
    uint8_t mCurrentSvcLayerIndex;

    double mPacketLostLower;
    double mPacketLostUpper;
    double mPacketLostCapping;
    double mLowestRttSeen;
    double mRttLower;
    double mRttUpper;
    double mMovingAverageRtt;
    double mMovingAveragePlost;
    time_t mTsLastSwitch;
};

/**
* @brief The Call class
*
* This object is created upon OP_JOINEDCALL (or OP_CALLSTATE).
* It implements ICall interface for the intermediate layer.
*/
class Call : public karere::DeleteTrackable, public sfu::SfuInterface, public ICall
{
public:
    enum SpeakerState
    {
        kNoSpeaker = 0,
        kPending = 1,
        kActive = 2,
    };

    Call(karere::Id callid, karere::Id chatid, karere::Id callerid, bool isRinging, CallHandler& callHandler, MyMegaApi& megaApi, RtcModuleSfu& rtc, bool isGroup, std::shared_ptr<std::string> callKey = nullptr, karere::AvFlags avflags = 0);
    virtual ~Call();


    // ---- ICall methods ----
    //
    karere::Id getChatid() const override;
    karere::Id getCallerid() const override;
    CallState getState() const override;
    // returns true if your user participates of the call
    bool participate() override;
    bool isJoining() const override;
    bool hasVideoSlot(Cid_t cid, bool highRes = true) const override;
    int getNetworkQuality() const override;
    TermCode getTermCode() const override;
    uint8_t getEndCallReason() const override;

    // called upon reception of OP_JOINEDCALL from chatd
    void joinedCallUpdateParticipants(const std::set<karere::Id> &usersJoined) override;
    // called upon reception of OP_LEFTCALL from chatd
    void removeParticipant(karere::Id peer) override;
    // check if our peer is participating in the call (called from chatd)
    bool isOtherClientParticipating() override;

    // called from chatd::onDisconnect() to remove peers from the call when disconnected from chatd
    void onDisconnectFromChatd() override;
    // called from chatd::setState(online) to reconnect to SFU
    void reconnectToSfu() override;

    promise::Promise<void> hangup() override;
    promise::Promise<void> endCall(int reason = chatd::kDefault) override;  // only used on 1on1 when incoming call is rejected
    promise::Promise<void> join(karere::AvFlags avFlags) override;

    // (for your own audio level)
    void enableAudioLevelMonitor(bool enable) override;
    bool isAudioLevelMonitorEnabled() const override;
    bool isAudioDetected() const override;

    // called when the user wants to "mute" an incoming call (the call is kept in ringing state)
    void ignoreCall() override;
    bool isIgnored() const override;

    void setRinging(bool ringing) override;
    bool isRinging() const override;    // (always false for outgoing calls)

    void setOnHold() override;
    void releaseOnHold() override;

    void setCallerId(karere::Id callerid) override;
    karere::Id getCallid() const override;

    // request to speak, or cancels a previous request (add = false)
    void requestSpeaker(bool add = true) override;
    bool hasRequestSpeak() const override;

    // get the list of users that have requested to speak
    std::vector<Cid_t> getSpeakerRequested() override;

    // allows to approve/deny requests to speak from other users (only allowed for moderators)
    void approveSpeakRequest(Cid_t cid, bool allow) override;
    bool isSpeakAllow() const override; // true if request has been approved
    void stopSpeak(Cid_t cid = 0) override; // after been approved

    void requestHighResolutionVideo(Cid_t cid, int quality) override;
    void stopHighResolutionVideo(std::vector<Cid_t> &cids) override;

    void requestLowResolutionVideo(std::vector<Cid_t> &cids) override;
    void stopLowResolutionVideo(std::vector<Cid_t> &cids) override;

    // ask the SFU to get higher/lower (spatial) quality of HighRes video (thanks to SVC), on demand by the app
    void requestHiResQuality(Cid_t cid, int quality) override;

    std::set<karere::Id> getParticipants() const override;
    std::vector<Cid_t> getSessionsCids() const override;
    ISession* getIsession(Cid_t cid) const override;

    bool isOutgoing() const override;   // true if your user started the call

    int64_t getInitialTimeStamp() const override;
    int64_t getFinalTimeStamp() const override;
    int64_t getInitialOffset() const override;

    karere::AvFlags getLocalAvFlags() const override;
    void updateAndSendLocalAvFlags(karere::AvFlags flags) override;
    void setAudioDetected(bool audioDetected) override;

    //
    // ------ end ICall methods -----


    Session* getSession(Cid_t cid);
    std::set<Cid_t> getSessionsCidsByUserHandle(const karere::Id& id);
    void setState(CallState newState);
    static const char *stateToStr(CallState state);

    bool connectSfu(const std::string& sfuUrlStr);
    void joinSfu();

    void createTransceivers(size_t &hiresTrackIndex);  // both, for sending your audio/video and for receiving from participants
    void getLocalStreams(); // update video and audio tracks based on AV flags and call state (on-hold)

    void disconnect(TermCode termCode, const std::string& msg, bool removeParticipants);
    void handleCallDisconnect(const TermCode &termCode);
    void setEndCallReason(uint8_t reason);
    std::string endCallReasonToString(const EndCallReason &reason) const;
    std::string connectionTermCodeToString(const TermCode &termcode) const;
    bool isValidConnectionTermcode(TermCode termCode) const;
    void sendStats(const TermCode& termCode);

    std::string getKeyFromPeer(Cid_t cid, Keyid_t keyid);
    bool hasCallKey();
    sfu::Peer &getMyPeer();
    sfu::SfuClient& getSfuClient();
    std::map<Cid_t, std::unique_ptr<Session>>& getSessions();
    void takeVideoDevice();
    void releaseVideoDevice();
    bool hasVideoDevice();
    void freeVideoTracks(bool releaseSlots = false);
    void freeAudioTrack(bool releaseSlot = false);
    // enable/disable video tracks depending on the video's flag and the call on-hold
    void updateVideoTracks();

    // --- SfuInterface methods ---
    bool handleAvCommand(Cid_t cid, unsigned av) override;
    bool handleAnswerCommand(Cid_t cid, sfu::Sdp &spd, uint64_t ts, const std::vector<sfu::Peer>&peers, const std::map<Cid_t, sfu::TrackDescriptor> &vthumbs, const std::map<Cid_t, sfu::TrackDescriptor> &speakers) override;
    bool handleKeyCommand(Keyid_t keyid, Cid_t cid, const std::string& key) override;
    bool handleVThumbsCommand(const std::map<Cid_t, sfu::TrackDescriptor> &videoTrackDescriptors) override;
    bool handleVThumbsStartCommand() override;
    bool handleVThumbsStopCommand() override;
    bool handleHiResCommand(const std::map<Cid_t, sfu::TrackDescriptor> &videoTrackDescriptors) override;
    bool handleHiResStartCommand() override;
    bool handleHiResStopCommand() override;
    bool handleSpeakReqsCommand(const std::vector<Cid_t> &speakRequests) override;
    bool handleSpeakReqDelCommand(Cid_t cid) override;
    bool handleSpeakOnCommand(Cid_t cid, sfu::TrackDescriptor speaker) override;
    bool handleSpeakOffCommand(Cid_t cid) override;
    bool handlePeerJoin(Cid_t cid, uint64_t userid, int av) override;
    bool handlePeerLeft(Cid_t cid) override;
    void onSfuConnected() override;
    void onSfuDisconnected() override;

    bool error(unsigned int code, const std::string& errMsg) override;
    void logError(const char* error) override;

    // PeerConnectionInterface events
    void onAddStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> stream);
    void onTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver);
    void onRemoveTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver);
    void onConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState newState);

protected:
    /* if we are connected to chatd, this participant list will be managed exclusively by meetings related chatd commands
     * if we are disconnected from chatd (chatd connectivity lost), but still participating in a call, peerleft and peerjoin SFU commands
     * could also add/remove participants to this list, in order to keep participants up to date */
    std::set<karere::Id> mParticipants;
    karere::Id mCallid;
    karere::Id mChatid;
    karere::Id mCallerId;
    CallState mState = CallState::kStateInitial;
    bool mIsRinging = false;
    bool mIgnored = false;

    // this flag indicates if we are reconnecting to chatd or not, in order to update mParticipants from chatd or SFU (in case we have lost chatd connectivity)
    bool mIsReconnectingToChatd = false;

    // state of request to speak for own user in this call
    SpeakerState mSpeakerState = SpeakerState::kPending;

    int64_t mInitialTs = 0; // when we joined the call
    int64_t mOffset = 0;    // duration of call when we joined
    int64_t mFinalTs = 0;   // end of the call
    bool mAudioDetected = false;

    // timer to check stats in order to detect local audio level (for remote audio level, audio monitor does it)
    megaHandle mVoiceDetectionTimer = 0;

    int mNetworkQuality = kNetworkQualityDefault;
    bool mIsGroup = false;
    TermCode mTermCode = kInvalidTermCode;
    uint8_t mEndCallReason = kInvalidReason;

    CallHandler& mCallHandler;
    MyMegaApi& mMegaApi;
    sfu::SfuClient& mSfuClient;
    sfu::SfuConnection* mSfuConnection = nullptr;   // owned by the SfuClient::mConnections, here for convenience

    artc::MyPeerConnection<Call> mRtcConn;
    std::string mSdpStr;   // session description provided by WebRTC::createOffer()
    std::unique_ptr<LocalSlot> mAudio;
    std::unique_ptr<LocalSlot> mVThumb;
    bool mVThumbActive = false;  // true when sending low res video
    std::unique_ptr<LocalSlot> mHiRes;
    bool mHiResActive = false;  // true when sending high res video
    std::map<uint32_t, std::unique_ptr<RemoteSlot>> mReceiverTracks;  // maps 'mid' to 'Slot'
    std::map<Cid_t, std::unique_ptr<Session>> mSessions;
    std::unique_ptr<sfu::Peer> mMyPeer;
    uint8_t mMaxPeers = 0; // maximum number of peers (excluding yourself), seen throughout the call

    // call key for public chats (128-bit key)
    std::string mCallKey;

    // this flag prevents that we start multiple joining attempts for a call
    bool mIsJoining;
    RtcModuleSfu& mRtc;
    artc::VideoManager* mVideoManager = nullptr;

    megaHandle mStatsTimer = 0;
    rtc::scoped_refptr<webrtc::RTCStatsCollectorCallback> mStatConnCallback;
    Stats mStats;
    SvcDriver mSvcDriver;
    Keyid_t generateNextKeyId();
    void generateAndSendNewkey(bool reset = false);
    // associate slots with their corresponding sessions (video)
    void handleIncomingVideo(const std::map<Cid_t, sfu::TrackDescriptor> &videotrackDescriptors, VideoResolution videoResolution);
    // associate slots with their corresponding sessions (audio)
    void addSpeaker(Cid_t cid, const sfu::TrackDescriptor &speaker);
    void removeSpeaker(Cid_t cid);
    const std::string &getCallKey() const;
    // enable/disable audio track depending on the audio's flag, the speaker is allowed and the call on-hold
    void updateAudioTracks();
    void attachSlotToSession (Cid_t cid, RemoteSlot *slot, bool audio, VideoResolution hiRes);
    void initStatsValues();
    void enableStats();
    void disableStats();
    void adjustSvcByStats();
    void collectNonRTCStats();
    // ask the SFU to get higher/lower (spatial + temporal) quality of HighRes video (thanks to SVC), automatically due to network quality
    void updateSvcQuality(int8_t delta);
    void resetLocalAvFlags();
    bool isDisconnectionTermcode(const TermCode& termCode) const;
};

class RtcModuleSfu : public RtcModule, public VideoSink
{
public:
    RtcModuleSfu(MyMegaApi &megaApi, CallHandler &callhandler, DNScache &dnsCache,
                 WebsocketsIO& websocketIO, void *appCtx,
                 rtcModule::RtcCryptoMeetings* rRtcCryptoMeetings);
    ICall* findCall(karere::Id callid) override;
    ICall* findCallByChatid(const karere::Id &chatid) override;
    bool isCallStartInProgress(const karere::Id &chatid) const override;
    bool selectVideoInDevice(const std::string& device) override;
    void getVideoInDevices(std::set<std::string>& devicesVector) override;
    promise::Promise<void> startCall(karere::Id chatid, karere::AvFlags avFlags, bool isGroup, std::shared_ptr<std::string> unifiedKey = nullptr) override;
    void takeDevice() override;
    void releaseDevice() override;
    void addLocalVideoRenderer(karere::Id chatid, IVideoRenderer *videoRederer) override;
    void removeLocalVideoRenderer(karere::Id chatid) override;

    std::vector<karere::Id> chatsWithCall() override;
    unsigned int getNumCalls() override;
    const std::string& getVideoDeviceSelected() const override;
    sfu::SfuClient& getSfuClient() override;
    DNScache& getDnsCache() override;

    void removeCall(karere::Id chatid, EndCallReason reason, TermCode connectionTermCode) override;

    void handleJoinedCall(karere::Id chatid, karere::Id callid, const std::set<karere::Id>& usersJoined) override;
    void handleLeftCall(karere::Id chatid, karere::Id callid, const std::set<karere::Id>& usersLeft) override;
    void handleNewCall(karere::Id chatid, karere::Id callerid, karere::Id callid, bool isRinging, bool isGroup, std::shared_ptr<std::string> callKey = nullptr) override;

    void OnFrame(const webrtc::VideoFrame& frame) override;

    artc::VideoManager* getVideoDevice();
    void changeDevice(const std::string& device, bool shouldOpen);
    void openDevice();
    void closeDevice();

    void* getAppCtx();
    std::string getDeviceInfo() const;

private:
    std::map<karere::Id, std::unique_ptr<Call>> mCalls;
    CallHandler& mCallHandler;
    MyMegaApi& mMegaApi;
    DNScache &mDnsCache;
    std::unique_ptr<sfu::SfuClient> mSfuClient;
    std::string mVideoDeviceSelected;
    rtc::scoped_refptr<artc::VideoManager> mVideoDevice;
    // count of times the device has been taken (without being released)
    unsigned int mDeviceTakenCount = 0;
    std::map<karere::Id, std::unique_ptr<IVideoRenderer>> mRenderers;
    std::map<karere::Id, VideoSink> mVideoSink;
    void* mAppCtx = nullptr;
    std::set<karere::Id> mCallStartAttempts;
};

void globalCleanup();

#endif
}


#endif // WEBRTCSFU_H
