#ifndef MEGACRYPTOFUNCTIONS_H
#define MEGACRYPTOFUNCTIONS_H
#include <map>
#include "IRtcCrypto.h"

#ifndef ENABLE_CHAT
    #define ENABLE_CHAT 1
#endif
namespace karere { class Client; }
namespace strongvelope { template<size_t L> class Key; typedef Key<16> SendKey; }
namespace rtcModule
{
class RtcCrypto: public rtcModule::IRtcCrypto
{
protected:
    karere::Client& mClient;
    void computeSymmetricKey(karere::Id peer, strongvelope::SendKey& output);
public:
    RtcCrypto(karere::Client& client);
    virtual void mac(const std::string& data, const SdpKey& key, SdpKey& output);
    virtual void decryptKeyFrom(karere::Id peer, const SdpKey& data, SdpKey& output);
    virtual void encryptKeyTo(karere::Id peer, const SdpKey& data, SdpKey& output);
    virtual karere::Id anonymizeId(karere::Id userid);
    virtual void random(char* buf, size_t len);
};

class RtcCryptoMeetings: public rtcModule::IRtcCryptoMeetings
{
protected:
    karere::Client& mClient;
    void computeSymmetricKey(karere::Id peer, strongvelope::SendKey& output);
public:
    RtcCryptoMeetings(karere::Client& client);
    void decryptKeyFrom(const karere::Id &peer, const strongvelope::SendKey &data, strongvelope::SendKey &output) override;
    void encryptKeyTo(const karere::Id &peer, const strongvelope::SendKey &data, strongvelope::SendKey &output) override;
    void xorWithCallKey(const strongvelope::SendKey &callKey, strongvelope::SendKey &sendKey) override;
    std::shared_ptr<strongvelope::SendKey> generateSendKey() override;
    promise::Promise<Buffer*> getCU25519PublicKey(const karere::Id &peer) override;
    static std::string keyToStr(const strongvelope::SendKey& key);
    static strongvelope::SendKey strToKey(const std::string& keystr);
};
}
#endif // MEGACRYPTOFUNCTIONS_H
