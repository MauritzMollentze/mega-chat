#ifndef KARERE_DISABLE_WEBRTC
#include <mega/types.h>
#include <mega/base64.h>
#include <rtcmPrivate.h>
#include <webrtcPrivate.h>
#include <api/video/i420_buffer.h>
#include <libyuv/convert.h>

namespace rtcModule
{
SvcDriver::SvcDriver ()
    : mCurrentSvcLayerIndex(kMaxQualityIndex), // by default max quality
      mPacketLostLower(0.01),
      mPacketLostUpper(1),
      mLowestRttSeen(10000),
      mRttLower(0),
      mRttUpper(0),
      mMovingAverageRtt(0),
      mMovingAveragePlost(0),
      mTsLastSwitch(0)
{

}

bool SvcDriver::setSvcLayer(int8_t delta, int8_t& rxSpt, int8_t& rxTmp, int8_t& rxStmp, int8_t& txSpt)
{
    int8_t newSvcLayerIndex = static_cast<int8_t>(mCurrentSvcLayerIndex + delta);
    if (newSvcLayerIndex < 0 || newSvcLayerIndex > kMaxQualityIndex)
    {
        return false;
    }

    RTCM_LOG_WARNING("setSvcLayer: Switching SVC layer from %d to %d", mCurrentSvcLayerIndex, newSvcLayerIndex);
    mTsLastSwitch = time(nullptr); // update last Ts SVC switch
    mCurrentSvcLayerIndex = static_cast<uint8_t>(newSvcLayerIndex);

    // we want to provide a linear quality scale,
    // layers are defined for each of the 7 "quality" steps
    // layer: rxSpatial (resolution), rxTemporal (FPS), rxScreenTemporal (for screen video), txSpatial (resolution)
    switch (mCurrentSvcLayerIndex)
    {
        case 0: { rxSpt = 0; rxTmp = 0; rxStmp = 0; txSpt = 0; return true; }
        case 1: { rxSpt = 0; rxTmp = 1; rxStmp = 0; txSpt = 0; return true; }
        case 2: { rxSpt = 0; rxTmp = 2; rxStmp = 0; txSpt = 1; return true; }
        case 3: { rxSpt = 1; rxTmp = 1; rxStmp = 0; txSpt = 1; return true; }
        case 4: { rxSpt = 1; rxTmp = 2; rxStmp = 1; txSpt = 1; return true; }
        case 5: { rxSpt = 2; rxTmp = 1; rxStmp = 1; txSpt = 2; return true; }
        case 6: { rxSpt = 2; rxTmp = 2; rxStmp = 2; txSpt = 2; return true; }
        default: return false;
    }
}

Call::Call(karere::Id callid, karere::Id chatid, karere::Id callerid, bool isRinging, CallHandler& callHandler, MyMegaApi& megaApi, RtcModuleSfu& rtc, bool isGroup, std::shared_ptr<std::string> callKey, karere::AvFlags avflags)
    : mCallid(callid)
    , mChatid(chatid)
    , mCallerId(callerid)
    , mIsRinging(isRinging)
    , mIsGroup(isGroup)
    , mCallHandler(callHandler) // CallHandler to receive notifications about the call
    , mMegaApi(megaApi)
    , mSfuClient(rtc.getSfuClient())
    , mCallKey(callKey ? *callKey : std::string())
    , mIsJoining(false)
    , mRtc(rtc)
{
    std::unique_ptr<char []> userHandle(mMegaApi.sdk.getMyUserHandle());
    karere::Id myUserHandle(userHandle.get());
    mMyPeer.reset(new sfu::Peer(myUserHandle, avflags.value()));
    setState(kStateInitial); // call after onNewCall, otherwise callhandler didn't exists
}

Call::~Call()
{
    disableStats();

    if (mTermCode == kInvalidTermCode)
    {
        mTermCode = kUnKnownTermCode;
    }

    setState(CallState::kStateDestroyed);
}

karere::Id Call::getCallid() const
{
    return mCallid;
}

karere::Id Call::getChatid() const
{
    return mChatid;
}

karere::Id Call::getCallerid() const
{
    return mCallerId;
}

bool Call::isAudioDetected() const
{
    return mAudioDetected;
}

void Call::setState(CallState newState)
{
    RTCM_LOG_DEBUG("Call state changed. ChatId: %s, callid: %s, state: %s --> %s",
                 karere::Id(getChatid()).toString().c_str(),
                 karere::Id(getCallid()).toString().c_str(),
                 Call::stateToStr(mState),
                 Call::stateToStr(newState));

    if (newState == CallState::kStateInProgress)
    {
        // initial ts is set when user has joined to the call
        mInitialTs = time(nullptr);
    }
    else if (newState == CallState::kStateDestroyed)
    {
        mFinalTs = time(nullptr);
    }

    mState = newState;
    mCallHandler.onCallStateChange(*this);
}

CallState Call::getState() const
{
    return mState;
}

void Call::joinedCallUpdateParticipants(const std::set<karere::Id> &usersJoined)
{
    if (usersJoined.find(mMyPeer->getPeerid()) != usersJoined.end())
    {
        setRinging(false);
    }

    if (!mIsReconnectingToChatd)
    {
        for (const karere::Id &peer : usersJoined)
        {
            // if we haven't experimented a chatd connection lost (mIsConnectedToChatd == true) just add received peers
            mParticipants.insert(peer);
            mCallHandler.onAddPeer(*this, peer);
        }
    }
    else
    {
        for (const karere::Id &recvPeer : usersJoined)
        {
            if (mParticipants.find(recvPeer) == mParticipants.end())
            {
                // add new participant received at OP_JOINEDCALL
                mParticipants.insert(recvPeer);
                mCallHandler.onAddPeer(*this, recvPeer);
            }
        }

        for (const karere::Id &peer : mParticipants)
        {
            if (usersJoined.find(peer) == usersJoined.end())
            {
                // remove participant from mParticipants, not present at list received at OP_JOINEDCALL
                mParticipants.erase(peer);
                mCallHandler.onRemovePeer(*this, peer);
            }
        }

        mIsReconnectingToChatd = false; // we can assume that we are connected to chatd, and our participants list is up to date
    }
}

void Call::onDisconnectFromChatd()
{
    if (!participate())
    {
        // if we don't participate in a meeting, and we are disconnected from chatd, we need to clear participants
        for (auto &it : mParticipants)
        {
            mCallHandler.onRemovePeer(*this, it);
        }
        mParticipants.clear();
    }

    mIsReconnectingToChatd = true;
}

void Call::reconnectToSfu()
{
    mSfuConnection->retryPendingConnection(true);
}

void Call::removeParticipant(karere::Id peer)
{
    for (auto itPeer = mParticipants.begin(); itPeer != mParticipants.end(); itPeer++)
    {
        if (*itPeer == peer)
        {
            mParticipants.erase(itPeer);
            mCallHandler.onRemovePeer(*this, peer);
            return;
        }
    }

    assert(false);
    return;
}

bool Call::isOtherClientParticipating()
{
    for (auto& peerid : mParticipants)
    {
        if (peerid == mMyPeer->getPeerid())
        {
            return true;
        }
    }

    return false;
}

// for the moment just chatd::kRejected is a valid reason (only for rejecting 1on1 call while ringing)
promise::Promise<void> Call::endCall(int reason)
{
    return mMegaApi.call(&::mega::MegaApi::endChatCall, mChatid, mCallid, reason)
    .then([](ReqResult /*result*/)
    {
    });
}

promise::Promise<void> Call::hangup()
{
    if (!isOtherClientParticipating() && mState == kStateClientNoParticipating && mIsRinging && !mIsGroup)
    {
        // in 1on1 calls, the hangup (reject) by the user while ringing should end the call
        return endCall(chatd::kRejected); // reject 1on1 call while ringing
    }
    else
    {
        onCallDisconnect(TermCode::kUserHangup, "normal user hangup", true, mIsReconnectingToChatd);
        return promise::_Void();
    }
}

promise::Promise<void> Call::join(karere::AvFlags avFlags)
{
    mIsJoining = true; // set flag true to avoid multiple join call attempts
    mMyPeer->setAvFlags(avFlags);
    auto wptr = weakHandle();
    return mMegaApi.call(&::mega::MegaApi::joinChatCall, mChatid.val, mCallid.val)
    .then([wptr, this](ReqResult result) -> promise::Promise<void>
    {
        if (wptr.deleted())
            return promise::Error("Join call succeed, but call has already ended");

        std::string sfuUrlStr = result->getText();
        mIsJoining = false;

        if (!connectSfu(sfuUrlStr))
        {
           return promise::Error("connectSfu error, invalid or empty sfu URL");
        }
        else
        {
           return promise::_Void();
        }
    })
    .fail([wptr, this](const ::promise::Error& err)
    {
        if (!wptr.deleted())
            return promise::Error("Join call failed, and call has already ended");

        mIsJoining = false;
        return err;
    });
}

bool Call::participate()
{
    return (mState > kStateClientNoParticipating && mState < kStateTerminatingUserParticipation);
}

bool Call::isJoining() const
{
    return mIsJoining;
}

void Call::enableAudioLevelMonitor(bool enable)
{
    if ( (enable && mVoiceDetectionTimer != 0)          // already enabled
        || (!enable && mVoiceDetectionTimer == 0) )     // already disabled
    {
        return;
    }

    RTCM_LOG_DEBUG("Audio level monitor %s", enable ? "enabled" : "disabled");

    if (enable)
    {
        mAudioDetected = false;
        auto wptr = weakHandle();
        mVoiceDetectionTimer = karere::setInterval([this, wptr]()
        {
            if (wptr.deleted())
                return;

            webrtc::AudioProcessingStats audioStats = artc::gAudioProcessing->GetStatistics(false);

            if (audioStats.voice_detected && mAudioDetected != audioStats.voice_detected.value())
            {
                setAudioDetected(audioStats.voice_detected.value());
            }
        }, kAudioMonitorTimeout, mRtc.getAppCtx());
    }
    else
    {
        setAudioDetected(false);
        karere::cancelInterval(mVoiceDetectionTimer, mRtc.getAppCtx());
        mVoiceDetectionTimer = 0;
    }
}

void Call::ignoreCall()
{
    mIgnored = true;
}

void Call::setRinging(bool ringing)
{
    if (mIsRinging != ringing)
    {
        mIsRinging = ringing;
        mCallHandler.onCallRinging(*this);
    }
}

void Call::setOnHold()
{
    // disable audio track
    if (mAudio && mAudio->getTransceiver()->sender()->track())
    {
        mAudio->getTransceiver()->sender()->SetTrack(nullptr);
    }

    // disable hi-res track
    if (mHiRes && mHiRes->getTransceiver()->sender()->track())
    {
        mHiRes->getTransceiver()->sender()->SetTrack(nullptr);
    }

    // disable low-res track
    if (mVThumb && mVThumb->getTransceiver()->sender()->track())
    {
        mVThumb->getTransceiver()->sender()->SetTrack(nullptr);
    }

    // release video device
    releaseVideoDevice();
}

void Call::releaseOnHold()
{
    updateAudioTracks();
    updateVideoTracks();
}

bool Call::isIgnored() const
{
    return mIgnored;
}

bool Call::isAudioLevelMonitorEnabled() const
{
    return mVoiceDetectionTimer;
}

bool Call::hasVideoSlot(Cid_t cid, bool highRes) const
{
    for (const auto& session : mSessions)
    {
        RemoteSlot *slot = highRes
                ? session.second->getHiResSlot()
                : session.second->getVthumSlot();

        if (slot && slot->getCid() == cid)
        {
            return true;
        }
    }
    return false;
}

int Call::getNetworkQuality() const
{
    return mNetworkQuality;
}

bool Call::hasRequestSpeak() const
{
    return mSpeakerState == SpeakerState::kPending;
}

TermCode Call::getTermCode() const
{
    return mTermCode;
}

uint8_t Call::getEndCallReason() const
{
    return mEndCallReason;
}

void Call::setCallerId(karere::Id callerid)
{
    mCallerId  = callerid;
}

bool Call::isRinging() const
{
    return mIsRinging;
}

bool Call::isOutgoing() const
{
    return mCallerId == mMyPeer->getPeerid();
}

int64_t Call::getInitialTimeStamp() const
{
    return mInitialTs;
}

int64_t Call::getFinalTimeStamp() const
{
    return mFinalTs;
}

int64_t Call::getInitialOffset() const
{
    return mOffset;
}

const char *Call::stateToStr(CallState state)
{
    switch(state)
    {
        RET_ENUM_RTC_NAME(kStateInitial);
        RET_ENUM_RTC_NAME(kStateClientNoParticipating);
        RET_ENUM_RTC_NAME(kStateConnecting);
        RET_ENUM_RTC_NAME(kStateJoining);    // < Joining a call
        RET_ENUM_RTC_NAME(kStateInProgress);
        RET_ENUM_RTC_NAME(kStateTerminatingUserParticipation);
        RET_ENUM_RTC_NAME(kStateDestroyed);
        default: return "(invalid call state)";
    }
}

karere::AvFlags Call::getLocalAvFlags() const
{
    return mMyPeer->getAvFlags();
}

void Call::updateAndSendLocalAvFlags(karere::AvFlags flags)
{
    if (flags == getLocalAvFlags())
    {
        RTCM_LOG_WARNING("updateAndSendLocalAvFlags: AV flags has not changed");
        return;
    }

    // update and send local AV flags
    karere::AvFlags oldFlags = getLocalAvFlags();
    mMyPeer->setAvFlags(flags);
    mSfuConnection->sendAv(flags.value());

    if (oldFlags.isOnHold() != flags.isOnHold())
    {
        // kOnHold flag has changed
        (flags.isOnHold())
                ? setOnHold()
                : releaseOnHold();

        mCallHandler.onOnHold(*this); // notify app onHold Change
    }
    else
    {
        updateAudioTracks();
        updateVideoTracks();
        mCallHandler.onLocalFlagsChanged(*this);  // notify app local AvFlags Change
    }
}

void Call::setAudioDetected(bool audioDetected)
{
    mAudioDetected = audioDetected;
    mCallHandler.onLocalAudioDetected(*this);
}

void Call::requestSpeaker(bool add)
{
    if (mSpeakerState == SpeakerState::kNoSpeaker && add)
    {
        mSpeakerState = SpeakerState::kPending;
        mSfuConnection->sendSpeakReq();
        return;
    }

    if (mSpeakerState == SpeakerState::kPending && !add)
    {
        mSpeakerState = SpeakerState::kNoSpeaker;
        mSfuConnection->sendSpeakReqDel();
        return;
    }
}

bool Call::isSpeakAllow() const
{
    return mSpeakerState == SpeakerState::kActive && getLocalAvFlags().audio();
}

void Call::approveSpeakRequest(Cid_t cid, bool allow)
{
    if (allow)
    {
        mSfuConnection->sendSpeakReq(cid);
    }
    else
    {
        mSfuConnection->sendSpeakReqDel(cid);
    }
}

void Call::stopSpeak(Cid_t cid)
{
    if (cid)
    {
        assert(mSessions.find(cid) != mSessions.end());
        mSfuConnection->sendSpeakDel(cid);
        return;
    }

    mSfuConnection->sendSpeakDel();
}

std::vector<Cid_t> Call::getSpeakerRequested()
{
    std::vector<Cid_t> speakerRequested;

    for (const auto& session : mSessions)
    {
        if (session.second->hasRequestSpeak())
        {
            speakerRequested.push_back(session.first);
        }
    }

    return speakerRequested;
}

void Call::requestHighResolutionVideo(Cid_t cid, int quality)
{
    Session *sess= getSession(cid);
    if (!sess)
    {
        RTCM_LOG_DEBUG("requestHighResolutionVideo: session not found for %d", cid);
        return;
    }

    if (quality < kCallQualityHighDef || quality > kCallQualityHighLow)
    {
        RTCM_LOG_WARNING("requestHighResolutionVideo: invalid resolution divider value (spatial layer offset): %d", quality);
        return;
    }

    if (sess->hasHighResolutionTrack())
    {
        RTCM_LOG_WARNING("High res video requested, but already available");
        sess->notifyHiResReceived();
    }
    else
    {
        mSfuConnection->sendGetHiRes(cid, hasVideoSlot(cid, false) ? 1 : 0, quality);
    }
}

void Call::requestHiResQuality(Cid_t cid, int quality)
{
    if (!hasVideoSlot(cid, true))
    {
        RTCM_LOG_WARNING("requestHiResQuality: Currently not receiving a hi-res stream for this peer");
        return;
    }

    if (quality < kCallQualityHighDef || quality > kCallQualityHighLow)
    {
        RTCM_LOG_WARNING("requestHiResQuality: invalid resolution divider value (spatial layer offset).");
        return;
    }

    mSfuConnection->sendHiResSetLo(cid, quality);
}

void Call::stopHighResolutionVideo(std::vector<Cid_t> &cids)
{
    for (auto it = cids.begin(); it != cids.end();)
    {
        auto auxit = it++;
        Session *sess= getSession(*auxit);
        if (!sess)
        {
            RTCM_LOG_DEBUG("stopHighResolutionVideo: session not found for %d", *auxit);
            it = cids.erase(auxit);
        }
        else if (!sess->hasHighResolutionTrack())
        {
            RTCM_LOG_WARNING("stopHighResolutionVideo: high resolution already not available for cid: %d", *auxit);
            it = cids.erase(auxit);
            sess->notifyHiResReceived();    // also used to notify there's no video anymore
        }
    }
    if (!cids.empty())
    {
        for (auto cid: cids)
        {
            Session *sess= getSession(cid);
            sess->disableVideoSlot(kHiRes);
        }

        mSfuConnection->sendDelHiRes(cids);
    }
}

void Call::requestLowResolutionVideo(std::vector<Cid_t> &cids)
{
    for (auto it = cids.begin(); it != cids.end();)
    {
        auto auxit = it++;
        Session *sess= getSession(*auxit);
        if (!sess)
        {
            // remove cid that has no active session
            RTCM_LOG_DEBUG("requestLowResolutionVideo: session not found for cid: %d", *auxit);
            it = cids.erase(auxit);
        }
        else if (sess->hasLowResolutionTrack())
        {
            RTCM_LOG_WARNING("requestLowResolutionVideo: low resolution already available for cid: %d", *auxit);
            it = cids.erase(auxit);
            sess->notifyLowResReceived();
        }
    }
    if (!cids.empty())
    {
        mSfuConnection->sendGetVtumbs(cids);
    }
}

void Call::stopLowResolutionVideo(std::vector<Cid_t> &cids)
{
    for (auto it = cids.begin(); it != cids.end();)
    {
        auto auxit = it++;
        Session *sess= getSession(*auxit);
        if (!sess)
        {
            RTCM_LOG_DEBUG("stopLowResolutionVideo: session not found for cid: %d", *auxit);
            it = cids.erase(auxit);
        }
        else if (!sess->hasLowResolutionTrack())
        {
            RTCM_LOG_WARNING("stopLowResolutionVideo: low resolution already not available for cid: %d", *auxit);
            it = cids.erase(auxit);
            sess->notifyLowResReceived();
        }
    }
    if (!cids.empty())
    {
        for (auto cid: cids)
        {
            Session *sess= getSession(cid);
            sess->disableVideoSlot(kLowRes);
        }

        mSfuConnection->sendDelVthumbs(cids);
    }
}

void Call::updateSvcQuality(int8_t delta)
{
    // layer: rxSpatial (resolution), rxTemporal (FPS), rxScreenTemporal (for screen video), txSpatial (resolution)
    int8_t rxSpt = 0;
    int8_t rxTmp = 0;
    int8_t rxStmp = 0;
    int8_t txSpt = 0;

    // calculate new layer index from delta and retrieve layer components separately
    if (!mSvcDriver.setSvcLayer(delta, rxSpt, rxTmp, rxStmp, txSpt))
    {
        RTCM_LOG_WARNING("updateSvcQuality: Invalid new layer index %d", mSvcDriver.mCurrentSvcLayerIndex + delta);
        return;
    }

    // adjust Received SVC quality by sending LAYER command
    mSfuConnection->sendLayer(rxSpt, rxTmp, rxStmp);
}

std::set<karere::Id> Call::getParticipants() const
{
    return mParticipants;
}

std::vector<Cid_t> Call::getSessionsCids() const
{
    std::vector<Cid_t> returnedValue;

    for (const auto& sessionIt : mSessions)
    {
        returnedValue.push_back(sessionIt.first);
    }

    return returnedValue;
}

ISession* Call::getIsession(Cid_t cid) const
{
    auto it = mSessions.find(cid);
    return (it != mSessions.end())
        ? it->second.get()
        : nullptr;
}

Session* Call::getSession(Cid_t cid)
{
    auto it = mSessions.find(cid);
    return (it != mSessions.end())
        ? it->second.get()
        : nullptr;
}

std::set<Cid_t> Call::getSessionsCidsByUserHandle(const karere::Id& id)
{
    std::set<Cid_t> peers;
    for (const auto& session : mSessions)
    {
        if (session.second->getPeerid() == id)
        {
            peers.insert(session.first);
        }
    }
    return peers;
}

bool Call::connectSfu(const std::string& sfuUrlStr)
{
    if (sfuUrlStr.empty()) // if URL by param is empty, we must ensure that we already have a valid URL
    {
        RTCM_LOG_ERROR("trying to connect to SFU with an Empty URL");
        assert(false);
        return false;
    }

    karere::Url sfuUrl(sfuUrlStr);
    if (!sfuUrl.isValid())
    {
        RTCM_LOG_ERROR("trying to connect to SFU with an Empty Host");
        assert(sfuUrl.isValid());
        return false;
    }

    setState(CallState::kStateConnecting);
    if (!mRtc.getDnsCache().getRecordByHost(sfuUrl.host) && !mRtc.getDnsCache().addSfuRecord(sfuUrl.host))
    {
        RTCM_LOG_ERROR("connectSfu: can't retrieve nor add SFU record");
        assert(mRtc.getDnsCache().getRecordByHost(sfuUrl.host));
        return false;
    }

    mSfuConnection = mSfuClient.createSfuConnection(mChatid, std::move(sfuUrl), *this, mRtc.getDnsCache());
    return true;
}

void Call::joinSfu()
{
    initStatsValues();
    mRtcConn = artc::MyPeerConnection<Call>(*this, this->mRtc.getAppCtx());
    size_t hiresTrackIndex = 0;
    createTransceivers(hiresTrackIndex);
    mSpeakerState = SpeakerState::kPending;
    getLocalStreams();
    setState(CallState::kStateJoining);

    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    options.offer_to_receive_audio = webrtc::PeerConnectionInterface::RTCOfferAnswerOptions::kMaxOfferToReceiveMedia;
    options.offer_to_receive_video = webrtc::PeerConnectionInterface::RTCOfferAnswerOptions::kMaxOfferToReceiveMedia;
    auto wptr = weakHandle();
    mRtcConn.createOffer(options)
    .then([wptr, this, hiresTrackIndex](webrtc::SessionDescriptionInterface* sdp) -> promise::Promise<void>
    {
        if (wptr.deleted())
        {
            return ::promise::_Void();
        }

        if (mState != kStateJoining)
        {
            RTCM_LOG_WARNING("joinSfu: get unexpected state change at createOffer");
            assert(false); // theoretically, it should not happen. If so, it may worth to investigate
            return ::promise::_Void();
        }

        if (!mRtcConn)
        {
            assert(mState == kStateClientNoParticipating
                   || mState == kStateTerminatingUserParticipation);
            return ::promise::Error("Failure at initialization. Call destroyed or disconnect");
        }

        KR_THROW_IF_FALSE(sdp->ToString(&mSdpStr));
        sfu::Sdp mungedSdp(mSdpStr, static_cast<int64_t>(hiresTrackIndex)); // Create a Sdp instance from String and modify it to enable SVC
        std::string sdpUncompress = mungedSdp.unCompress(); // get string from modified Sdp instance

        webrtc::SdpParseError error;
        std::unique_ptr<webrtc::SessionDescriptionInterface> sdpInterface(webrtc::CreateSessionDescription(sdp->GetType(), sdpUncompress, &error));
        if (!sdpInterface)
        {
            onCallDisconnect(TermCode::kErrSdp, "Error parsing SDP offer: line= " + error.line +"  \nError: " + error.description, true, mIsReconnectingToChatd);
        }

        // update mSdpStr with modified SDP
        KR_THROW_IF_FALSE(sdpInterface->ToString(&mSdpStr));
        return mRtcConn.setLocalDescription(std::move(sdpInterface));   // takes onwership of sdp
    })
    .then([wptr, this]()
    {
        if (wptr.deleted())
        {
            return;
        }

        if (mState != kStateJoining)
        {
            RTCM_LOG_WARNING("joinSfu: get unexpected state change at setLocalDescription");
            return;
        }

        sfu::Sdp sdp(mSdpStr);
        std::map<std::string, std::string> ivs;
        ivs["0"] = sfu::Command::binaryToHex(mVThumb->getIv());
        ivs["1"] = sfu::Command::binaryToHex(mHiRes->getIv());
        ivs["2"] = sfu::Command::binaryToHex(mAudio->getIv());
        mSfuConnection->joinSfu(sdp, ivs, getLocalAvFlags().value(), mSpeakerState, kInitialvthumbCount);
    })
    .fail([wptr, this](const ::promise::Error& err)
    {
        if (wptr.deleted())
            return;

        onCallDisconnect(TermCode::kErrSdp, std::string("Error creating SDP offer: ") + err.msg(), true, mIsReconnectingToChatd);
    });
}

void Call::createTransceivers(size_t &hiresTrackIndex)
{
    assert(mRtcConn);

    // create your transceivers for sending (and receiving)
    webrtc::RtpTransceiverInit transceiverInitVThumb;
    transceiverInitVThumb.direction = webrtc::RtpTransceiverDirection::kSendRecv;
    webrtc::RTCErrorOr<rtc::scoped_refptr<webrtc::RtpTransceiverInterface>> err
            = mRtcConn->AddTransceiver(cricket::MediaType::MEDIA_TYPE_VIDEO, transceiverInitVThumb);
    mVThumb = ::mega::make_unique<LocalSlot>(*this, err.MoveValue());
    mVThumb->generateRandomIv();

    webrtc::RtpTransceiverInit transceiverInitHiRes;
    transceiverInitHiRes.direction = webrtc::RtpTransceiverDirection::kSendRecv;
    err = mRtcConn->AddTransceiver(cricket::MediaType::MEDIA_TYPE_VIDEO, transceiverInitHiRes);
    hiresTrackIndex = mRtcConn->GetTransceivers().size() - 1; // keep this sentence just after add transceiver for hiRes track
    mHiRes = ::mega::make_unique<LocalSlot>(*this, err.MoveValue());
    mHiRes->generateRandomIv();

    webrtc::RtpTransceiverInit transceiverInitAudio;
    transceiverInitAudio.direction = webrtc::RtpTransceiverDirection::kSendRecv;
    err = mRtcConn->AddTransceiver(cricket::MediaType::MEDIA_TYPE_AUDIO, transceiverInitAudio);
    mAudio = ::mega::make_unique<LocalSlot>(*this, err.MoveValue());
    mAudio->generateRandomIv();

    // create transceivers for receiving audio from peers
    for (int i = 1; i < RtcConstant::kMaxCallAudioSenders; i++)
    {
        webrtc::RtpTransceiverInit transceiverInit;
        transceiverInit.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
        mRtcConn->AddTransceiver(cricket::MediaType::MEDIA_TYPE_AUDIO, transceiverInit);
    }

    // create transceivers for receiving video from peers
    for (int i = 2; i < RtcConstant::kMaxCallVideoSenders; i++)
    {
        webrtc::RtpTransceiverInit transceiverInit;
        transceiverInit.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
        mRtcConn->AddTransceiver(cricket::MediaType::MEDIA_TYPE_VIDEO, transceiverInit);
    }
}

void Call::getLocalStreams()
{
    updateAudioTracks();
    if (getLocalAvFlags().videoCam())
    {
        updateVideoTracks();
    }
}


void Call::onCallDisconnect(TermCode termCode, const std::string &msg, bool isDefinitive, bool removeParticipants, bool avoidByeCommand)
{
    RTCM_LOG_DEBUG("onCallDisconnect, termcode: %s, msg: %s", connectionTermCodeToString(termCode).c_str(), msg.c_str());
    if (isDefinitive && removeParticipants)
    {
        // if we don't participate in a meeting, and we are disconnected from chatd, we need to clear participants
        // in case of SDP error and normal hangup
        for (auto &it : mParticipants)
        {
            mCallHandler.onRemovePeer(*this, it);
        }
        mParticipants.clear();
    }
    sendStats(termCode);
    if (termCode != kSigDisconn
            && mSfuConnection
            && mSfuConnection->isOnline()
            && !avoidByeCommand)
    {
        if (mSfuConnection->isSendingByeCommand())
        {
            RTCM_LOG_DEBUG("onCallDisconnect, already sending BYE command");
            return;
        }

        // we need to store termcode temporarily until confirm BYE command has been sent
        mTempTermCode = termCode;

        // once LWS confirms that BYE command has been sent (check processNextCommand) onSendByeCommand will be called
        mSfuConnection->sendBye(termCode, isDefinitive
                                    ? ::sfu::SfuConnection::kSfuDisconnect
                                    : ::sfu::SfuConnection::kSignalingDisconnect);
    }
    else
    {
        assert(termCode == kSigDisconn ? !isDefinitive : true);
        if (!isDefinitive)
        {
            signalingDisconnectAndClear(termCode);
            if (mSfuConnection && !mSfuConnection->isOnline())
            {
                // just reset disconnect attempt vars if not connected to sfu, to not interfere
                // with a previous disconnect attempt that also sent a BYE command
                mSfuConnection->resetDisconnectAttempt();
            }
        }
        else
        {
            callDisconnect(termCode);  // full disconnect (sfu & signaling), mSfuConnection will be destroyed
        }
    }
    return;
}

void Call::signalingDisconnectAndClear(const TermCode& termCode)
{
    RTCM_LOG_DEBUG("signalingDisconnectAndClear, termcode (%d): %s", termCode, connectionTermCodeToString(termCode).c_str());
    if (mRtcConn)
    {
        mRtcConn->Close();
        mRtcConn = nullptr;
    }

    disableStats();
    enableAudioLevelMonitor(false); // disable local audio level monitor
    mSessions.clear();              // session dtor will notify apps through onDestroySession callback
    mVThumb.reset();
    mHiRes.reset();
    mAudio.reset();
    mReceiverTracks.clear();        // clear receiver tracks after free sessions and audio/video local tracks
    if (!isDisconnectionTermcode(termCode))
    {
        resetLocalAvFlags();        // reset local AvFlags: Audio | Video | OnHold => disabled
    }
}

void Call::resetLocalAvFlags()
{
    mMyPeer->setAvFlags(karere::AvFlags::kEmpty);
}

void Call::setEndCallReason(uint8_t reason)
{
    mEndCallReason = reason;
}

std::string Call::endCallReasonToString(const EndCallReason &reason) const
{
    switch (reason)
    {
        case kEnded:            return "normal hangup of on-going call";
        case kRejected:         return "incoming call was rejected by callee";
        case kNoAnswer:         return "outgoing call didn't receive any answer from the callee";
        case kFailed:           return "on-going call failed";
        case kCancelled:        return "outgoing call was cancelled by caller before receiving any answer from the callee";
        case kInvalidReason:    return "invalid endcall reason";
    }
}

std::string Call::connectionTermCodeToString(const TermCode &termcode) const
{
    switch (termcode)
    {
        case kUserHangup:               return "normal user hangup";
        case kTooManyParticipants:      return "there are too many participants";
        case kLeavingRoom:              return "user has been removed from chatroom";
        case kRtcDisconn:               return "SFU connection failed";
        case kSigDisconn:               return "socket error on the signalling connection";
        case kSvrShuttingDown:          return "SFU server is shutting down";
        case kErrSignaling:             return "signalling error";
        case kErrNoCall:                return "attempted to join non-existing call";
        case kErrAuth:                  return "authentication error";
        case kErrApiTimeout:            return "ping timeout between SFU and API";
        case kErrSdp:                   return "error generating or setting SDP description";
        case kErrGeneral:               return "general error";
        case kChatDisconn:              return "chatd connection is broken";
        case kApiEndCall:               return "API/chatd ended call";
        case kUnKnownTermCode:          return "unknown error";
        default:                        return "invalid connection termcode";
    }
}

bool Call::isTermCodeRetriable(const TermCode& termCode) const
{
    return termCode == kRtcDisconn || termCode == kSigDisconn;
}

bool Call::isDisconnectionTermcode(const TermCode& termCode) const
{
    return termCode & kFlagDisconn;
}

bool Call::isValidConnectionTermcode(TermCode termCode) const
{
    return termCode >= kUserHangup && termCode <= kFlagMaxValid;
}

void Call::sendStats(const TermCode& termCode)
{
    assert(isValidConnectionTermcode(termCode));
    mStats.mDuration = mInitialTs
            ? static_cast<uint64_t>((time(nullptr) - mInitialTs) * 1000)  // ms
            : 0; // in case we have not joined SFU yet, send duration = 0
    mStats.mMaxPeers = mMaxPeers;
    mStats.mTermCode = static_cast<int32_t>(termCode);
    mMegaApi.sdk.sendChatStats(mStats.getJson().c_str());
    RTCM_LOG_DEBUG("Clear local SFU stats");
    mStats.clear();
}

std::string Call::getKeyFromPeer(Cid_t cid, Keyid_t keyid)
{
    Session *session = getSession(cid);
    return session
            ? session->getPeer().getKey(keyid)
            : std::string();
}

bool Call::hasCallKey()
{
    return !mCallKey.empty();
}

bool Call::handleAvCommand(Cid_t cid, unsigned av)
{
    if (mState != kStateJoining && mState != kStateInProgress)
    {
        RTCM_LOG_WARNING("handleAvCommand: get unexpected state");
        assert(false); // theoretically, it should not happen. If so, it may worth to investigate
        return false;
    }

    if (mMyPeer->getCid() == cid)
    {
        RTCM_LOG_WARNING("handleAvCommand: Received our own AV flags");
        return false;
    }

    Session *session = getSession(cid);
    if (!session)
    {
        RTCM_LOG_WARNING("handleAvCommand: Received AV flags for unknown peer cid %d", cid);
        return false;
    }

    // update session flags
    session->setAvFlags(karere::AvFlags(static_cast<uint8_t>(av)));
    return true;
}

bool Call::handleAnswerCommand(Cid_t cid, sfu::Sdp& sdp, uint64_t duration, const std::vector<sfu::Peer>& peers,
                               const std::map<Cid_t, sfu::TrackDescriptor>& vthumbs, const std::map<Cid_t, sfu::TrackDescriptor>& speakers)
{
    if (mState != kStateJoining)
    {
        RTCM_LOG_WARNING("handleAnswerCommand: get unexpect state change");
        return false;
    }

    // set my own client-id (cid)
    mMyPeer->setCid(cid);

    // update max peers seen in call
    mMaxPeers = static_cast<uint8_t> (peers.size() > mMaxPeers ? peers.size() : mMaxPeers);

    std::set<Cid_t> cids;
    for (const sfu::Peer& peer : peers) // does not include own cid
    {
        cids.insert(peer.getCid());
        mSessions[peer.getCid()] = ::mega::make_unique<Session>(peer);
        mCallHandler.onNewSession(*mSessions[peer.getCid()], *this);
    }

    generateAndSendNewkey(true);

    std::string sdpUncompress = sdp.unCompress();
    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> sdpInterface(webrtc::CreateSessionDescription("answer", sdpUncompress, &error));
    if (!sdpInterface)
    {
        onCallDisconnect(TermCode::kErrSdp, "Error parsing peer SDP answer: line= " + error.line +"  \nError: " + error.description, true, mIsReconnectingToChatd);
        return false;
    }

    assert(mRtcConn);
    auto wptr = weakHandle();
    mRtcConn.setRemoteDescription(move(sdpInterface))
    .then([wptr, this, vthumbs, speakers, duration, cids]()
    {
        if (wptr.deleted())
        {
            return;
        }

        if (mState != kStateJoining)
        {
            RTCM_LOG_WARNING("handleAnswerCommand: get unexpect state change at setRemoteDescription");
            return;
        }

        // prepare parameters for low resolution video
        double scale = static_cast<double>(RtcConstant::kHiResWidth) / static_cast<double>(RtcConstant::kVthumbWidth);
        webrtc::RtpParameters parameters = mVThumb->getTransceiver()->sender()->GetParameters();
        assert(parameters.encodings.size());
        parameters.encodings[0].scale_resolution_down_by = scale;
        parameters.encodings[0].max_bitrate_bps = 100 * 1024;   // 100 Kbps
        mVThumb->getTransceiver()->sender()->SetParameters(parameters).ok();
        handleIncomingVideo(vthumbs, kLowRes);

        for (const auto& speak : speakers)  // current speakers in the call
        {
            Cid_t cid = speak.first;
            const sfu::TrackDescriptor& speakerDecriptor = speak.second;
            addSpeaker(cid, speakerDecriptor);
        }

        setState(CallState::kStateInProgress);

        mOffset = duration / 1000;
        enableStats();
    })
    .fail([wptr, this](const ::promise::Error& err)
    {
        if (wptr.deleted())
            return;

        std::string msg = "Error setting SDP answer: " + err.msg();
        onCallDisconnect(TermCode::kErrSdp, msg, true, mIsReconnectingToChatd);
    });

    return true;
}

bool Call::handleKeyCommand(Keyid_t keyid, Cid_t cid, const std::string &key)
{
    if (mState != kStateInProgress && mState != kStateJoining)
    {
        RTCM_LOG_WARNING("handleKeyCommand: get unexpected state");
        assert(false); // theoretically, it should not happen. If so, it may worth to investigate
        return false;
    }

    Session *session = getSession(cid);
    if (!session)
    {
        RTCM_LOG_ERROR("handleKeyCommand: Received key for unknown peer cid %d", cid);
        return false;
    }

    karere::Id peerid = session->getPeer().getPeerid();
    auto wptr = weakHandle();
    mSfuClient.getRtcCryptoMeetings()->getCU25519PublicKey(peerid)
    .then([wptr, keyid, cid, key, this](Buffer*)
    {
        if (wptr.deleted())
        {
            return;
        }

        Session *session = getSession(cid);
        if (!session)
        {
            RTCM_LOG_WARNING("handleKeyCommand after get Cu25510 key: Received key for unknown peer cid %d", cid);
            return;
        }

        // decrypt received key
        std::string binaryKey = mega::Base64::atob(key);
        strongvelope::SendKey encryptedKey;
        mSfuClient.getRtcCryptoMeetings()->strToKey(binaryKey, encryptedKey);

        strongvelope::SendKey plainKey;
        mSfuClient.getRtcCryptoMeetings()->decryptKeyFrom(session->getPeer().getPeerid(), encryptedKey, plainKey);

        // in case of a call in a public chatroom, XORs received key with the call key for additional authentication
        if (hasCallKey())
        {
            strongvelope::SendKey callKey;
            mSfuClient.getRtcCryptoMeetings()->strToKey(mCallKey, callKey);
            mSfuClient.getRtcCryptoMeetings()->xorWithCallKey(callKey, plainKey);
        }

        // add new key to peer key map
        std::string newKey = mSfuClient.getRtcCryptoMeetings()->keyToStr(plainKey);
        session->addKey(keyid, newKey);
    });

    return true;
}

bool Call::handleVThumbsCommand(const std::map<Cid_t, sfu::TrackDescriptor> &videoTrackDescriptors)
{
    if (mState != kStateInProgress && mState != kStateJoining)
    {
        RTCM_LOG_WARNING("handleVThumbsCommand: get unexpected state");
        assert(false); // theoretically, it should not happen. If so, it may worth to investigate
        return false;
    }

    handleIncomingVideo(videoTrackDescriptors, kLowRes);
    return true;
}

bool Call::handleVThumbsStartCommand()
{
    if (mState != kStateInProgress && mState != kStateJoining)
    {
        RTCM_LOG_WARNING("handleVThumbsStartCommand: get unexpected state");
        assert(false); // theoretically, it should not happen. If so, it may worth to investigate
        return false;
    }

    mVThumbActive = true;
    updateVideoTracks();
    return true;
}

bool Call::handleVThumbsStopCommand()
{
    if (mState != kStateInProgress && mState != kStateJoining)
    {
        RTCM_LOG_WARNING("handleVThumbsStopCommand: get unexpected state");
        assert(false); // theoretically, it should not happen. If so, it may worth to investigate
        return false;
    }

    mVThumbActive = false;
    updateVideoTracks();
    return true;
}

bool Call::handleHiResCommand(const std::map<Cid_t, sfu::TrackDescriptor>& videoTrackDescriptors)
{
    if (mState != kStateInProgress && mState != kStateJoining)
    {
        RTCM_LOG_WARNING("handleHiResCommand: get unexpected state");
        assert(false); // theoretically, it should not happen. If so, it may worth to investigate
        return false;
    }

    handleIncomingVideo(videoTrackDescriptors, kHiRes);
    return true;
}

bool Call::handleHiResStartCommand()
{
    if (mState != kStateInProgress && mState != kStateJoining)
    {
        RTCM_LOG_WARNING("handleHiResStartCommand: get unexpected state");
        assert(false); // theoretically, it should not happen. If so, it may worth to investigate
        return false;
    }

    mHiResActive = true;
    updateVideoTracks();
    return true;
}

bool Call::handleHiResStopCommand()
{
    if (mState != kStateInProgress && mState != kStateJoining)
    {
        RTCM_LOG_WARNING("handleHiResStopCommand: get unexpected state");
        assert(false); // theoretically, it should not happen. If so, it may worth to investigate
        return false;
    }

    mHiResActive = false;
    updateVideoTracks();
    return true;
}

bool Call::handleSpeakReqsCommand(const std::vector<Cid_t> &speakRequests)
{
    if (mState != kStateInProgress && mState != kStateJoining)
    {
        RTCM_LOG_WARNING("handleSpeakReqsCommand: get unexpected state");
        assert(false); // theoretically, it should not happen. If so, it may worth to investigate
        return false;
    }

    for (Cid_t cid : speakRequests)
    {
        if (cid != mMyPeer->getCid())
        {
            Session *session = getSession(cid);
            assert(session);
            if (!session)
            {
                RTCM_LOG_ERROR("handleSpeakReqsCommand: Received speakRequest for unknown peer cid %d", cid);
                continue;
            }
            session->setSpeakRequested(true);
        }
    }

    return true;
}

bool Call::handleSpeakReqDelCommand(Cid_t cid)
{
    if (mState != kStateInProgress && mState != kStateJoining)
    {
        RTCM_LOG_WARNING("handleSpeakReqDelCommand: get unexpected state");
        assert(false); // theoretically, it should not happen. If so, it may worth to investigate
        return false;
    }

    if (mMyPeer->getCid() != cid) // remote peer
    {
        Session *session = getSession(cid);
        assert(session);
        if (!session)
        {
            RTCM_LOG_ERROR("handleSpeakReqDelCommand: Received delSpeakRequest for unknown peer cid %d", cid);
            return false;
        }
        session->setSpeakRequested(false);
    }
    else if (mSpeakerState == SpeakerState::kPending)
    {
        // only update audio tracks if mSpeakerState is pending to be accepted
        mSpeakerState = SpeakerState::kNoSpeaker;
        updateAudioTracks();
    }
    else    // own cid, but SpeakerState is not kPending
    {
        RTCM_LOG_ERROR("handleSpeakReqDelCommand: Received delSpeakRequest for own cid %d without a pending requests", cid);
        assert(false);
        return false;
    }

    return true;
}

bool Call::handleSpeakOnCommand(Cid_t cid, sfu::TrackDescriptor speaker)
{
    if (mState != kStateInProgress && mState != kStateJoining)
    {
        RTCM_LOG_WARNING("handleSpeakOnCommand: get unexpected state");
        assert(false); // theoretically, it should not happen. If so, it may worth to investigate
        return false;
    }

    // TODO: check if the received `cid` is 0 for own cid, or it should be mMyPeer->getCid()
    if (cid)
    {
        assert(cid != mMyPeer->getCid());
        addSpeaker(cid, speaker);
    }
    else if (mSpeakerState == SpeakerState::kPending)
    {
        mSpeakerState = SpeakerState::kActive;
        updateAudioTracks();
    }
    else    // own cid, but SpeakerState is not kPending
    {
        RTCM_LOG_ERROR("handleSpeakOnCommand: Received speak on for own cid %d without a pending requests", cid);
        assert(false);
        return false;
    }

    return true;
}

bool Call::handleSpeakOffCommand(Cid_t cid)
{
    if (mState != kStateInProgress && mState != kStateJoining)
    {
        RTCM_LOG_WARNING("handleSpeakOffCommand: get unexpected state");
        assert(false); // theoretically, it should not happen. If so, it may worth to investigate
        return false;
    }

    // TODO: check if the received `cid` is 0 for own cid, or it should be mMyPeer->getCid()
    if (cid)
    {
        assert(cid != mMyPeer->getCid());
        removeSpeaker(cid);
    }
    else if (mSpeakerState == SpeakerState::kActive)
    {
        mSpeakerState = SpeakerState::kNoSpeaker;
        updateAudioTracks();
    }
    else    // own cid, but SpeakerState is not kActive
    {
        RTCM_LOG_ERROR("handleSpeakOffCommand: Received speak off for own cid %d without being active", cid);
        assert(false);
        return false;
    }

    return true;
}


bool Call::handlePeerJoin(Cid_t cid, uint64_t userid, int av)
{
    if (mState != kStateInProgress && mState != kStateJoining)
    {
        RTCM_LOG_WARNING("handlePeerJoin: get unexpected state");
        assert(false); // theoretically, it should not happen. If so, it may worth to investigate
        return false;
    }

    sfu::Peer peer(userid, static_cast<unsigned>(av), cid);
    mSessions[cid] = ::mega::make_unique<Session>(peer);
    mCallHandler.onNewSession(*mSessions[cid], *this);
    // update max peers seen in call
    mMaxPeers = static_cast<uint8_t> (mSessions.size() > mMaxPeers ? mSessions.size() : mMaxPeers);
    generateAndSendNewkey();

    if (mIsReconnectingToChatd && mParticipants.find(peer.getPeerid()) == mParticipants.end())
    {
        // if we are disconnected from chatd, but still connected to SFU and participating in a call
        // we need to update participants list with SFU information
        mParticipants.insert(peer.getPeerid());
        mCallHandler.onAddPeer(*this, peer.getPeerid());
    }

    return true;
}

bool Call::handlePeerLeft(Cid_t cid)
{
    if (mState != kStateInProgress && mState != kStateJoining)
    {
        RTCM_LOG_WARNING("handlePeerLeft: get unexpected state");
        assert(false); // theoretically, it should not happen. If so, it may worth to investigate
        return false;
    }

    auto it = mSessions.find(cid);
    if (it == mSessions.end())
    {
        RTCM_LOG_ERROR("handlePeerLeft: unknown cid");
        return false;
    }

    if (mIsReconnectingToChatd && mParticipants.find(it->second->getPeerid()) != mParticipants.end()
            && getSessionsCidsByUserHandle(it->second->getPeerid()).size() == 1)
    {
        // Check that received peer left is not participating in meeting with more than one client

        // if we are disconnected from chatd but still connected to SFU, and participating in a call
        // we need to update participants list with SFU information
        mParticipants.erase(it->second->getPeerid());
        mCallHandler.onRemovePeer(*this, it->second->getPeerid());
    }

    it->second->disableAudioSlot();
    it->second->disableVideoSlot(kHiRes);
    it->second->disableVideoSlot(kLowRes);
    mSessions.erase(cid);
    return true;
}

void Call::onSfuConnected()
{
    joinSfu();
}

void Call::onSfuDisconnected()
{
    onCallDisconnect(kRtcDisconn, "SFU connection onSocketClose", false, mIsReconnectingToChatd, true /*avoid bye command*/);
}

void Call::callDisconnect(const TermCode& termCode)
{
    RTCM_LOG_DEBUG("callDisconnect, termcode (%d): %s", termCode, connectionTermCodeToString(termCode).c_str());
    if (getLocalAvFlags().videoCam())
    {
        releaseVideoDevice();
    }

    for (const auto& session : mSessions)
    {
        session.second->disableAudioSlot();
    }

    // close signaling connection and clear some call stuff
    signalingDisconnectAndClear(termCode);

    // termcode is only valid at state kStateTerminatingUserParticipation
    mTermCode = termCode;
    setState(CallState::kStateTerminatingUserParticipation);
    if (mSfuConnection)
    {
        mSfuClient.closeSfuConnection(mChatid);
        mSfuConnection = nullptr;
    }

    // reset termcode upon set state kStateClientNoParticipating
    mTermCode = kInvalidTermCode;
    setState(CallState::kStateClientNoParticipating);
}

void Call::onSendByeCommand()
{
    auto wptr = weakHandle();
    karere::marshallCall([wptr, this]()
    {
        if (wptr.deleted())
        {
            return;
        }

        if (!mSfuConnection)
        {
            RTCM_LOG_DEBUG("onSendByeCommand: SFU connection doesn't exists anymore");
            return;
        }

        assert (mTempTermCode != kInvalidTermCode);
        switch (mSfuConnection->getDisconnectType())
        {
            case ::sfu::SfuConnection::kSignalingDisconnect:
                {
                signalingDisconnectAndClear(mTempTermCode);
                mSfuConnection->resetDisconnectAttempt();
                }
                break;
            case ::sfu::SfuConnection::kSfuDisconnect:
                callDisconnect(mTempTermCode); // full disconnect (sfu & signaling), mSfuConnection will be destroyed
                break;
            default:
                assert(!sfu::SfuConnection::isValidDisconnectType(mSfuConnection->getDisconnectType()));
                break;
        }
        mTempTermCode = kInvalidTermCode;
    }, mRtc.getAppCtx());
}

bool Call::error(unsigned int code, const std::string &errMsg)
{
    auto wptr = weakHandle();
    karere::marshallCall([wptr, this, code, errMsg]()
    {
        // error() is called from LibwebsocketsClient::wsCallback() for LWS_CALLBACK_CLIENT_RECEIVE.
        // If disconnect() is called here immediately, it will destroy the LWS client synchronously,
        // leave it in an invalid state (and will crash at Libwebsockets::resetMessage())

        if (wptr.deleted())
        {
            return;
        }

        // For SFU errors no need to send BYE command, the SFU will close the connection immediately after sending the error
        // TermCode is set at callDisconnect, removeCall will set EndCall reason to kFailed
        TermCode connectionTermCode = static_cast<TermCode>(code);

        // don't clear participants at disconnect if recoverable error, as some temporal errors received from SFU don't require to remove call
        onCallDisconnect(connectionTermCode, errMsg, true, !isTermCodeRetriable(connectionTermCode), true /*avoid bye command*/);
        if (mParticipants.empty())
        {
            mRtc.removeCall(mChatid, EndCallReason::kFailed, connectionTermCode);
        }
    }, mRtc.getAppCtx());

    return true;
}

void Call::logError(const char *error)
{
    RTCM_LOG_ERROR("SFU: %s", error);
}

void Call::onAddStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> /*stream*/)
{
    if (mState != kStateJoining)
    {
        RTCM_LOG_WARNING("onAddStream: get unexpected state");
        assert(mState != kStateInProgress); // theoretically, it should not happen. If so, it may worth to investigate
        return;
    }

    assert(mVThumb && mHiRes && mAudio);
    mVThumb->createEncryptor();
    mHiRes->createEncryptor();
    mAudio->createEncryptor();
}

