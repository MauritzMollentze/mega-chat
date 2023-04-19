/**
 * @file tests/sdk_test.cpp
 * @brief Mega SDK test file
 *
 * (c) 2016 by Mega Limited, Wellsford, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#ifndef CHATTEST_H
#define CHATTEST_H

#include "megachatapi.h"
#include <chatClient.h>
#include <future>
#include "gtest/gtest.h"

static const std::string APPLICATION_KEY = "MBoVFSyZ";
static const std::string USER_AGENT_DESCRIPTION  = "MEGAChatTest";
static constexpr unsigned int MAX_ATTEMPTS = 3;
static const unsigned int maxTimeout = 600;    // (seconds)
static const unsigned int pollingT = 500000;   // (microseconds) to check if response from server is received
static const unsigned int NUM_ACCOUNTS = 2;

#define TEST_LOG_ERROR(a, message) \
    do { \
        if (!(a)) \
        { \
            postLog(message); \
        } \
    } \
    while(false) \

class MegaLoggerTest : public ::mega::MegaLogger,
        public megachat::MegaChatLogger {

public:
    MegaLoggerTest(const char *filename);
    ~MegaLoggerTest();

    std::ofstream *getOutputStream() { return &testlog; }
    void postLog(const char *message);

private:
    std::ofstream testlog;

protected:
    void log(const char *time, int loglevel, const char *source, const char *message);
    void log(int loglevel, const char *message);
};

class Account
{
public:
    Account();
    Account(const std::string &email, const std::string &password);

    std::string getEmail() const;
    std::string getPassword() const;
private:
    std::string mEmail;
    std::string mPassword;
};

class TestChatRoomListener;

#ifndef KARERE_DISABLE_WEBRTC
class TestChatVideoListener : public megachat::MegaChatVideoListener
{
public:
    TestChatVideoListener();
    virtual ~TestChatVideoListener();

    virtual void onChatVideoData(megachat::MegaChatApi *api, megachat::MegaChatHandle chatid, int width, int height, char *buffer, size_t size);
};
#endif

class RequestListener
{
public:
    bool waitForResponse(unsigned int timeout = maxTimeout);
    virtual int getErrorCode() const = 0;

protected:
    bool mFinished = false;
    mega::MegaApi* mMegaApi = nullptr;
    megachat::MegaChatApi* mMegaChatApi = nullptr;
    RequestListener(mega::MegaApi* megaApi, megachat::MegaChatApi *megaChatApi);
};

class TestMegaRequestListener : public mega::MegaRequestListener, public RequestListener
{
public:
    TestMegaRequestListener(mega::MegaApi* megaApi, megachat::MegaChatApi *megaChatApi);
    ~TestMegaRequestListener();
    void onRequestFinish(mega::MegaApi* api, mega::MegaRequest *request, mega::MegaError* e) override;
    int getErrorCode() const override;
    mega::MegaRequest* getMegaRequest() const;

private:
    mega::MegaRequest *mRequest = nullptr;
    mega::MegaError *mError = nullptr;

};

class TestMegaChatRequestListener : public megachat::MegaChatRequestListener, public RequestListener
{
public:
    TestMegaChatRequestListener(mega::MegaApi *megaApi, megachat::MegaChatApi *megaChatApi);
    ~TestMegaChatRequestListener();
    void onRequestFinish(megachat::MegaChatApi *api, megachat::MegaChatRequest *request, megachat::MegaChatError *e) override;
    int getErrorCode() const override;
    megachat::MegaChatRequest* getMegaChatRequest() const;

private:
    megachat::MegaChatRequest *mRequest = nullptr;
    megachat::MegaChatError *mError = nullptr;
};

class MegaChatApiTest :
        public ::testing::Test,
        public ::mega::MegaListener,
        public ::mega::MegaTransferListener,
        public ::mega::MegaLogger,
        public megachat::MegaChatListener,
        public megachat::MegaChatCallListener,
        public megachat::MegaChatScheduledMeetingListener
{
public:
    MegaChatApiTest();
    ~MegaChatApiTest();

    // Global test environment initialization
    static void init();
    // Global test environment clear up
    static void terminate();

protected:
    static Account& account(unsigned i) { return getEnv().account(i); }
    static MegaLoggerTest* logger() { return getEnv().logger(); }

    // Specific test environment initialization for each test
    void SetUp() override;
    // Specific test environment clear up for each test
    void TearDown() override;

    // email and password parameter is used if you don't want to use default values for accountIndex
    char *login(unsigned int accountIndex, const char *session = NULL, const char *email = NULL, const char *password = NULL);
    void logout(unsigned int accountIndex, bool closeSession = false);

public:
    static const char* printChatRoomInfo(const megachat::MegaChatRoom *);
    static const char* printMessageInfo(const megachat::MegaChatMessage *);
protected:
    static const char* printChatListItemInfo(const megachat::MegaChatListItem *);
public:
    void postLog(const std::string &msg);

protected:
    bool exitWait(const std::vector<bool *>&responsesReceived, bool any) const;
    bool waitForMultiResponse(std::vector<bool *>responsesReceived, bool any, unsigned int timeout = maxTimeout) const;
    bool waitForResponse(bool *responseReceived, unsigned int timeout = maxTimeout) const;

    /**
     * @brief executes an asynchronous action and wait for results
     * @param maxAttempts max number of attempts the action must be retried
     * @param exitFlags vector of conditions that must be accomplished consider action finished
     * @param flagsStr vector of strings to identify each condition
     * @param actionMsg string that defines the action
     * @param waitForAll wait for all exit conditions
     * @param resetFlags flag that indicates if exitFlags must be reset before executing action
     * @param timeout max timeout (in seconds) to execute the action
     * @param action function to be executed
     */
    void waitForAction(int maxAttempts, std::vector<bool*> exitFlags, const std::vector<std::string>& flagsStr, const std::string& actionMsg, bool waitForAll, bool resetFlags, unsigned int timeout, std::function<void()>action);
    void initChat(unsigned int a1, unsigned int a2, mega::MegaUser*& user, megachat::MegaChatHandle& chatid, char*& primarySession, char*& secondarySession, TestChatRoomListener*& chatroomListener);
    int loadHistory(unsigned int accountIndex, megachat::MegaChatHandle chatid, TestChatRoomListener *chatroomListener);
    void makeContact(unsigned int a1, unsigned int a2);
    bool isChatroomUpdated(unsigned int index, megachat::MegaChatHandle chatid);

    /* select a group chat room, by default with PRIV_MODERATOR for primary account
     * in case chat privileges for primary account doesn't matter, provide PRIV_UNKNOWN in priv param */
    megachat::MegaChatHandle getGroupChatRoom(unsigned int a1, unsigned int a2,
                                              megachat::MegaChatPeerList *peers, int a1Priv = megachat::MegaChatPeerList::PRIV_UNKNOWN, bool create = true, bool publicChat = false, const char *title = NULL);

    megachat::MegaChatHandle getPeerToPeerChatRoom(unsigned int a1, unsigned int a2);

    // send msg, wait for confirmation, reception by other side, delivery status. Returns ownership of confirmed msg
    megachat::MegaChatMessage *sendTextMessageOrUpdate(unsigned int senderAccountIndex, unsigned int receiverAccountIndex,
                                               megachat::MegaChatHandle chatid, const std::string& textToSend,
                                               TestChatRoomListener *chatroomListener, megachat::MegaChatHandle messageId = megachat::MEGACHAT_INVALID_HANDLE);

    void checkEmail(unsigned int indexAccount);
    std::string dateToString();
    megachat::MegaChatMessage *attachNode(unsigned int a1, unsigned int a2, megachat::MegaChatHandle chatid,
                                    ::mega::MegaNode *nodeToSend, TestChatRoomListener* chatroomListener);

    void clearHistory(unsigned int a1, unsigned int a2, megachat::MegaChatHandle chatid, TestChatRoomListener *chatroomListener);
    void leaveChat(unsigned int accountIndex, megachat::MegaChatHandle chatid);

    unsigned int getMegaChatApiIndex(megachat::MegaChatApi *api);
    unsigned int getMegaApiIndex(::mega::MegaApi *api);

    void createFile(const std::string &fileName, const std::string &sourcePath, const std::string &contain);
    ::mega::MegaNode *uploadFile(int accountIndex, const std::string &fileName, const std::string &sourcePath, const std::string &targetPath);
    void addTransfer(int accountIndex);
    bool &isNotTransferRunning(int accountIndex);

    bool downloadNode(int accountIndex, ::mega::MegaNode *nodeToDownload);
    bool importNode(int accountIndex, ::mega::MegaNode* node, const std::string& destinationName);

    void getContactRequest(unsigned int accountIndex, bool outgoing, int expectedSize = 1);

    int purgeLocalTree(const std::string& path);
    void purgeCloudTree(unsigned int accountIndex, ::mega::MegaNode* node);
    void clearAndLeaveChats(unsigned int accountIndex, megachat::MegaChatHandle skipChatId =  megachat::MEGACHAT_INVALID_HANDLE);
    void removePendingContactRequest(unsigned int accountIndex);
    void changeLastName(unsigned int accountIndex, std::string lastName);

    ::mega::MegaApi* megaApi[NUM_ACCOUNTS];
    megachat::MegaChatApi* megaChatApi[NUM_ACCOUNTS];

    // flags
    bool requestFlags[NUM_ACCOUNTS][::mega::MegaRequest::TYPE_CHAT_SET_TITLE];
    bool initStateChanged[NUM_ACCOUNTS];
    int initState[NUM_ACCOUNTS];
    bool mChatConnectionOnline[NUM_ACCOUNTS];
    int lastErrorTransfer[NUM_ACCOUNTS];

    megachat::MegaChatRoom *chatroom[NUM_ACCOUNTS];
    bool chatUpdated[NUM_ACCOUNTS];
    bool chatItemUpdated[NUM_ACCOUNTS];
    bool chatItemClosed[NUM_ACCOUNTS];
    bool peersUpdated[NUM_ACCOUNTS];
    bool titleUpdated[NUM_ACCOUNTS];
    bool chatArchived[NUM_ACCOUNTS];

    ::mega::MegaHandle mNodeCopiedHandle[NUM_ACCOUNTS];
    ::mega::MegaHandle mNodeUploadHandle[NUM_ACCOUNTS];

    bool mNotTransferRunning[NUM_ACCOUNTS];
    bool mPresenceConfigUpdated[NUM_ACCOUNTS];
    bool mOnlineStatusUpdated[NUM_ACCOUNTS];
    int mOnlineStatus[NUM_ACCOUNTS];

    ::mega::MegaContactRequest* mContactRequest[NUM_ACCOUNTS];
    bool mContactRequestUpdated[NUM_ACCOUNTS];
    std::map <unsigned int, bool> mUsersChanged[NUM_ACCOUNTS];

