#include <mega/types.h>
#include <rtcmPrivate.h>
#include <webrtcPrivate.h>
#include "chatClient.h"
#include <api/video/i420_buffer.h>
#include <libyuv/convert.h>

namespace rtcModule
{

Call::Call(karere::Id callid, karere::Id chatid, IGlobalCallHandler &globalCallHandler, MyMegaApi& megaApi, sfu::SfuClient &sfuClient)
    : mCallid(callid)
    , mChatid(chatid)
    , mState(kStateInitial)
    , mGlobalCallHandler(globalCallHandler)
    , mMegaApi(megaApi)
    , mSfuClient(sfuClient)
    , mMyPeer()
{
    mGlobalCallHandler.onNewCall(*this);
}

Call::~Call()
{
    mState = kStateDestroyed;
    mGlobalCallHandler.onEndCall(*this);
}

karere::Id Call::getCallid() const
{
    return mCallid;
}

karere::Id Call::getChatid() const
{
    return mChatid;
}

void Call::setState(CallState state)
{
    mState = state;
    mCallHandler->onCallStateChange(*this);
}

CallState Call::getState() const
{
    return mState;
}

void Call::addParticipant(karere::Id peer)
{
    mParticipants.push_back(peer);
    mGlobalCallHandler.onAddPeer(*this, peer);
}

void Call::removeParticipant(karere::Id peer)
{
    for (auto itPeer = mParticipants.begin(); itPeer != mParticipants.end(); itPeer++)
    {
        if (*itPeer == peer)
        {
            mParticipants.erase(itPeer);
            mGlobalCallHandler.onRemovePeer(*this, peer);
            return;
        }
    }

    assert(false);
    return;
}

void Call::hangup()
{
    disconnect(TermCode::kUserHangup);
}

promise::Promise<void> Call::join()
{
    setState(CallState::kStateJoining);
    auto wptr = weakHandle();
    return mMegaApi.call(&::mega::MegaApi::joinChatCall, mChatid, mCallid)
    .then([wptr, this](ReqResult result)
    {
        wptr.throwIfDeleted();
        std::string sfuUrl = result->getText();
        connectSfu(sfuUrl);
    });
}

bool Call::participate()
{
    return (mState > kStateUserNoParticipating && mState < kStateTerminatingUserParticipation);
}

void Call::enableAudioLevelMonitor(bool enable)
{

}

void Call::ignoreCall()
{

}

bool Call::isRinging() const
{
    return mIsRinging;
}

bool Call::isModerator() const
{
    return mMyPeer.getModerator();
}

void Call::setCallHandler(CallHandler* callHanlder)
{
    mCallHandler = std::unique_ptr<CallHandler>(callHanlder);
}

void Call::setVideoRendererVthumb(IVideoRenderer *videoRederer)
{
    mVThumb->setVideoRender(videoRederer);
}

void Call::setVideoRendererHiRes(IVideoRenderer *videoRederer)
{
    mHiRes->setVideoRender(videoRederer);
}

void Call::requestModerator()
{
    if (mModeratorRequested)
    {
        return;
    }

    mModeratorRequested = true;
    mSfuConnection->sendModeratorRequested();
}

void Call::requestSpeaker(bool add)
{
    if (!mSpeakerRequested && add)
    {
        mSpeakerRequested = true;
        mSfuConnection->sendSpeakReq();
        return;
    }

    if (mSpeakerRequested && !add)
    {
        mSpeakerRequested = false;
        mSfuConnection->sendSpeakReqDel();
        return;
    }
}

bool Call::isSpeakAllow()
{
    assert(false);
}

void Call::approveSpeakRequest(Cid_t cid, bool allow)
{
    assert(mMyPeer.getModerator());
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
        assert(mMyPeer.getModerator());
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
        if (session.second->hasRequestedSpeaker())
        {
            speakerRequested.push_back(session.first);
        }
    }

    return speakerRequested;
}

void Call::requestHighResolutionVideo(Cid_t cid)
{
    bool hasVthumb = false;
    for (const auto& session : mSessions)
    {
        if (session.second->getVthumSlot()->getCid() == cid)
        {
            hasVthumb = true;
            break;
        }
    }

    mSfuConnection->sendGetHiRes(cid, hasVthumb);
}

void Call::stopHighResolutionVideo(Cid_t cid)
{
    mSfuConnection->sendDelHiRes(cid);
}