void Call::onTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver)
{
    if (mState != kStateJoining)
    {
        RTCM_LOG_WARNING("onTrack: get unexpected state");
        assert(mState != kStateInProgress); // theoretically, it should not happen. If so, it may worth to investigate
        return;
    }

    absl::optional<std::string> mid = transceiver->mid();
    if (mid.has_value())
    {
        std::string value = mid.value();
        if (transceiver->media_type() == cricket::MediaType::MEDIA_TYPE_AUDIO)
        {
            mReceiverTracks[static_cast<uint32_t>(atoi(value.c_str()))] = ::mega::make_unique<RemoteAudioSlot>(*this, transceiver,
                                                                                                               mRtc.getAppCtx());
        }
        else
        {
            mReceiverTracks[static_cast<uint32_t>(atoi(value.c_str()))] = ::mega::make_unique<RemoteVideoSlot>(*this, transceiver,
                                                                                                               mRtc.getAppCtx());
        }
    }
}

void Call::onRemoveTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> /*receiver*/)
{
    RTCM_LOG_DEBUG("onRemoveTrack received");
}

void Call::onConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState newState)
{
    RTCM_LOG_DEBUG("onConnectionChange newstate: %d", newState);
    if (!mSfuConnection)
    {
        RTCM_LOG_WARNING("onConnectionChange: mSfuConnection no longer exists");
        return;
    }

    if (newState >= webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected)
    {
        // if mState is kDisconnected | kFailed | kClosed we need to clear commands queue and set sending as false
        // otherwise nextcommand could get stucked
        if (mSfuConnection)
        {
            mSfuConnection->clearCommandsQueue();
        }

        if (mState == CallState::kStateJoining ||  mState == CallState::kStateInProgress) //  kStateConnecting isn't included to avoid interrupting a reconnection in progress
        {
            if (mState == CallState::kStateInProgress)
            {
                onCallDisconnect(TermCode::kRtcDisconn, "onConnectionChange received with PeerConnectionState::kDisconnected", false, mIsReconnectingToChatd);
            }

            setState(CallState::kStateConnecting);
            mSfuConnection->retryPendingConnection(true);
        }
    }
    else if (newState == webrtc::PeerConnectionInterface::PeerConnectionState::kConnected)
    {
        bool reconnect = !mSfuConnection->isOnline();
        RTCM_LOG_DEBUG("onConnectionChange retryPendingConnection (reconnect) : %d", reconnect);
        mSfuConnection->retryPendingConnection(reconnect);
    }
}