#ifndef KARERE_DISABLE_WEBRTC
    bool mCallReceived[NUM_ACCOUNTS];
    bool mCallReceivedRinging[NUM_ACCOUNTS];
    bool mCallInProgress[NUM_ACCOUNTS];
    bool mCallDestroyed[NUM_ACCOUNTS];
    bool mCallConnecting[NUM_ACCOUNTS];
    int mTerminationCode[NUM_ACCOUNTS];
    megachat::MegaChatHandle mChatIdRingInCall[NUM_ACCOUNTS];
    megachat::MegaChatHandle mChatIdInProgressCall[NUM_ACCOUNTS];
    megachat::MegaChatHandle mCallIdRingIn[NUM_ACCOUNTS];
    megachat::MegaChatHandle mCallIdExpectedReceived[NUM_ACCOUNTS];
    megachat::MegaChatHandle mCallIdJoining[NUM_ACCOUNTS];
    megachat::MegaChatHandle mSchedIdUpdated[NUM_ACCOUNTS];
    megachat::MegaChatHandle mSchedIdRemoved[NUM_ACCOUNTS];
    TestChatVideoListener *mLocalVideoListener[NUM_ACCOUNTS];
    TestChatVideoListener *mRemoteVideoListener[NUM_ACCOUNTS];
    bool mChatCallOnHold[NUM_ACCOUNTS];
    bool mChatCallOnHoldResumed[NUM_ACCOUNTS];
    bool mChatCallAudioEnabled[NUM_ACCOUNTS];
    bool mChatCallAudioDisabled[NUM_ACCOUNTS];
    bool mChatCallSessionStatusInProgress[NUM_ACCOUNTS];
    bool mChatSessionWasDestroyed[NUM_ACCOUNTS];
    bool mChatCallSilenceReq[NUM_ACCOUNTS];
    bool mSchedMeetingUpdated[NUM_ACCOUNTS];
    bool mSchedOccurrUpdated[NUM_ACCOUNTS];
