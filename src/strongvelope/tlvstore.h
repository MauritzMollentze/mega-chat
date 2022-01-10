#include <string>
#include <map>

namespace strongvelope
{

/** @brief Holds info about a TLV record: its type and offset and length that allow
 *  its payload data to be extracted from the container
 */
struct TlvRecord
{
    const StaticBuffer& sourceBuf;
    uint8_t type = 0; ///< Type code of TLV record.
    size_t dataOffset = 0; ///< Offset of record payload data inside the container
    size_t dataLen = 0; ///< Length of payload data
    TlvRecord(const StaticBuffer& aBuf): sourceBuf(aBuf){}
    void validateDataLen(size_t expected)
    {
        if (dataLen != expected)
            throw std::runtime_error("parseMessageContent: Unexpected length of TLV record with type "+std::to_string(type)+ ": expected "+std::to_string(expected)+" actual: "+std::to_string(dataLen));
    }
    char* buf() const { return sourceBuf.buf()+dataOffset; }
    template <class T>
    T read() { validateDataLen(sizeof(T)); return sourceBuf.read<T>(dataOffset); }
    template <class T>
    void addToSet(std::set<T>& s) { s.insert(sourceBuf.read<T>()); }
    template <typename T>
    void appendToVector(std::vector<T>& v) { v.push_back(sourceBuf.read<T>()); }
    void appendToBufVector(std::vector<Buffer>& v)
    {
        v.emplace_back(buf(), dataLen);
    }
};

class TlvParser
{
/**
 * @brief Parses a TLV record off of a container, at a given offset inside the container.
 * Fills a TlvRecordInfo structure with information about the record
 *
 * @param tlvContainer Single binary encoded container of TLV records.
 * @param offset Offset at which to read the record
 * @param [out] record Infomation about the record - type, offset of payload,
 * and layload length
 * @returns The offset of the next recorf if any, or Buffer::kNotFound if
 * this was the last record in the container.
 */
protected:
    const StaticBuffer& mSource;
    size_t mOffset;
public:
    TlvParser(const StaticBuffer& source, size_t offset)
    :mSource(source), mOffset(offset){}
    bool getRecord(TlvRecord& record)
    {
       if (mOffset == Buffer::kNotFound)
            return false;
        record.type = mSource.read<uint8_t>(mOffset);
        record.dataOffset = mOffset + sizeof(uint8_t) + 2;
        uint16_t valueLen = ntohs(mSource.read<uint16_t>(mOffset + sizeof(uint8_t)));

        if (valueLen == 0xffff)
        {
            record.dataLen = mSource.dataSize() - record.dataOffset;
            mOffset = mSource.dataSize();
        }
        else
        {
            record.dataLen = valueLen;
            mOffset = record.dataOffset+record.dataLen;
        }

        if (mOffset > mSource.dataSize())
            throw std::runtime_error("TlvContainer::getRecord: Corrupt data - spans outside of physical buffer");

        if (mOffset == mSource.dataSize())
            mOffset = Buffer::kNotFound;
        return true;
}
};
class TlvWriter: public Buffer
{
protected:
#ifndef NDEBUG
    bool mEnded = false;
#endif
public:
    explicit TlvWriter(size_t reserve=128): Buffer(reserve){}

/**
 * Generates a binary encoded TLV record from a key-value pair.
 *
 * @param type Record type code byte
 * @param value Binary payload of record.
 */

void addRecord(uint8_t type, const StaticBuffer& value)
{
    assert(!mEnded);
    append(type);
    if (value.dataSize() >= 0xffff)
    {
        append<uint16_t>(0xffff);
#ifdef NODEBUG
        mEnded = true;
#endif
    }
    else
    {
        append<uint16_t>(htons(uint16_t(value.dataSize())));
    }
    append(value);
}

template <typename T, typename=typename std::enable_if<std::is_pod<T>::value>::type>
void addRecord(uint8_t type, T val)
{
    assert(!mEnded);
    append(type);
    append<uint16_t>(htons(sizeof(val))).append(val);
}
};
}