Keyid_t Call::generateNextKeyId()
{
    if (mMyPeer->getCurrentKeyId() >= 255
            || (!mMyPeer->getCurrentKeyId() && !mMyPeer->hasAnyKey()))
    {
        // if we have exceeded max keyid => reset keyid to zero
        // if current keyId is zero and we don't have stored any key => first keyId (zero)
        return 0;
    }
    else
    {
        return static_cast<Keyid_t>(mMyPeer->getCurrentKeyId() + 1);
    }
}

void Call::generateAndSendNewkey(bool reset)
{
    if (reset)
    {
        // when you leave a meeting or you experiment a reconnect, we should reset keyId to zero and clear keys map
        mMyPeer->resetKeys();
    }

    // generate a new plain key
    std::shared_ptr<strongvelope::SendKey> newPlainKey = mSfuClient.getRtcCryptoMeetings()->generateSendKey();

    // add new key to own peer key map and update currentKeyId
    Keyid_t newKeyId = generateNextKeyId();
    std::string plainKeyStr = mSfuClient.getRtcCryptoMeetings()->keyToStr(*newPlainKey.get());

    // in case of a call in a public chatroom, XORs new key with the call key for additional authentication
    if (hasCallKey())
    {
        strongvelope::SendKey callKey;
        mSfuClient.getRtcCryptoMeetings()->strToKey(mCallKey, callKey);
        mSfuClient.getRtcCryptoMeetings()->xorWithCallKey(callKey, *newPlainKey.get());
    }

    std::vector<promise::Promise<Buffer*>> promises;
    for (const auto& session : mSessions) // encrypt key to all participants
    {
        promises.push_back(mSfuClient.getRtcCryptoMeetings()->getCU25519PublicKey(session.second->getPeer().getPeerid()));
    }

    auto wptr = weakHandle();
    promise::when(promises)
    .then([wptr, newKeyId, plainKeyStr, newPlainKey, this]
    {
        if (wptr.deleted())
        {
            return;
        }

        std::map<Cid_t, std::string> keys;

        for (const auto& session : mSessions) // encrypt key to all participants
        {
            // get peer Cid
            Cid_t sessionCid = session.first;

            // get peer id
            karere::Id peerId = session.second->getPeer().getPeerid();

            // encrypt key to participant
            strongvelope::SendKey encryptedKey;
            mSfuClient.getRtcCryptoMeetings()->encryptKeyTo(peerId, *newPlainKey.get(), encryptedKey);

            keys[sessionCid] = mega::Base64::btoa(std::string(encryptedKey.buf(), encryptedKey.size()));
        }

        mSfuConnection->sendKey(newKeyId, keys);

        // set a small delay after broadcasting the new key, and before starting to use it,
        // to minimize the chance that the key hasn't yet been received over the signaling channel
        karere::setTimeout([this, newKeyId, plainKeyStr, wptr]()
        {
            if (wptr.deleted())
            {
                return;
            }

            // add key to peer's key map, although is not encrypted for any other participant,
            // as we need to start sending audio frames as soon as we receive SPEAK_ON command
            // and we could receive it even if there's no more participants in the meeting
            mMyPeer->addKey(newKeyId, plainKeyStr);
        }, RtcConstant::kRotateKeyUseDelay, mRtc.getAppCtx());

    });
}