#endif

    bool mLoggedInAllChats[NUM_ACCOUNTS];
    std::vector <megachat::MegaChatHandle>mChatListUpdated[NUM_ACCOUNTS];
    bool mChatsUpdated[NUM_ACCOUNTS];
    static const std::string DEFAULT_PATH;
    static const std::string PATH_IMAGE;
    static const std::string FILE_IMAGE_NAME;

    static const std::string LOCAL_PATH;
    static const std::string REMOTE_PATH;
    static const std::string DOWNLOAD_PATH;

    // implementation for MegaListener
    void onRequestStart(::mega::MegaApi *, ::mega::MegaRequest *) override {}
    void onRequestFinish(::mega::MegaApi *api, ::mega::MegaRequest *request, ::mega::MegaError *e) override;
    void onRequestUpdate(::mega::MegaApi*, ::mega::MegaRequest *) override {}
    void onChatsUpdate(mega::MegaApi* api, mega::MegaTextChatList *chats) override;
    void onRequestTemporaryError(::mega::MegaApi *, ::mega::MegaRequest *, ::mega::MegaError*) override {}
    void onContactRequestsUpdate(::mega::MegaApi* api, ::mega::MegaContactRequestList* requests) override;
    void onUsersUpdate(::mega::MegaApi* api, ::mega::MegaUserList* userList) override;

    // implementation for MegaChatListener
    void onChatInitStateUpdate(megachat::MegaChatApi *api, int newState) override;
    void onChatListItemUpdate(megachat::MegaChatApi* api, megachat::MegaChatListItem *item) override;
    void onChatOnlineStatusUpdate(megachat::MegaChatApi* api, megachat::MegaChatHandle userhandle, int status, bool inProgress) override;
    void onChatPresenceConfigUpdate(megachat::MegaChatApi* api, megachat::MegaChatPresenceConfig *config) override;
    void onChatConnectionStateUpdate(megachat::MegaChatApi* api, megachat::MegaChatHandle chatid, int state) override;

    void onTransferStart(::mega::MegaApi *api, ::mega::MegaTransfer *transfer) override;
    void onTransferFinish(::mega::MegaApi* api, ::mega::MegaTransfer *transfer, ::mega::MegaError* error) override;
    void onTransferUpdate(::mega::MegaApi *api, ::mega::MegaTransfer *transfer) override;
    void onTransferTemporaryError(::mega::MegaApi *api, ::mega::MegaTransfer *transfer, ::mega::MegaError* error) override;
    bool onTransferData(::mega::MegaApi *api, ::mega::MegaTransfer *transfer, char *buffer, size_t size) override;