void Call::connectSfu(const std::string &sfuUrl)
{
    setState(CallState::kStateConnecting);
    mSfuUrl = sfuUrl;
    mSfuConnection = mSfuClient.generateSfuConnection(mChatid, sfuUrl, *this);
    auto wptr = weakHandle();
    mSfuConnection->getPromiseConnection()
    .then([wptr, this]()
    {
        setState(CallState::kStateJoining);
        if (wptr.deleted())
        {
            return;
        }

        webrtc::PeerConnectionInterface::IceServers iceServer;
        mRtcConn = artc::myPeerConnection<Call>(iceServer, *this);

        createTranceiver();
        getLocalStreams();

        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        options.offer_to_receive_audio = webrtc::PeerConnectionInterface::RTCOfferAnswerOptions::kMaxOfferToReceiveMedia;
        options.offer_to_receive_video = webrtc::PeerConnectionInterface::RTCOfferAnswerOptions::kMaxOfferToReceiveMedia;
        auto wptr = weakHandle();
        mRtcConn.createOffer(options)
        .then([wptr, this](webrtc::SessionDescriptionInterface* sdp) -> promise::Promise<void>
        {
            if (wptr.deleted())
                return ::promise::_Void();

            KR_THROW_IF_FALSE(sdp->ToString(&mSdp));
            return mRtcConn.setLocalDescription(sdp);
        })
        .then([wptr, this]()
        {
            if (wptr.deleted())
            {
                return;
            }

            //TODO Compress sdp

            std::map<int, std::string> ivs;
            // TODO binary to HEX
//            ivs[0] = mVThumb->mIv;
//            ivs[1] = mHiRes->mIv;
//            ivs[2] = mAudio->mIv;
            int avFlags = 0;
            mSfuConnection->joinSfu(mSdp, ivs, avFlags);
        })
        .fail([wptr, this](const ::promise::Error& err)
        {
            if (wptr.deleted())
                return;
            disconnect(TermCode::kErrSdp, std::string("Error creating SDP offer: ") + err.msg());
        });

    });
}

void Call::createTranceiver()
{
    webrtc::RtpTransceiverInit transceiverInit;
    transceiverInit.direction = webrtc::RtpTransceiverDirection::kSendRecv;
    webrtc::RTCErrorOr<rtc::scoped_refptr<webrtc::RtpTransceiverInterface>> err
            = mRtcConn->AddTransceiver(cricket::MediaType::MEDIA_TYPE_VIDEO, transceiverInit);

    if (err.ok())
    {
        mVThumb = ::mega::make_unique<VideoSlot>(*this, err.MoveValue());
    }

    err = mRtcConn->AddTransceiver(cricket::MediaType::MEDIA_TYPE_VIDEO, transceiverInit);
    mHiRes = ::mega::make_unique<VideoSlot>(*this, err.MoveValue());

    err = mRtcConn->AddTransceiver(cricket::MediaType::MEDIA_TYPE_AUDIO, transceiverInit);
    mAudio = ::mega::make_unique<Slot>(*this, err.MoveValue());

    for (int i = 0; i < RtcConstant::kMaxCallAudioSenders; i++)
    {
        webrtc::RtpTransceiverInit transceiverInit;
        transceiverInit.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
        mRtcConn->AddTransceiver(cricket::MediaType::MEDIA_TYPE_AUDIO, transceiverInit);
    }

    for (int i = 0; i < RtcConstant::kMaxCallVideoSenders; i++)
    {
        webrtc::RtpTransceiverInit transceiverInit;
        transceiverInit.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
        mRtcConn->AddTransceiver(cricket::MediaType::MEDIA_TYPE_VIDEO, transceiverInit);
    }
}