void Call::handleIncomingVideo(const std::map<Cid_t, sfu::TrackDescriptor> &videotrackDescriptors, VideoResolution videoResolution)
{
    for (auto trackDescriptor : videotrackDescriptors)
    {
        auto it = mReceiverTracks.find(trackDescriptor.second.mMid);
        if (it == mReceiverTracks.end())
        {
            RTCM_LOG_ERROR("Unknown vtrack mid %d", trackDescriptor.second.mMid);
            continue;
        }

        Cid_t cid = trackDescriptor.first;
        uint32_t mid = trackDescriptor.second.mMid;
        RemoteVideoSlot *slot = static_cast<RemoteVideoSlot*>(it->second.get());
        if (slot->getCid() == cid && slot->getVideoResolution() == videoResolution)
        {
            RTCM_LOG_WARNING("Follow same cid with same resolution over same track");
            continue;
        }

        if (slot->getCid() != 0)    // the slot is already in use, need to release first and notify
        {
            if (trackDescriptor.second.mReuse && slot->getCid() != cid)
            {
                RTCM_LOG_ERROR("attachSlotToSession: trying to reuse slot, but cid has changed");
                assert(false && "Possible error at SFU: slot with CID not found");
            }

            RTCM_LOG_DEBUG("reassign slot with mid: %d from cid: %d to newcid: %d, reuse: %d ", mid, slot->getCid(), cid, trackDescriptor.second.mReuse);

            Session *oldSess = getSession(slot->getCid());
            if (oldSess)
            {
                // In case of Slot reassign for another peer (CID) or same peer (CID) slot reusing, we need to notify app about that
                oldSess->disableVideoSlot(slot->getVideoResolution());
            }
        }

        Session *sess = getSession(cid);
        if (!sess)
        {
            RTCM_LOG_ERROR("handleIncomingVideo: session with CID %d not found", cid);
            assert(false && "Possible error at SFU: session with CID not found");
            continue;
        }

        slot->assignVideoSlot(cid, trackDescriptor.second.mIv, videoResolution);
        attachSlotToSession(cid, slot, false, videoResolution);
    }
}