#ifndef KARERE_DISABLE_WEBRTC
    void onChatCallUpdate(megachat::MegaChatApi* api, megachat::MegaChatCall *call) override;
    void onChatSessionUpdate(megachat::MegaChatApi* api, megachat::MegaChatHandle chatid,
                                     megachat::MegaChatHandle callid,
                                     megachat::MegaChatSession *session) override;

    void onChatSchedMeetingUpdate(megachat::MegaChatApi* api, megachat::MegaChatScheduledMeeting* sm) override;
    void onSchedMeetingOccurrencesUpdate(megachat::MegaChatApi* api, megachat::MegaChatHandle chatid, bool append) override;
#endif

private:
    class TestEnv
    {
    public:
        void setLogFile(const std::string& f) { mLogger.reset(new MegaLoggerTest(f.c_str())); }
        MegaLoggerTest* logger() const { return mLogger.get(); }
        void addAccount(const std::string& email, const std::string& pswd) { mAccounts.emplace_back(email, pswd); }
        Account& account(unsigned i) { assert(i < mAccounts.size()); return mAccounts[i]; }

    private:
        std::vector<Account> mAccounts;
        std::unique_ptr<MegaLoggerTest> mLogger;
    };

    static TestEnv& getEnv() { static TestEnv env; return env; }
};

class TestChatRoomListener : public megachat::MegaChatRoomListener
{
public:
    TestChatRoomListener(MegaChatApiTest *t, megachat::MegaChatApi **apis, megachat::MegaChatHandle chatid);
    void clearMessages(unsigned int apiIndex);
    bool hasValidMessages(unsigned int apiIndex);
    bool hasArrivedMessage(unsigned int apiIndex, megachat::MegaChatHandle messageHandle);

    MegaChatApiTest *t;
    megachat::MegaChatApi **megaChatApi;
    megachat::MegaChatHandle chatid;