void Call::getLocalStreams()
{
    rtc::scoped_refptr<webrtc::AudioTrackInterface> audioTrack =
            artc::gWebrtcContext->CreateAudioTrack("a"+std::to_string(artc::generateId()), artc::gWebrtcContext->CreateAudioSource(cricket::AudioOptions()));

    mAudio->getTransceiver()->sender()->SetTrack(audioTrack);

    rtc::scoped_refptr<webrtc::VideoTrackInterface> videoTrack;
    webrtc::VideoCaptureCapability capabilities;
    capabilities.width = RtcConstant::kHiResWidth;
    capabilities.height = RtcConstant::kHiResHeight;
    capabilities.maxFPS = RtcConstant::kHiResMaxFPS;
    std::set<std::pair<std::string, std::string>> videoDevices = artc::VideoManager::getVideoDevices();
    mVideoDevice = artc::VideoManager::Create(capabilities, videoDevices.begin()->second, artc::gAsyncWaiter->guiThread());
    videoTrack = artc::gWebrtcContext->CreateVideoTrack("v"+std::to_string(artc::generateId()), mVideoDevice->getVideoTrackSource());
    mHiRes->getTransceiver()->sender()->SetTrack(videoTrack);
    rtc::VideoSinkWants wants;
    static_cast<webrtc::VideoTrackInterface*>(mHiRes->getTransceiver()->sender()->track().get())->AddOrUpdateSink(mHiRes.get(), wants);

    mVThumb->getTransceiver()->sender()->SetTrack(videoTrack);
    webrtc::RtpParameters parameters;
    webrtc::RtpEncodingParameters encoding;
    double scale = static_cast<double>(RtcConstant::kHiResWidth) / static_cast<double>(RtcConstant::kVthumbWidth);
    encoding.scale_resolution_down_by = scale;
    encoding.max_bitrate_bps = 100 * 1024;
    parameters.encodings.push_back(encoding);
    mVThumb->getTransceiver()->sender()->SetParameters(parameters);
    static_cast<webrtc::VideoTrackInterface*>(mVThumb->getTransceiver()->sender()->track().get())->AddOrUpdateSink(mVThumb.get(), wants);
    mVideoDevice->openDevice(videoDevices.begin()->second);
}

void Call::disconnect(TermCode termCode, const std::string &msg)
{
    mVideoDevice->releaseDevice();
    mSessions.clear();
    mVThumb.reset(nullptr);
    mHiRes.reset(nullptr);
    mAudio.reset(nullptr);
    mReceiverTracks.clear();
    setState(CallState::kStateTerminatingUserParticipation);
}

std::string Call::getKeyFromPeer(Cid_t cid, Keyid_t keyid)
{
    return mSessions[cid]->getPeer().getKey(keyid);
}

bool Call::handleAvCommand(Cid_t cid, int av)
{
    mSessions[cid]->setAvFlags(av);
    return true;
}

bool Call::handleAnswerCommand(Cid_t cid, const std::string& spdString, int mod,  const std::vector<sfu::Peer>&peers, const std::map<Cid_t, sfu::VideoTrackDescriptor>&vthumbs, const std::map<Cid_t, sfu::SpeakersDescriptor>&speakers)
{
    mMyPeer.init(cid, mSfuClient.getKarereClient().myHandle(), 0, mod);
    if (mMyPeer.getModerator())
    {
        mSpeakAllow = true;
    }

    for (const sfu::Peer& peer : peers)
    {
        mSessions[cid] = ::mega::make_unique<Session>(peer);
        mCallHandler->onNewSession(*mSessions[cid]);
    }

    generateAndSendNewkey();

    //TODO Uncompress sdp
    webrtc::SdpParseError error;
    webrtc::SessionDescriptionInterface *sdp = webrtc::CreateSessionDescription("answer", spdString, &error);
    if (!sdp)
    {
        disconnect(TermCode::kErrSdp, "Error parsing peer SDP answer: line="+error.line+"\nError: "+error.description);
        return false;
    }

    auto wptr = weakHandle();
    mRtcConn.setRemoteDescription(sdp)
    .then([wptr, this, vthumbs, speakers]()
    {
        if (wptr.deleted())
            return;

        //TODO setThumbVtrackResScale()

        handleIncomingVideo(vthumbs);

        for(auto speak : speakers)
        {
            Cid_t cid = speak.first;
            const sfu::SpeakersDescriptor& speakerDecriptor = speak.second;
            addSpeaker(cid, speakerDecriptor);
        }

        setState(CallState::kStateInProgress);
    })
    .fail([wptr, this](const ::promise::Error& err)
    {
        if (wptr.deleted())
            return;

        std::string msg = "Error setting SDP answer: " + err.msg();
        disconnect(TermCode::kErrSdp, msg);
    });

    return true;
}

bool Call::handleKeyCommand(Keyid_t keyid, Cid_t cid, const std::string &key)
{
    mSessions[cid]->addKey(keyid, key);
    return true;
}