void Call::attachSlotToSession (Cid_t cid, RemoteSlot* slot, bool audio, VideoResolution hiRes)
{
    Session *session = getSession(cid);
    assert(session);
    if (!session)
    {
        RTCM_LOG_WARNING("attachSlotToSession: unknown peer cid %d", cid);
        return;
    }

    if (audio)
    {
        session->setAudioSlot(static_cast<RemoteAudioSlot *>(slot));
    }
    else
    {
        if (hiRes)
        {
            session->setHiResSlot(static_cast<RemoteVideoSlot *>(slot));
        }
        else
        {
            session->setVThumSlot(static_cast<RemoteVideoSlot *>(slot));
        }
    }
}

void Call::addSpeaker(Cid_t cid, const sfu::TrackDescriptor &speaker)
{
    auto it = mReceiverTracks.find(speaker.mMid);
    if (it == mReceiverTracks.end())
    {
        RTCM_LOG_WARNING("AddSpeaker: unknown track mid %d", speaker.mMid);
        return;
    }

    RemoteAudioSlot* slot = static_cast<RemoteAudioSlot*>(it->second.get());
    if (slot->getCid() != cid)
    {
        Session *oldSess = getSession(slot->getCid());
        if (oldSess)
        {
            // In case of Slot reassign for another peer (CID) we need to notify app about that
            oldSess->disableAudioSlot();
        }
    }

    Session *sess = getSession(cid);
    if (!sess)
    {
        RTCM_LOG_WARNING("AddSpeaker: unknown cid");
        return;
    }

    slot->assignAudioSlot(cid, speaker.mIv);
    attachSlotToSession(cid, slot, true, kUndefined);
}

void Call::removeSpeaker(Cid_t cid)
{
    auto it = mSessions.find(cid);
    if (it == mSessions.end())
    {
        RTCM_LOG_ERROR("removeSpeaker: unknown cid");
        return;
    }
    it->second->disableAudioSlot();
}

sfu::Peer& Call::getMyPeer()
{
    return *mMyPeer;
}

sfu::SfuClient &Call::getSfuClient()
{
    return mSfuClient;
}

std::map<Cid_t, std::unique_ptr<Session> > &Call::getSessions()
{
    return mSessions;
}

void Call::takeVideoDevice()
{
    if (!mVideoManager)
    {
        mRtc.takeDevice();
        mVideoManager = mRtc.getVideoDevice();
    }
}

void Call::releaseVideoDevice()
{
    if (mVideoManager)
    {
        mRtc.releaseDevice();
        mVideoManager = nullptr;
    }
}

bool Call::hasVideoDevice()
{
    return mVideoManager ? true : false;
}

void Call::freeVideoTracks(bool releaseSlots)
{
    // disable hi-res track
    if (mHiRes && mHiRes->getTransceiver()->sender()->track())
    {
        mHiRes->getTransceiver()->sender()->SetTrack(nullptr);
    }

    // disable low-res track
    if (mVThumb && mVThumb->getTransceiver()->sender()->track())
    {
        mVThumb->getTransceiver()->sender()->SetTrack(nullptr);
    }

    if (releaseSlots) // release slots in case flag is true
    {
        mVThumb.reset();
        mHiRes.reset();
    }
}