    bool historyLoaded[NUM_ACCOUNTS];   // when, after loadMessage(X), X messages have been loaded
    bool historyTruncated[NUM_ACCOUNTS];
    bool msgLoaded[NUM_ACCOUNTS];
    bool msgConfirmed[NUM_ACCOUNTS];
    bool msgDelivered[NUM_ACCOUNTS];
    bool msgReceived[NUM_ACCOUNTS];
    bool msgEdited[NUM_ACCOUNTS];
    bool msgRejected[NUM_ACCOUNTS];
    bool msgAttachmentReceived[NUM_ACCOUNTS];
    bool msgContactReceived[NUM_ACCOUNTS];
    bool msgRevokeAttachmentReceived[NUM_ACCOUNTS];
    bool reactionReceived[NUM_ACCOUNTS];
    bool retentionHistoryTruncated[NUM_ACCOUNTS];
    megachat::MegaChatHandle mConfirmedMessageHandle[NUM_ACCOUNTS];
    megachat::MegaChatHandle mEditedMessageHandle[NUM_ACCOUNTS];
    megachat::MegaChatHandle mRetentionMessageHandle[NUM_ACCOUNTS];

    megachat::MegaChatMessage *message;
    std::vector <megachat::MegaChatHandle>msgId[NUM_ACCOUNTS];
    int msgCount[NUM_ACCOUNTS];
    megachat::MegaChatHandle uhAction[NUM_ACCOUNTS];
    int priv[NUM_ACCOUNTS];
    std::string content[NUM_ACCOUNTS];
    bool chatUpdated[NUM_ACCOUNTS];
    bool userTyping[NUM_ACCOUNTS];
    bool titleUpdated[NUM_ACCOUNTS];
    bool archiveUpdated[NUM_ACCOUNTS];
    bool previewsUpdated[NUM_ACCOUNTS];
    bool retentionTimeUpdated[NUM_ACCOUNTS];

    // implementation for MegaChatRoomListener
    void onChatRoomUpdate(megachat::MegaChatApi* megaChatApi, megachat::MegaChatRoom *chat) override;
    void onMessageLoaded(megachat::MegaChatApi* megaChatApi, megachat::MegaChatMessage *msg) override;   // loaded by getMessages()
    void onMessageReceived(megachat::MegaChatApi* megaChatApi, megachat::MegaChatMessage *msg) override;
    void onMessageUpdate(megachat::MegaChatApi* megaChatApi, megachat::MegaChatMessage *msg) override;   // new or updated
    void onReactionUpdate(megachat::MegaChatApi *api, megachat::MegaChatHandle msgid, const char *reaction, int count) override;
    void onHistoryTruncatedByRetentionTime(megachat::MegaChatApi *api, megachat::MegaChatMessage *msg) override;

private:
    unsigned int getMegaChatApiIndex(megachat::MegaChatApi *api);
};

class MegaChatApiUnitaryTest: public karere::IApp
{
public:
    bool UNITARYTEST_ParseUrl();
#ifndef KARERE_DISABLE_WEBRTC
    bool UNITARYTEST_SfuDataReception();
#endif

    unsigned mOKTests = 0;
    unsigned mFailedTests = 0;

#ifndef KARERE_DISABLE_WEBRTC
    friend sfu::SfuConnection;
#endif

   // karere::IApp implementation
   IChatListHandler* chatListHandler() override;
   void onPresenceConfigChanged(const presenced::Config& config, bool pending) override;
   void onPresenceLastGreenUpdated(karere::Id userid, uint16_t lastGreen) override;
   void onDbError(int error, const std::string &msg) override;
};

class ResultHandler
{
public:
    int waitForResult(int seconds = maxTimeout)
    {
        if (std::future_status::ready != futureResult.wait_for(std::chrono::seconds(seconds)))
        {
            errorStr = "Timeout";
            return -999; // local timeout
        }
        return futureResult.get();
    }

    const std::string& getErrorString() const { return errorStr; }

protected:
    void finish(int errCode, std::string&& errStr)
    {
        assert(!resultReceived); // call this function only once!
        errorStr.swap(errStr);
        promiseResult.set_value(errCode);
        resultReceived = true;
    }

    bool finished() const { return resultReceived; }

private:
    std::promise<int> promiseResult;
    std::future<int> futureResult = promiseResult.get_future();
    std::atomic<bool> resultReceived = false;
    std::string errorStr;
};

