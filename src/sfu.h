#ifndef SFU_H
#define SFU_H

#include <chatClient.h>


#define SFU_LOG_DEBUG(fmtString,...) KARERE_LOG_DEBUG(krLogChannel_sfu, fmtString, ##__VA_ARGS__)
#define SFU_LOG_INFO(fmtString,...) KARERE_LOG_INFO(krLogChannel_sfu, fmtString, ##__VA_ARGS__)
#define SFU_LOG_WARNING(fmtString,...) KARERE_LOG_WARNING(krLogChannel_sfu, fmtString, ##__VA_ARGS__)
#define SFU_LOG_ERROR(fmtString,...) KARERE_LOG_ERROR(krLogChannel_sfu, fmtString, ##__VA_ARGS__)
namespace sfu
{
    class Command;
    class SfuConnection : public karere::DeleteTrackable, public WebsocketsClient
    {
    public:
        enum ConnState
        {
            kConnNew = 0,
            kDisconnected,
            kResolving,
            kConnecting,
            kConnected,
            kLoggedIn
        };

        SfuConnection(const std::string& sfuUrl, karere::Client& karereClient);
        bool isOnline() const { return (mConnState >= kConnected); }
        promise::Promise<void> connect();
        void disconnect();
        void doConnect();
        void retryPendingConnection(bool disconnect);
        bool handleIncomingData(const char* data, size_t len);
        virtual bool handleAvCommand(karere::Id cid, karere::Id peer, int av);
        virtual bool handleAnswerCommand(karere::Id, int, std::vector<karere::Id>, const std::string&, std::vector<karere::Id>);

    protected:
        std::string mSfuUrl;
        karere::Client& mKarereClient;

        /** Current state of the connection */
        ConnState mConnState = kConnNew;

        /** Target IP address being used for the reconnection in-flight */
        std::string mTargetIp;

        /** ip version to try first (both are tried) */
        bool usingipv6 = false;

        /** RetryController that manages the reconnection's attempts */
        std::unique_ptr<karere::rh::IRetryController> mRetryCtrl;

        /** Input promise for the RetryController
         *  - If it fails: a new attempt is schedulled
         *  - If it success: the reconnection is taken as done */
        promise::Promise<void> mConnectPromise;
        std::vector<std::string> mIpsv4;
        std::vector<std::string> mIpsv6;

        void setConnState(ConnState newState);

        void wsConnectCb() override;
        void wsCloseCb(int errcode, int errtype, const char *preason, size_t preason_len) override;
        void wsHandleMsgCb(char *data, size_t len) override;
        void wsSendMsgCb(const char *, size_t) override {}

        void onSocketClose(int errcode, int errtype, const std::string& reason);
        promise::Promise<void> reconnect();
        void abortRetryController();

        std::map<std::string, std::unique_ptr<Command>> mCommands;
    };

    class SfuClient
    {
    public:
        SfuClient(karere::Client& karereClient);
        promise::Promise<void> startCall(karere::Id chatid, const std::string& sfuUrl);
        void endCall(karere::Id chatid);

    private:
        std::map<karere::Id, std::unique_ptr<SfuConnection>> mConnections;
        karere::Client& mKarereClient;
    };

    static inline const char* connStateToStr(SfuConnection::ConnState state)
    {
        switch (state)
        {
        case SfuConnection::kDisconnected: return "Disconnected";
        case SfuConnection::kResolving: return "Resolving";
        case SfuConnection::kConnecting: return "Connecting";
        case SfuConnection::kConnected: return "Connected";
        case SfuConnection::kLoggedIn: return "Logged-in";
        case SfuConnection::kConnNew: return "New";
        default: return "(invalid)";
        }
    }

    class Command
    {
    public:
        virtual bool processCommand(const rapidjson::Document& command) = 0;
        static std::string COMMAND_IDENTIFIER;
    protected:
        Command();
    };

    typedef std::function<bool(karere::Id, karere::Id, int)> AvCompleteFunction;
    class AVCommand : public Command
    {
    public:
        AVCommand(const AvCompleteFunction& complete);
        bool processCommand(const rapidjson::Document& command) override;
        static std::string COMMAND_NAME;
        AvCompleteFunction mComplete;
    };

    typedef std::function<bool(karere::Id, int, std::vector<karere::Id>, const std::string&, std::vector<karere::Id>)> AnswerCompleteFunction;
    class AnswerCommand : public Command
    {
    public:
        AnswerCommand(const AnswerCompleteFunction& complete);
        bool processCommand(const rapidjson::Document& command) override;
        static std::string COMMAND_NAME;
        AnswerCompleteFunction mComplete;
    };

}

#endif // SFU_H