void Call::freeAudioTrack(bool releaseSlot)
{
    // disable audio track
    if (mAudio && mAudio->getTransceiver()->sender()->track())
    {
        mAudio->getTransceiver()->sender()->SetTrack(nullptr);
    }

    if (releaseSlot) // release slot in case flag is true
    {
        mAudio.reset();
    }
}

void Call::collectNonRTCStats()
{
    int audioSession = 0;
    int vThumbSession = 0;
    int hiResSession = 0;
    for (const auto& session : mSessions)
    {
        if (session.second->getAudioSlot())
        {
            audioSession++;
        }

        if (session.second->getVthumSlot())
        {
            vThumbSession++;
        }

        if (session.second->getHiResSlot())
        {
            hiResSession++;
        }
    }

    // TODO: pending to implement disabledTxLayers in future if needed
    mStats.mSamples.mQ.push_back(static_cast<int32_t>(mSvcDriver.mCurrentSvcLayerIndex) | static_cast<int32_t>(kTxSpatialLayerCount) << 8);
    mStats.mSamples.mNrxa.push_back(audioSession);
    mStats.mSamples.mNrxl.push_back(vThumbSession);
    mStats.mSamples.mNrxh.push_back(hiResSession);
    mStats.mSamples.mAv.push_back(getLocalAvFlags().value());
}

void Call::initStatsValues()
{
    mStats.mPeerId = mMyPeer->getPeerid();
    mStats.mCallid = mCallid;
    mStats.mIsGroup = mIsGroup;
    mStats.mDevice = mRtc.getDeviceInfo();
    mStats.mSfuHost = mSfuConnection->getSfuUrl().host;
}

void Call::enableStats()
{
    mStats.mCid = mMyPeer->getCid();
    mStats.mTimeOffset = static_cast<uint64_t>(mOffset);
    auto wptr = weakHandle();
    mStatsTimer = karere::setInterval([this, wptr]()
    {
        if (wptr.deleted())
        {
            return;
        }

        if (!mSfuConnection || !mSfuConnection->isJoined())
        {
            RTCM_LOG_WARNING("Cannot collect stats until reach kJoined state");
            return;
        }

        // poll TxVideoStats
        assert(mVThumb && mHiRes);
        uint32_t hiResId = 0;
        if (mHiResActive)
        {
            hiResId = mHiRes->getTransceiver()->sender()->ssrc();
        }

        uint32_t lowResId = 0;
        if (mVThumbActive)
        {
            lowResId = mVThumb->getTransceiver()->sender()->ssrc();
        }

        // poll non-rtc stats
        collectNonRTCStats();

        // Keep mStats ownership
        mStatConnCallback = rtc::scoped_refptr<webrtc::RTCStatsCollectorCallback>(new ConnStatsCallBack(&mStats, hiResId, lowResId, mRtc.getAppCtx()));
        assert(mRtcConn);
        mRtcConn->GetStats(mStatConnCallback.get());

        // adjust SVC driver based on collected stats
        // TODO: I can be done in ConnStatsCallBack to take into account latest stats
        adjustSvcByStats();
    }, RtcConstant::kStatsInterval, mRtc.getAppCtx());
}

void Call::disableStats()
{
    if (mStatsTimer != 0)
    {
        karere::cancelInterval(mStatsTimer, mRtc.getAppCtx());
        mStatsTimer = 0;
        if (mStatConnCallback)
        {
            static_cast<ConnStatsCallBack*>(mStatConnCallback.get())->removeStats();
        }

        mStatConnCallback = nullptr;
    }
}

void Call::updateVideoTracks()
{
    bool isOnHold = getLocalAvFlags().isOnHold();
    if (getLocalAvFlags().videoCam() && !isOnHold)
    {
        takeVideoDevice();

        // hi-res track
        if (mHiRes)
        {
            if (mHiResActive && !mHiRes->getTransceiver()->sender()->track())
            {
                rtc::scoped_refptr<webrtc::VideoTrackInterface> videoTrack;
                videoTrack = artc::gWebrtcContext->CreateVideoTrack("v"+std::to_string(artc::generateId()), mRtc.getVideoDevice()->getVideoTrackSource());
                mHiRes->getTransceiver()->sender()->SetTrack(videoTrack);
            }
            else if (!mHiResActive)
            {
                // if there is a track, but none in the call has requested hi res video, disable the track
                mHiRes->getTransceiver()->sender()->SetTrack(nullptr);
            }
        }

        // low-res track
        if (mVThumb)
        {
            if (mVThumbActive && !mVThumb->getTransceiver()->sender()->track())
            {
                rtc::scoped_refptr<webrtc::VideoTrackInterface> videoTrack;
                videoTrack = artc::gWebrtcContext->CreateVideoTrack("v"+std::to_string(artc::generateId()), mRtc.getVideoDevice()->getVideoTrackSource());
                mVThumb->getTransceiver()->sender()->SetTrack(videoTrack);
            }
            else if (!mVThumbActive)
            {
                // if there is a track, but none in the call has requested low res video, disable the track
                mVThumb->getTransceiver()->sender()->SetTrack(nullptr);
            }
        }
    }
    else    // no video from camera (muted or not available), or call on-hold
    {
        freeVideoTracks();
        releaseVideoDevice();
    }
}

void Call::adjustSvcByStats()
{
    if (mStats.mSamples.mRoundTripTime.empty())
    {
        RTCM_LOG_WARNING("adjustSvcBystats: not enough collected data");
        return;
    }

    double roundTripTime = mStats.mSamples.mRoundTripTime.back();
    double packetLost = 0;
    if (mStats.mSamples.mPacketLost.size() >= 2)
    {
        // get last lost packets
        int lastpl =  mStats.mSamples.mPacketLost.back();
        int prelastpl= mStats.mSamples.mPacketLost.at(mStats.mSamples.mPacketLost.size()-2);

        // get periods
        int lastT = mStats.mSamples.mT.back();
        int prelastT = mStats.mSamples.mT.at(mStats.mSamples.mT.size() - 2);
        packetLost = static_cast<double>(abs(lastpl - prelastpl)) / (static_cast<double>(abs(lastT - prelastT)) / 1000.0);
    }

    if (std::fabs(mSvcDriver.mMovingAverageRtt) <= std::numeric_limits<double>::epsilon())
    {
         // if mMovingAverageRtt has not value yet
         mSvcDriver.mMovingAverageRtt = roundTripTime;
         mSvcDriver.mMovingAveragePlost = packetLost;
         return; // intentionally skip first sample for lower/upper range calculation
    }

    if (roundTripTime < mSvcDriver.mLowestRttSeen)
    {
        // rttLower and rttUpper define the window inside which layer is not switched.
        //  - if rtt falls below that window, layer is switched to higher quality,
        //  - if rtt is higher, layer is switched to lower quality.
        // the window is defined/redefined relative to the lowest rtt seen.
        mSvcDriver.mLowestRttSeen = roundTripTime;
        mSvcDriver.mRttLower = roundTripTime + mSvcDriver.kRttLowerHeadroom;
        mSvcDriver.mRttUpper = roundTripTime + mSvcDriver.kRttUpperHeadroom;
    }

    roundTripTime = mSvcDriver.mMovingAverageRtt = (mSvcDriver.mMovingAverageRtt * 3 + roundTripTime) / 4;
    packetLost  = mSvcDriver.mMovingAveragePlost = (mSvcDriver.mMovingAveragePlost * 3 + packetLost) / 4;

    time_t tsNow = time(nullptr);
    if (mSvcDriver.mTsLastSwitch
            && (tsNow - mSvcDriver.mTsLastSwitch < mSvcDriver.kMinTimeBetweenSwitches))
    {
        return; // too early
    }

    if (mSvcDriver.mCurrentSvcLayerIndex > 0
            && (roundTripTime > mSvcDriver.mRttUpper || packetLost > mSvcDriver.mPacketLostUpper))
    {
        // if retrieved rtt OR packetLost have increased respect current values decrement 1 layer
        // we want to decrease layer when references values (mRttUpper and mPacketLostUpper)
        // have been exceeded.
        updateSvcQuality(-1);
    }
    else if (mSvcDriver.mCurrentSvcLayerIndex < mSvcDriver.kMaxQualityIndex
             && roundTripTime < mSvcDriver.mRttLower
             && packetLost < mSvcDriver.mPacketLostLower)
    {
        // if retrieved rtt AND packetLost have decreased respect current values increment 1 layer
        // we only want to increase layer when the improvement is bigger enough to represents a
        // faithfully improvement in network quality, we take mRttLower and mPacketLostLower as references
        updateSvcQuality(+1);
    }
}

const std::string& Call::getCallKey() const
{
    return mCallKey;
}

void Call::updateAudioTracks()
{
    if (!mAudio)
    {
        return;
    }

    bool audio = mSpeakerState > SpeakerState::kNoSpeaker && getLocalAvFlags().audio();
    rtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track = mAudio->getTransceiver()->sender()->track();
    if (audio && !getLocalAvFlags().isOnHold())
    {
        if (!track) // create audio track only if not exists
        {
            rtc::scoped_refptr<webrtc::AudioTrackInterface> audioTrack =
                    artc::gWebrtcContext->CreateAudioTrack("a"+std::to_string(artc::generateId()), artc::gWebrtcContext->CreateAudioSource(cricket::AudioOptions()));

            mAudio->getTransceiver()->sender()->SetTrack(audioTrack);
            audioTrack->set_enabled(true);
        }
        else
        {
            track->set_enabled(true);
        }
    }
    else if (track) // if no audio flags active, no speaker allowed, or call is onHold
    {
        track->set_enabled(false);
        mAudio->getTransceiver()->sender()->SetTrack(nullptr);
    }
}

RtcModuleSfu::RtcModuleSfu(MyMegaApi &megaApi, CallHandler &callhandler, DNScache &dnsCache,
                           WebsocketsIO& websocketIO, void *appCtx,
                           rtcModule::RtcCryptoMeetings* rRtcCryptoMeetings)
    : VideoSink(appCtx)
    , mCallHandler(callhandler)
    , mMegaApi(megaApi)
    , mDnsCache(dnsCache)
{
    mAppCtx = appCtx;

    mSfuClient = ::mega::make_unique<sfu::SfuClient>(websocketIO, appCtx, rRtcCryptoMeetings);
    if (!artc::isInitialized())
    {
        //rtc::LogMessage::LogToDebug(rtc::LS_VERBOSE);
        artc::init(appCtx);
        RTCM_LOG_DEBUG("WebRTC stack initialized before first use");
    }

    // set default video in device
    std::set<std::pair<std::string, std::string>> videoDevices = artc::VideoManager::getVideoDevices();
    if (videoDevices.size())
    {
        mVideoDeviceSelected = videoDevices.begin()->second;
    }

    mDeviceTakenCount = 0;
}

ICall *RtcModuleSfu::findCall(karere::Id callid)
{
    auto it = mCalls.find(callid);
    if (it != mCalls.end())
    {
        return it->second.get();
    }

    return nullptr;
}

ICall *RtcModuleSfu::findCallByChatid(const karere::Id &chatid)
{
    for (const auto& call : mCalls)
    {
        if (call.second->getChatid() == chatid)
        {
            return call.second.get();
        }
    }

    return nullptr;
}

bool RtcModuleSfu::isCallStartInProgress(const karere::Id &chatid) const
{
    return mCallStartAttempts.find(chatid) != mCallStartAttempts.end();
}

bool RtcModuleSfu::selectVideoInDevice(const std::string &device)
{
    std::set<std::pair<std::string, std::string>> videoDevices = artc::VideoManager::getVideoDevices();
    bool shouldOpen = false;
    for (auto it = videoDevices.begin(); it != videoDevices.end(); it++)
    {
        if (!it->first.compare(device))
        {
            std::vector<Call*> calls;
            for (auto& callIt : mCalls)
            {
                if (callIt.second->hasVideoDevice())
                {
                    calls.push_back(callIt.second.get());
                    callIt.second->freeVideoTracks();
                    callIt.second->releaseVideoDevice();
                    shouldOpen = true;
                }
            }

            changeDevice(it->second, shouldOpen);

            for (auto& call : calls)
            {
                call->updateVideoTracks();
            }

            return true;
        }
    }
    return false;
}

void RtcModuleSfu::getVideoInDevices(std::set<std::string> &devicesVector)
{
    std::set<std::pair<std::string, std::string>> videoDevices = artc::VideoManager::getVideoDevices();
    for (auto it = videoDevices.begin(); it != videoDevices.end(); it++)
    {
        devicesVector.insert(it->first);
    }
}