bool Call::handleVThumbsCommand(const std::map<Cid_t, sfu::VideoTrackDescriptor> &videoTrackDescriptors)
{
    handleIncomingVideo(videoTrackDescriptors);
    return true;
}

bool Call::handleVThumbsStartCommand()
{
    mVThumb->enableTrack(true);
    return true;
}

bool Call::handleVThumbsStopCommand()
{
    mVThumb->enableTrack(false);
    return true;
}

bool Call::handleHiResCommand(const std::map<Cid_t, sfu::VideoTrackDescriptor>& videoTrackDescriptors)
{
    handleIncomingVideo(videoTrackDescriptors, true);
    return true;
}

bool Call::handleHiResStartCommand()
{
    mHiRes->enableTrack(true);
    return true;
}

bool Call::handleHiResStopCommand()
{
    mHiRes->enableTrack(false);
    return true;
}

bool Call::handleSpeakReqsCommand(const std::vector<Cid_t> &speakRequests)
{
    for (Cid_t cid : speakRequests)
    {
        mSessions[cid]->setSpeakRequested(true);
    }

    return true;
}

bool Call::handleSpeakReqDelCommand(Cid_t cid)
{
    if (cid)
    {
        mSessions[cid]->setSpeakRequested(false);
    }
    else
    {
        mSpeakAllow = false;
        mSpeakerRequested = false;
    }

    return true;
}

bool Call::handleSpeakOnCommand(Cid_t cid, sfu::SpeakersDescriptor speaker)
{
    if (cid)
    {
        addSpeaker(cid, speaker);
    }
    else
    {
        mSpeakAllow = true;
        mAudio->enableTrack(true);
    }

    return true;
}

bool Call::handleSpeakOffCommand(Cid_t cid)
{
    if (cid)
    {
        removeSpeaker(cid);
    }
    else
    {
        mSpeakAllow = false;
        mAudio->enableTrack(false);
    }

    return true;
}

void Call::onError()
{

}

void Call::onAddStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> stream)
{
    mVThumb->createDecryptor();
    mVThumb->createEncryptor(getMyPeer());

    mHiRes->createDecryptor();
    mHiRes->createEncryptor(getMyPeer());

    mAudio->createDecryptor();
    mAudio->createEncryptor(getMyPeer());
}

void Call::onRemoveStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> stream)
{
}

void Call::onIceCandidate(std::shared_ptr<artc::IceCandText> cand)
{

}

void Call::onIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState state)
{
    if (state == webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionFailed)
    {
        // force reconnect
    }
}

void Call::onIceComplete()
{

}

void Call::onSignalingChange(webrtc::PeerConnectionInterface::SignalingState newState)
{

}

void Call::onDataChannel(webrtc::DataChannelInterface *data_channel)
{

}

void Call::onTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver)
{
    absl::optional<std::string> mid = transceiver->mid();
    assert(transceiver->direction() == webrtc::RtpTransceiverDirection::kRecvOnly);
    if (mid.has_value())
    {
        if (transceiver->media_type() == cricket::MediaType::MEDIA_TYPE_AUDIO)
        {
            mReceiverTracks[mid.value()] = ::mega::make_unique<Slot>(*this, transceiver);
        }
        else
        {
            mReceiverTracks[mid.value()] = ::mega::make_unique<VideoSlot>(*this, transceiver);
        }

        mReceiverTracks[mid.value()]->createDecryptor();
    }
}

void Call::onRenegotiationNeeded()
{

}

void Call::generateAndSendNewkey()
{
    // Generate key

    //encrypt key for all peers in mPeers store in dataKey
    std::string dataKey;
    uint64_t id = -1;
    mSfuConnection->sendKey(id, dataKey);
}

void Call::handleIncomingVideo(const std::map<Cid_t, sfu::VideoTrackDescriptor> &videotrackDescriptors, bool hiRes)
{
    for (auto trackDescriptor : videotrackDescriptors)
    {
        auto it = mReceiverTracks.find(trackDescriptor.second.mMid);
        if (it == mReceiverTracks.end())
        {
            RTCM_LOG_ERROR("Unknown vtrack mid %s", trackDescriptor.second.mMid.c_str());
            return;
        }

        webrtc::VideoTrackInterface* videoTrack =
                static_cast<webrtc::VideoTrackInterface*>(it->second.get()->getTransceiver()->receiver()->track().get());

        rtc::VideoSinkWants opts;
        VideoSlot* slot = static_cast<VideoSlot*>(it->second.get());
        Cid_t cid = trackDescriptor.first;
        slot->setParams(cid, trackDescriptor.second.mIv);
        slot->createDecryptor();

        if (hiRes)
        {
            mSessions[cid]->setHiResSlot(slot);
        }
        else
        {
            mSessions[cid]->setVThumSlot(slot);
        }
    }
}