class RequestTracker : public ::mega::MegaRequestListener, public ResultHandler
{
public:
    void onRequestFinish(::mega::MegaApi*, ::mega::MegaRequest* req,
                         ::mega::MegaError* e) override
    {
        request.reset(req ? req->copy() : nullptr);
        finish(e->getErrorCode(), e->getErrorString() ? e->getErrorString() : "");
    }

    ::mega::MegaHandle getNodeHandle() const
    {
        // if the operation succeeded and supplies a node handle
        return (finished() && request) ? request->getNodeHandle() : ::mega::INVALID_HANDLE;
    }

    std::string getLink() const
    {
        // if the operation succeeded and supplied a link
        return (finished() && request && request->getLink()) ? request->getLink() : std::string();
    }

    std::unique_ptr<::mega::MegaNode> getPublicMegaNode() const
    {
        return (finished() && request) ? std::unique_ptr<::mega::MegaNode>(request->getPublicMegaNode()) : nullptr;
    }

    std::string getText() const
    {
        return (finished() && request && request->getText()) ? request->getText() : std::string();
    }

private:
    std::unique_ptr<::mega::MegaRequest> request;
};

class ChatRequestTracker : public megachat::MegaChatRequestListener, public ResultHandler
{
    std::unique_ptr<::megachat::MegaChatRequest> request;

public:
    void onRequestFinish(::megachat::MegaChatApi*, ::megachat::MegaChatRequest* req,
                         ::megachat::MegaChatError* e) override
    {
        request.reset(req ? req->copy() : nullptr);
        finish(e->getErrorCode(), e->getErrorString() ? e->getErrorString() : "");
    }

    std::string getText() const
    {
        return (finished() && request && request->getText()) ? request->getText() : std::string();
    }

    bool getFlag() const
    {
        return (finished() && request) ? request->getFlag() : false;
    }

    ::megachat::MegaChatHandle getChatHandle() const
    {
        return (finished() && request) ? request->getChatHandle() : ::megachat::MEGACHAT_INVALID_HANDLE;
    }

    int getParamType() const
    {
        return (finished() && request) ? request->getParamType() : 0;
    }

    std::unique_ptr<::megachat::MegaChatScheduledMeetingOccurrList> getScheduledMeetings() const
    {
        return (finished() && request)
                  ? std::unique_ptr<::megachat::MegaChatScheduledMeetingOccurrList>(request->getMegaChatScheduledMeetingOccurrList()->copy())
                  : nullptr;
    }
};

#ifndef KARERE_DISABLE_WEBRTC
class MockupCall : public sfu::SfuInterface
{
public:
    bool handleAvCommand(Cid_t cid, unsigned av) override;
    bool handleAnswerCommand(Cid_t cid, sfu::Sdp& sdp, uint64_t ts, const std::vector<sfu::Peer>&peers, const std::map<Cid_t, sfu::TrackDescriptor>&vthumbs, const std::map<Cid_t, sfu::TrackDescriptor>&speakers,  std::set<karere::Id>& moderators, bool ownMod) override;
    bool handleKeyCommand(Keyid_t keyid, Cid_t cid, const std::string&key) override;
    bool handleVThumbsCommand(const std::map<Cid_t, sfu::TrackDescriptor> &) override;
    bool handleVThumbsStartCommand() override;
    bool handleVThumbsStopCommand() override;
    bool handleHiResCommand(const std::map<Cid_t, sfu::TrackDescriptor> &) override;
    bool handleHiResStartCommand() override;
    bool handleHiResStopCommand() override;
    bool handleSpeakReqsCommand(const std::vector<Cid_t>&) override;
    bool handleSpeakReqDelCommand(Cid_t cid) override;
    bool handleSpeakOnCommand(Cid_t cid, sfu::TrackDescriptor speaker) override;
    bool handleSpeakOffCommand(Cid_t cid) override;
    bool handlePeerJoin(Cid_t cid, uint64_t userid, int av) override;
    bool handlePeerLeft(Cid_t cid, unsigned termcode) override;
    bool handleBye(unsigned termcode) override;
    bool handleModAdd(uint64_t userid) override;
    bool handleModDel(uint64_t userid) override;
    void onSfuConnected() override;
    void onSendByeCommand() override;
    void onSfuDisconnected() override;
    bool error(unsigned int, const std::string &) override;
    void logError(const char* error) override;
};
#endif
#endif // CHATTEST_H