promise::Promise<void> RtcModuleSfu::startCall(karere::Id chatid, karere::AvFlags avFlags, bool isGroup, std::shared_ptr<std::string> unifiedKey)
{
    // add chatid to CallsAttempts to avoid multiple start call attempts
    mCallStartAttempts.insert(chatid);

    // we need a temp string to avoid issues with lambda shared pointer capture
    std::string auxCallKey = unifiedKey ? (*unifiedKey.get()) : std::string();
    auto wptr = weakHandle();
    return mMegaApi.call(&::mega::MegaApi::startChatCall, chatid)
    .then([wptr, this, chatid, avFlags, isGroup, auxCallKey](ReqResult result) -> promise::Promise<void>
    {
        if (wptr.deleted())
        {
            return promise::Error("call started successfully, but RtcModuleSfu instance was removed");
        }

        std::shared_ptr<std::string> sharedUnifiedKey = !auxCallKey.empty()
                ? std::make_shared<std::string>(auxCallKey)
                : nullptr;

        karere::Id callid = result->getParentHandle();
        std::string sfuUrlStr = result->getText();
        mCallStartAttempts.erase(chatid); // remove chatid from CallsAttempts
        if (mCalls.find(callid) == mCalls.end()) // it can be created by JOINEDCALL command
        {
            std::unique_ptr<char []> userHandle(mMegaApi.sdk.getMyUserHandle());
            karere::Id myUserHandle(userHandle.get());
            mCalls[callid] = ::mega::make_unique<Call>(callid, chatid, myUserHandle, false, mCallHandler, mMegaApi, (*this), isGroup, sharedUnifiedKey, avFlags);

            if (!mCalls[callid]->connectSfu(sfuUrlStr))
            {
               return promise::Error("connectSfu error, invalid or empty URL");
            }
        }
        return promise::_Void();
    })
    .fail([wptr, this, chatid](const ::promise::Error& err)
    {
        if (wptr.deleted())
        {
            return err;
        }

        mCallStartAttempts.erase(chatid); // remove chatid from CallsAttempts
        return err;
    });
}

void RtcModuleSfu::takeDevice()
{
    if (!mDeviceTakenCount)
    {
        openDevice();
    }

    mDeviceTakenCount++;
}

void RtcModuleSfu::releaseDevice()
{
    if (mDeviceTakenCount > 0)
    {
        mDeviceTakenCount--;
        if (mDeviceTakenCount == 0)
        {
            assert(mVideoDevice);
            closeDevice();
        }
    }
}

void RtcModuleSfu::addLocalVideoRenderer(karere::Id chatid, IVideoRenderer *videoRederer)
{
    mRenderers[chatid] = std::unique_ptr<IVideoRenderer>(videoRederer);
}

void RtcModuleSfu::removeLocalVideoRenderer(karere::Id chatid)
{
    mRenderers.erase(chatid);
}

std::vector<karere::Id> RtcModuleSfu::chatsWithCall()
{
    std::vector<karere::Id> chats;
    for (const auto& call : mCalls)
    {
        chats.push_back(call.second->getChatid());
    }

    return chats;
}

unsigned int RtcModuleSfu::getNumCalls()
{
    return static_cast<unsigned int>(mCalls.size());
}

const std::string& RtcModuleSfu::getVideoDeviceSelected() const
{
    return mVideoDeviceSelected;
}

sfu::SfuClient& RtcModuleSfu::getSfuClient()
{
    return (*mSfuClient.get());
}

DNScache& RtcModuleSfu::getDnsCache()
{
    return mDnsCache;
}

void RtcModuleSfu::removeCall(karere::Id chatid, EndCallReason reason, TermCode connectionTermCode)
{
    Call *call = static_cast<Call*>(findCallByChatid(chatid));
    if (call)
    {
        if (call->getState() > kStateClientNoParticipating && call->getState() <= kStateInProgress)
        {
            call->onCallDisconnect(connectionTermCode,
                             std::string("disconnect done from removeCall, reason: ") + call->endCallReasonToString(reason),
                                   true,
                                   false); // no need to clear participants as call dtor will do it
        }

        // upon kStateDestroyed state change (in call dtor) mEndCallReason will be notified through onCallStateChange
        RTCM_LOG_WARNING("Removing call with callid: %s", call->getCallid().toString().c_str());
        call->setEndCallReason(reason);
        mCalls.erase(call->getCallid());
    }
}

void RtcModuleSfu::handleJoinedCall(karere::Id /*chatid*/, karere::Id callid, const std::set<karere::Id> &usersJoined)
{
    mCalls[callid]->joinedCallUpdateParticipants(usersJoined);
}

void RtcModuleSfu::handleLeftCall(karere::Id /*chatid*/, karere::Id callid, const std::set<karere::Id> &usersLeft)
{
    for (const karere::Id &peer : usersLeft)
    {
        mCalls[callid]->removeParticipant(peer);
    }
}

void RtcModuleSfu::handleNewCall(karere::Id chatid, karere::Id callerid, karere::Id callid, bool isRinging, bool isGroup, std::shared_ptr<std::string> callKey)
{
    mCalls[callid] = ::mega::make_unique<Call>(callid, chatid, callerid, isRinging, mCallHandler, mMegaApi, (*this), isGroup, callKey);
    mCalls[callid]->setState(kStateClientNoParticipating);
}

void RtcModuleSfu::OnFrame(const webrtc::VideoFrame &frame)
{
    auto wptr = weakHandle();
    karere::marshallCall([wptr, this, frame]()
    {
        if (wptr.deleted())
        {
            return;
        }

        for (auto& render : mRenderers)
        {
            ICall* call = findCallByChatid(render.first);
            if ((call && call->getLocalAvFlags().videoCam() && !call->getLocalAvFlags().has(karere::AvFlags::kOnHold)) || !call)
            {
                assert(render.second != nullptr);
                void* userData = NULL;
                auto buffer = frame.video_frame_buffer()->ToI420();   // smart ptr type changed
                if (frame.rotation() != webrtc::kVideoRotation_0)
                {
                    buffer = webrtc::I420Buffer::Rotate(*buffer, frame.rotation());
                }
                unsigned short width = (unsigned short)buffer->width();
                unsigned short height = (unsigned short)buffer->height();
                void* frameBuf = render.second->getImageBuffer(width, height, userData);
                if (!frameBuf) //image is frozen or app is minimized/covered
                    return;
                libyuv::I420ToABGR(buffer->DataY(), buffer->StrideY(),
                                   buffer->DataU(), buffer->StrideU(),
                                   buffer->DataV(), buffer->StrideV(),
                                   (uint8_t*)frameBuf, width * 4, width, height);

                render.second->frameComplete(userData);
            }
        }
    }, mAppCtx);

}

artc::VideoManager *RtcModuleSfu::getVideoDevice()
{
    return mVideoDevice;
}

void RtcModuleSfu::changeDevice(const std::string &device, bool shouldOpen)
{
    if (mVideoDevice)
    {
        shouldOpen = true;
        closeDevice();
    }

    mVideoDeviceSelected = device;
    if (shouldOpen)
    {
        openDevice();
    }
}

void RtcModuleSfu::openDevice()
{
    std::string videoDevice = mVideoDeviceSelected; // get default video device
    if (videoDevice.empty())
    {
        RTCM_LOG_WARNING("Default video in device is not set");
        assert(false);
        std::set<std::pair<std::string, std::string>> videoDevices = artc::VideoManager::getVideoDevices();
        if (videoDevices.empty())
        {
            RTCM_LOG_WARNING("openDevice(): no video devices available");
            return;
        }

        videoDevice = videoDevices.begin()->second;
    }

    webrtc::VideoCaptureCapability capabilities;
    capabilities.width = RtcConstant::kHiResWidth;
    capabilities.height = RtcConstant::kHiResHeight;
    capabilities.maxFPS = RtcConstant::kHiResMaxFPS;

    mVideoDevice = artc::VideoManager::Create(capabilities, videoDevice, artc::gWorkerThread.get());
    mVideoDevice->openDevice(videoDevice);
    rtc::VideoSinkWants wants;
    mVideoDevice->AddOrUpdateSink(this, wants);
}

void RtcModuleSfu::closeDevice()
{
    if (mVideoDevice)
    {
        mVideoDevice->RemoveSink(this);
        mVideoDevice->releaseDevice();
        mVideoDevice = nullptr;
    }
}

void *RtcModuleSfu::getAppCtx()
{
    return mAppCtx;
}

std::string RtcModuleSfu::getDeviceInfo() const
{
    // UserAgent Format
    // MEGA<app>/<version> (platform) Megaclient/<version>
    std::string userAgent = mMegaApi.sdk.getUserAgent();

    std::string androidId = "MEGAAndroid";
    std::string iosId = "MEGAiOS";
    std::string testChatId = "MEGAChatTest";
    std::string syncId = "MEGAsync";
    std::string qtAppId = "MEGAChatQtApp";
    std::string megaClcId = "MEGAclc";

    std::string deviceType = "n";
    std::string version = "0";

    size_t endTypePosition = std::string::npos;
    size_t idPosition;
    if ((idPosition = userAgent.find(androidId)) != std::string::npos)
    {
        deviceType = "na";
        endTypePosition = idPosition + androidId.size() + 1; // remove '/'
    }
    else if ((idPosition = userAgent.find(iosId)) != std::string::npos)
    {
        deviceType = "ni";
        endTypePosition = idPosition + iosId.size() + 1;  // remove '/'
    }
    else if ((idPosition = userAgent.find(testChatId)) != std::string::npos)
    {
        deviceType = "nct";
    }
    else if ((idPosition = userAgent.find(syncId)) != std::string::npos)
    {
        deviceType = "nsync";
        endTypePosition = idPosition + syncId.size() + 1;  // remove '/'
    }
    else if ((idPosition = userAgent.find(qtAppId)) != std::string::npos)
    {
        deviceType = "nqtApp";
    }
    else if ((idPosition = userAgent.find(megaClcId)) != std::string::npos)
    {
        deviceType = "nclc";
    }

    size_t endVersionPosition = userAgent.find(" (");
    if (endVersionPosition != std::string::npos &&
            endTypePosition != std::string::npos &&
            endVersionPosition > endTypePosition)
    {
        version = userAgent.substr(endTypePosition, endVersionPosition - endTypePosition);
    }

    return deviceType + ":" + version;
}

RtcModule* createRtcModule(MyMegaApi &megaApi, rtcModule::CallHandler &callHandler,
                           DNScache &dnsCache, WebsocketsIO& websocketIO, void *appCtx,
                           rtcModule::RtcCryptoMeetings* rRtcCryptoMeetings)
{
    return new RtcModuleSfu(megaApi, callHandler, dnsCache, websocketIO, appCtx,
                            rRtcCryptoMeetings);
}

Slot::Slot(Call &call, rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver)
    : mCall(call)
    , mTransceiver(transceiver)
{
}

Slot::~Slot()
{
    if (mTransceiver->receiver())
    {
       rtc::scoped_refptr<webrtc::FrameDecryptorInterface> decryptor = mTransceiver->receiver()->GetFrameDecryptor();
       if (decryptor)
       {
           static_cast<artc::MegaDecryptor*>(decryptor.get())->setTerminating();
       }
    }

    if (mTransceiver->sender())
    {
        rtc::scoped_refptr<webrtc::FrameEncryptorInterface> encryptor = mTransceiver->sender()->GetFrameEncryptor();
        if (encryptor)
        {
            static_cast<artc::MegaEncryptor*>(encryptor.get())->setTerminating();
        }
    }
}

uint32_t Slot::getTransceiverMid() const
{
    if (!mTransceiver->mid())
    {
        assert(false);
        RTCM_LOG_WARNING("We have received a transceiver without 'mid'");
        return 0;
    }

    return static_cast<uint32_t>(atoi(mTransceiver->mid()->c_str()));
}

void RemoteSlot::release()
{
    if (!mCid)
    {
        return;
    }

    mIv = 0;
    mCid = 0;

    enableTrack(false, kRecv);
    rtc::scoped_refptr<webrtc::FrameDecryptorInterface> decryptor = getTransceiver()->receiver()->GetFrameDecryptor();
    static_cast<artc::MegaDecryptor*>(decryptor.get())->setTerminating();
    getTransceiver()->receiver()->SetFrameDecryptor(nullptr);
}

void RemoteSlot::assign(Cid_t cid, IvStatic_t iv)
{
    assert(!mCid);
    createDecryptor(cid, iv);
    enableTrack(true, kRecv);
}

void RemoteSlot::createDecryptor(Cid_t cid, IvStatic_t iv)
{
    mCid = cid;
    mIv = iv;

    auto it = mCall.getSessions().find(mCid);
    if (it == mCall.getSessions().end())
    {
        mCall.logError("createDecryptor: unknown cid");
        return;
    }

    mTransceiver->receiver()->SetFrameDecryptor(new artc::MegaDecryptor(it->second->getPeer(),
                                                                      mCall.getSfuClient().getRtcCryptoMeetings(),
                                                                      mIv, getTransceiverMid()));
}