void Call::addSpeaker(Cid_t cid, const sfu::SpeakersDescriptor &speaker)
{
    if (mSessions.find(cid) == mSessions.end())
    {
        RTCM_LOG_ERROR("AddSpeaker: unknown cid");
        return;
    }

    auto it = mReceiverTracks.find(speaker.mMid);
    if (it == mReceiverTracks.end())
    {
        RTCM_LOG_ERROR("AddSpeaker: unknown mid");
        return;
    }

    Slot* slot = it->second.get();
    slot->enableTrack(true);
    slot->setParams(cid, speaker.mIv);
    slot->createDecryptor();

    mSessions[cid]->setAudioSlot(slot);
}

void Call::removeSpeaker(Cid_t cid)
{
    auto it = mSessions.find(cid);
    if (it == mSessions.end())
    {
        RTCM_LOG_ERROR("removeSpeaker: unknown cid");
        return;
    }

    Slot* slot = it->second->getAudioSlot();
    slot->enableTrack(false);
    it->second->setAudioSlot(nullptr);
}

sfu::Peer& Call::getMyPeer()
{
    return mMyPeer;
}

const std::string& Call::getCallKey() const
{
    return mCallKey;
}

RtcModuleSfu::RtcModuleSfu(MyMegaApi &megaApi, IGlobalCallHandler &callhandler, IRtcCrypto *crypto, const char *iceServers)
    : mCallHandler(callhandler)
    , mMegaApi(megaApi)
{
}

void RtcModuleSfu::init(WebsocketsIO& websocketIO, void *appCtx, rtcModule::RtcCryptoMeetings* rRtcCryptoMeetings, karere::Client& karereClient)
{
    mSfuClient = ::mega::make_unique<sfu::SfuClient>(websocketIO, appCtx, rRtcCryptoMeetings, karereClient);
    if (!artc::isInitialized())
    {
        artc::init(appCtx);
        RTCM_LOG_ERROR("WebRTC stack initialized before first use");
    }
}