RemoteSlot::RemoteSlot(Call& call, rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver, void* appCtx)
    : Slot(call, transceiver), mAppCtx(appCtx)
{
}

void RemoteSlot::enableTrack(bool enable, TrackDirection direction)
{
    assert(mTransceiver);
    if (direction == kRecv)
    {
        mTransceiver->receiver()->track()->set_enabled(enable);
    }
    else if (direction == kSend)
    {
        mTransceiver->sender()->track()->set_enabled(enable);
    }
}

LocalSlot::LocalSlot(Call& call, rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver)
    : Slot(call, transceiver)
{
}

void LocalSlot::createEncryptor()
{
    mTransceiver->sender()->SetFrameEncryptor(new artc::MegaEncryptor(mCall.getMyPeer(),
                                                                      mCall.getSfuClient().getRtcCryptoMeetings(),
                                                                      mIv, getTransceiverMid()));
}

void LocalSlot::generateRandomIv()
{
    randombytes_buf(&mIv, sizeof(mIv));
}

RemoteVideoSlot::RemoteVideoSlot(Call& call, rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver, void* appCtx)
    : RemoteSlot(call, transceiver, appCtx)
    , VideoSink(appCtx)
{
    webrtc::VideoTrackInterface* videoTrack =
            static_cast<webrtc::VideoTrackInterface*>(mTransceiver->receiver()->track().get());

    assert(videoTrack);
    rtc::VideoSinkWants wants;
    videoTrack->AddOrUpdateSink(this, wants);
}

RemoteVideoSlot::~RemoteVideoSlot()
{
}

VideoSink::VideoSink(void* appCtx) :mAppCtx(appCtx)
{

}

VideoSink::~VideoSink()
{

}

void VideoSink::setVideoRender(IVideoRenderer *videoRenderer)
{
    mRenderer = std::unique_ptr<IVideoRenderer>(videoRenderer);
}

void VideoSink::OnFrame(const webrtc::VideoFrame &frame)
{
    auto wptr = weakHandle();
    karere::marshallCall([wptr, this, frame]()
    {
        if (wptr.deleted())
        {
            return;
        }

        if (mRenderer)
        {
            void* userData = NULL;
            auto buffer = frame.video_frame_buffer()->ToI420();   // smart ptr type changed
            if (frame.rotation() != webrtc::kVideoRotation_0)
            {
                buffer = webrtc::I420Buffer::Rotate(*buffer, frame.rotation());
            }
            unsigned short width = (unsigned short)buffer->width();
            unsigned short height = (unsigned short)buffer->height();
            void* frameBuf = mRenderer->getImageBuffer(width, height, userData);
            if (!frameBuf) //image is frozen or app is minimized/covered
                return;
            libyuv::I420ToABGR(buffer->DataY(), buffer->StrideY(),
                               buffer->DataU(), buffer->StrideU(),
                               buffer->DataV(), buffer->StrideV(),
                               (uint8_t*)frameBuf, width * 4, width, height);
            mRenderer->frameComplete(userData);
        }
    }, mAppCtx);
}

void RemoteVideoSlot::assignVideoSlot(Cid_t cid, IvStatic_t iv, VideoResolution videoResolution)
{
    assert(mVideoResolution == kUndefined);
    assign(cid, iv);
    mVideoResolution = videoResolution;
}

void RemoteVideoSlot::release()
{
    RemoteSlot::release();
    mVideoResolution = VideoResolution::kUndefined;
}

VideoResolution RemoteVideoSlot::getVideoResolution() const
{
    return mVideoResolution;
}

bool RemoteVideoSlot::hasTrack()
{
    assert(mTransceiver);

    if (mTransceiver->receiver())
    {
        return  mTransceiver->receiver()->track();
    }

    return false;

}

void RemoteVideoSlot::enableTrack()
{
    webrtc::VideoTrackInterface* videoTrack =
            static_cast<webrtc::VideoTrackInterface*>(mTransceiver->receiver()->track().get());
    videoTrack->set_enabled(true);
}

RemoteAudioSlot::RemoteAudioSlot(Call &call, rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver, void* appCtx)
    : RemoteSlot(call, transceiver, appCtx)
{
}

void RemoteAudioSlot::assignAudioSlot(Cid_t cid, IvStatic_t iv)
{
    assign(cid, iv);
    enableAudioMonitor(true);   // Enable audio monitor
}

void RemoteAudioSlot::enableAudioMonitor(bool enable)
{
    rtc::scoped_refptr<webrtc::MediaStreamTrackInterface> mediaTrack = mTransceiver->receiver()->track();
    webrtc::AudioTrackInterface *audioTrack = static_cast<webrtc::AudioTrackInterface*>(mediaTrack.get());
    assert(audioTrack);
    if (enable && !mAudioLevelMonitorEnabled)
    {
        mAudioLevelMonitorEnabled = true;
        audioTrack->AddSink(mAudioLevelMonitor.get());     // enable AudioLevelMonitor for remote audio detection
    }
    else if (!enable && mAudioLevelMonitorEnabled)
    {
        mAudioLevelMonitorEnabled = false;
        audioTrack->RemoveSink(mAudioLevelMonitor.get()); // disable AudioLevelMonitor
    }
}

void RemoteAudioSlot::createDecryptor(Cid_t cid, IvStatic_t iv)
{
    RemoteSlot::createDecryptor(cid, iv);
    mAudioLevelMonitor.reset(new AudioLevelMonitor(mCall, mAppCtx, static_cast<int32_t>(mCid)));
}

void RemoteAudioSlot::release()
{
    RemoteSlot::release();
    if (mAudioLevelMonitor)
    {
        enableAudioMonitor(false);
        mAudioLevelMonitor = nullptr;
        mAudioLevelMonitorEnabled = false;
    }
}

void globalCleanup()
{
    if (!artc::isInitialized())
        return;
    artc::cleanup();
}

Session::Session(const sfu::Peer& peer)
    : mPeer(peer)
{

}

Session::~Session()
{
    disableAudioSlot();
    disableVideoSlot(kHiRes);
    disableVideoSlot(kLowRes);
    mState = kSessStateDestroyed;
    mSessionHandler->onDestroySession(*this);
}

void Session::setSessionHandler(SessionHandler* sessionHandler)
{
    mSessionHandler = std::unique_ptr<SessionHandler>(sessionHandler);
}

void Session::setVideoRendererVthumb(IVideoRenderer *videoRenderer)
{
    if (!mVthumSlot)
    {
        RTCM_LOG_WARNING("setVideoRendererVthumb: There's no low-res slot associated to this session");
        return;
    }

    mVthumSlot->setVideoRender(videoRenderer);
}

void Session::setVideoRendererHiRes(IVideoRenderer *videoRenderer)
{
    if (!mHiresSlot)
    {
        RTCM_LOG_WARNING("setVideoRendererHiRes: There's no hi-res slot associated to this session");
        return;
    }

    mHiresSlot->setVideoRender(videoRenderer);
}

void Session::setAudioDetected(bool audioDetected)
{
    mAudioDetected = audioDetected;
    mSessionHandler->onRemoteAudioDetected(*this);
}

bool Session::hasHighResolutionTrack() const
{
    return mHiresSlot && mHiresSlot->hasTrack();
}

bool Session::hasLowResolutionTrack() const
{
    return mVthumSlot && mVthumSlot->hasTrack();
}

void Session::notifyHiResReceived()
{
    mSessionHandler->onHiResReceived(*this);
}

void Session::notifyLowResReceived()
{
    mSessionHandler->onVThumbReceived(*this);
}

const sfu::Peer& Session::getPeer() const
{
    return mPeer;
}

void Session::setVThumSlot(RemoteVideoSlot *slot)
{
    assert(slot);
    mVthumSlot = slot;
    mSessionHandler->onVThumbReceived(*this);
}

void Session::setHiResSlot(RemoteVideoSlot *slot)
{
    assert(slot);
    mHiresSlot = slot;
    mSessionHandler->onHiResReceived(*this);
}

void Session::setAudioSlot(RemoteAudioSlot *slot)
{
    mAudioSlot = slot;
    setSpeakRequested(false);
}

void Session::addKey(Keyid_t keyid, const std::string &key)
{
    mPeer.addKey(keyid, key);
}

void Session::setAvFlags(karere::AvFlags flags)
{
    assert(mSessionHandler);
    if (flags == mPeer.getAvFlags())
    {
        RTCM_LOG_WARNING("setAvFlags: remote AV flags has not changed");
        return;
    }

    bool onHoldChanged = mPeer.getAvFlags().isOnHold() != flags.isOnHold();
    mPeer.setAvFlags(flags);
    onHoldChanged
        ? mSessionHandler->onOnHold(*this)              // notify session onHold Change
        : mSessionHandler->onRemoteFlagsChanged(*this); // notify remote AvFlags Change
}

RemoteAudioSlot *Session::getAudioSlot()
{
    return mAudioSlot;
}

RemoteVideoSlot *Session::getVthumSlot()
{
    return mVthumSlot;
}

RemoteVideoSlot *Session::getHiResSlot()
{
    return mHiresSlot;
}

void Session::disableAudioSlot()
{
    if (mAudioSlot)
    {
        mAudioSlot->release();
        setAudioSlot(nullptr);
    }
}

void Session::disableVideoSlot(VideoResolution videoResolution)
{
    if ((videoResolution == kHiRes && !mHiresSlot) || (videoResolution == kLowRes && !mVthumSlot))
    {
        return;
    }

    if (videoResolution == kHiRes)
    {
        mHiresSlot->release();
        mHiresSlot = nullptr;
        mSessionHandler->onHiResReceived(*this);
    }
    else
    {
        mVthumSlot->release();
        mVthumSlot = nullptr;
        mSessionHandler->onVThumbReceived(*this);
    }
}

void Session::setSpeakRequested(bool requested)
{
    mHasRequestSpeak = requested;
    mSessionHandler->onAudioRequested(*this);
}

karere::Id Session::getPeerid() const
{
    return mPeer.getPeerid();
}

Cid_t Session::getClientid() const
{
    return mPeer.getCid();
}

SessionState Session::getState() const
{
    return mState;
}

karere::AvFlags Session::getAvFlags() const
{
    return mPeer.getAvFlags();
}

bool Session::isAudioDetected() const
{
    return mAudioDetected;
}

bool Session::hasRequestSpeak() const
{
    return mHasRequestSpeak;
}

AudioLevelMonitor::AudioLevelMonitor(Call &call, void* appCtx, int32_t cid)
    : mCall(call), mCid(cid), mAppCtx(appCtx)
{
}

void AudioLevelMonitor::OnData(const void *audio_data, int bits_per_sample, int /*sample_rate*/, size_t number_of_channels, size_t number_of_frames)
{
    assert(bits_per_sample == 16);
    time_t nowTime = time(NULL);
    if (nowTime - mPreviousTime > 2) // Two seconds between samples
    {
        mPreviousTime = nowTime;
        size_t valueCount = number_of_channels * number_of_frames;
        int16_t *data = (int16_t*)audio_data;
        int16_t audioMaxValue = data[0];
        int16_t audioMinValue = data[0];
        for (size_t i = 1; i < valueCount; i++)
        {
            if (data[i] > audioMaxValue)
            {
                audioMaxValue = data[i];
            }

            if (data[i] < audioMinValue)
            {
                audioMinValue = data[i];
            }
        }

        bool audioDetected = (abs(audioMaxValue) + abs(audioMinValue) > kAudioThreshold);

        auto wptr = weakHandle();
        karere::marshallCall([wptr, this, audioDetected]()
        {
            if (wptr.deleted())
            {
                return;
            }

            if (!hasAudio())
            {
                if (mAudioDetected)
                {
                    onAudioDetected(false);
                }

                return;
            }

            if (audioDetected != mAudioDetected)
            {
                onAudioDetected(mAudioDetected);
            }

        }, mAppCtx);
    }
}

bool AudioLevelMonitor::hasAudio()
{
    Session *sess = mCall.getSession(static_cast<Cid_t>(mCid));
    if (sess)
    {
        return sess->getAvFlags().audio();
    }
    return false;
}

void AudioLevelMonitor::onAudioDetected(bool audioDetected)
{
    mAudioDetected = audioDetected;
    assert(mCall.getSession(static_cast<Cid_t>(mCid)));
    Session *sess = mCall.getSession(static_cast<Cid_t>(mCid));
    sess->setAudioDetected(mAudioDetected);
}
}
#endif