void RtcModuleSfu::hangupAll()
{

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

ICall *RtcModuleSfu::findCallByChatid(karere::Id chatid)
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

void RtcModuleSfu::loadDeviceList()
{

}

bool RtcModuleSfu::selectVideoInDevice(const std::string &device)
{
    return false;
}

void RtcModuleSfu::getVideoInDevices(std::set<std::string> &devicesVector)
{

}

std::string RtcModuleSfu::getVideoDeviceSelected()
{
    return "";
}

promise::Promise<void> RtcModuleSfu::startCall(karere::Id chatid)
{
    auto wptr = weakHandle();
    mCalls[chatid] = ::mega::make_unique<Call>(chatid, chatid, mCallHandler, mMegaApi, *mSfuClient.get());
    mCalls[chatid]->connectSfu("");
    return promise::Void();

//    return mMegaApi.call(&::mega::MegaApi::startChatCall, chatid)
//    .then([wptr, this, chatid](ReqResult result)
//    {
//        wptr.throwIfDeleted();
//        karere::Id callid = result->getParentHandle();
//        std::string sfuUrl = result->getText();
//        mCallNews[callid] = ::mega::make_unique<Call>(callid, chatid, mCallHandler, mMegaApi, *mSfuClient.get());
//        mCallNews[callid]->connectSfu(sfuUrl);
//    });
}

std::vector<karere::Id> RtcModuleSfu::chatsWithCall()
{
    std::vector<karere::Id> v;
    return v;
}

unsigned int RtcModuleSfu::getNumCalls()
{
    return 0;
}

void RtcModuleSfu::removeCall(karere::Id chatid, TermCode termCode)
{
    Call* call = static_cast<Call*>(findCallByChatid(chatid));
    if (call)
    {
        call->disconnect(termCode);
        mCalls.erase(call->getCallid());
    }
}

void RtcModuleSfu::handleJoinedCall(karere::Id chatid, karere::Id callid, const std::vector<karere::Id> &usersJoined)
{
    for (karere::Id peer : usersJoined)
    {
        mCalls[callid]->addParticipant(peer);
    }
}

void RtcModuleSfu::handleLefCall(karere::Id chatid, karere::Id callid, const std::vector<karere::Id> &usersLeft)
{
    for (karere::Id peer : usersLeft)
    {
        mCalls[callid]->removeParticipant(peer);
    }
}

void RtcModuleSfu::handleCallEnd(karere::Id chatid, karere::Id callid, uint8_t reason)
{
    mCalls.erase(callid);
}

void RtcModuleSfu::handleNewCall(karere::Id chatid, karere::Id callid)
{
    mCalls[callid] = ::mega::make_unique<Call>(callid, chatid, mCallHandler, mMegaApi, *mSfuClient.get());
}

RtcModule* createRtcModule(MyMegaApi &megaApi, IGlobalCallHandler& callhandler, IRtcCrypto* crypto, const char* iceServers)
{
    return new RtcModuleSfu(megaApi, callhandler, crypto, iceServers);
}

Slot::Slot(Call &call, rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver)
    : mCall(call)
    , mTransceiver(transceiver)
{

}

Slot::~Slot()
{

}

void Slot::createEncryptor(const sfu::Peer& peer)
{
    mTransceiver->sender()->SetFrameEncryptor(new artc::MegaEncryptor(mCall.getMyPeer(),
                                                                      mCall.mSfuClient.getRtcCryptoMeetings(),
                                                                      mCall.getCallKey(),
                                                                      mIv));
}

void Slot::createDecryptor()
{
    auto it = mCall.mSessions.find(mCid);
    if (it == mCall.mSessions.end())
    {
        RTCM_LOG_ERROR("createDecryptor: unknown cid");
        return;
    }

    mTransceiver->receiver()->SetFrameDecryptor(new artc::MegaDecryptor(it->second->getPeer(),
                                                                      mCall.mSfuClient.getRtcCryptoMeetings(),
                                                                      mCall.getCallKey(),
                                                                      mIv));
}

webrtc::RtpTransceiverInterface *Slot::getTransceiver()
{
    return mTransceiver.get();
}

Cid_t Slot::getCid() const
{
    return mCid;
}

void Slot::setParams(Cid_t cid, IvStatic_t iv)
{
    mCid = cid;
    mIv = iv;
}

void Slot::enableTrack(bool enable)
{
    if (mTransceiver->direction() == webrtc::RtpTransceiverDirection::kRecvOnly)
    {
        mTransceiver->receiver()->track()->set_enabled(enable);
    }
    else if(mTransceiver->direction() == webrtc::RtpTransceiverDirection::kSendRecv)
    {
        mTransceiver->receiver()->track()->set_enabled(enable);
        mTransceiver->sender()->track()->set_enabled(enable);
    }
}

VideoSlot::VideoSlot(Call& call, rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver)
    : Slot(call, transceiver)
{
}

void VideoSlot::setVideoRender(IVideoRenderer *videoRenderer)
{
    mRenderer = std::unique_ptr<IVideoRenderer>(videoRenderer);
}

void VideoSlot::OnFrame(const webrtc::VideoFrame &frame)
{
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
}

void globalCleanup()
{
    if (!artc::isInitialized())
        return;
    artc::cleanup();
}

Session::Session(const sfu::Peer &peer)
    : mPeer(peer)
{

}

Session::~Session()
{

}

void Session::setSessionHandler(SessionHandler* sessionHandler)
{
    mSessionHandler = sessionHandler;
}

const sfu::Peer& Session::getPeer() const
{
    return mPeer;
}

void Session::setVThumSlot(VideoSlot *slot)
{
    mVthumSlot = slot;
}

void Session::setHiResSlot(VideoSlot *slot)
{
    mHiresSlot = slot;
}

void Session::setAudioSlot(Slot *slot)
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
    mPeer.setAvFlags(flags);
    assert(mSessionHandler);
}

Slot *Session::getAudioSlot()
{
    return mAudioSlot;
}

VideoSlot *Session::getVthumSlot()
{
    return mVthumSlot;
}

VideoSlot *Session::betHiResSlot()
{
    return mHiresSlot;
}

void Session::setSpeakRequested(bool requested)
{
    mSpeakRequest = requested;
}

bool Session::hasRequestedSpeaker() const
{
    return mSpeakRequest;
}

}
