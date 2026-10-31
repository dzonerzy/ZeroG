#include "config.h"
#include "media/MP4Stream.h"

#if ENABLE(MEDIA_SOURCE) && defined(ZEROG_MEDIA)

#include <optional>

namespace ZeroG::Media {

namespace {

// Big-endian fields of a box's body, bounds-checked: a read past the end
// leaves the reader failed and gives 0.
class BoxReader {
public:
    explicit BoxReader(std::span<const uint8_t> data)
        : m_data(data)
    {
    }

    bool ok() const { return m_ok; }
    size_t offset() const { return m_offset; }
    size_t remaining() const { return m_ok ? m_data.size() - m_offset : 0; }

    uint64_t read(size_t bytes)
    {
        if (!m_ok || remaining() < bytes) {
            m_ok = false;
            return 0;
        }
        uint64_t value = 0;
        for (size_t i = 0; i < bytes; ++i)
            value = (value << 8) | m_data[m_offset + i];
        m_offset += bytes;
        return value;
    }
    uint8_t u8() { return read(1); }
    uint16_t u16() { return read(2); }
    uint32_t u32() { return read(4); }
    uint64_t u64() { return read(8); }
    void skip(size_t bytes)
    {
        if (remaining() < bytes)
            m_ok = false;
        else
            m_offset += bytes;
    }
    std::span<const uint8_t> rest() const { return m_ok ? m_data.subspan(m_offset) : std::span<const uint8_t> { }; }
    std::span<const uint8_t> take(size_t bytes)
    {
        if (remaining() < bytes) {
            m_ok = false;
            return { };
        }
        auto span = m_data.subspan(m_offset, bytes);
        m_offset += bytes;
        return span;
    }

private:
    std::span<const uint8_t> m_data;
    size_t m_offset { 0 };
    bool m_ok { true };
};

constexpr uint32_t fourCC(const char (&name)[5])
{
    return (static_cast<uint32_t>(name[0]) << 24) | (static_cast<uint32_t>(name[1]) << 16) | (static_cast<uint32_t>(name[2]) << 8) | static_cast<uint32_t>(name[3]);
}

struct BoxHeader {
    uint32_t type { 0 };
    size_t headerSize { 0 };
    uint64_t size { 0 }; // the whole box; 0: to the end of its container
};

// The header of the box at the start of `data`; nullopt when it has not all
// arrived.
std::optional<BoxHeader> readHeader(std::span<const uint8_t> data)
{
    BoxReader reader(data);
    BoxHeader header;
    header.size = reader.u32();
    header.type = reader.u32();
    if (!reader.ok())
        return std::nullopt;
    header.headerSize = 8;
    if (header.size == 1) {
        header.size = reader.u64();
        if (!reader.ok())
            return std::nullopt;
        header.headerSize = 16;
    }
    return header;
}

// The child boxes of a container's body.
template<typename Function>
bool forEachBox(std::span<const uint8_t> data, const Function& function)
{
    while (!data.empty()) {
        auto header = readHeader(data);
        if (!header)
            return false;
        uint64_t size = header->size ? header->size : data.size();
        if (size < header->headerSize || size > data.size())
            return false;
        function(header->type, data.subspan(header->headerSize, size - header->headerSize));
        data = data.subspan(size);
    }
    return true;
}

std::span<const uint8_t> findBox(std::span<const uint8_t> data, uint32_t type)
{
    std::span<const uint8_t> found;
    bool present = false;
    forEachBox(data, [&](uint32_t boxType, std::span<const uint8_t> body) {
        if (!present && boxType == type) {
            found = body;
            present = true;
        }
    });
    return found;
}

// An MPEG-4 descriptor's length (up to four bytes of seven bits).
size_t descriptorLength(BoxReader& reader)
{
    size_t length = 0;
    for (int i = 0; i < 4; ++i) {
        uint8_t byte = reader.u8();
        length = (length << 7) | (byte & 0x7F);
        if (!(byte & 0x80))
            break;
    }
    return length;
}

// esds: the decoder's object type and its DecoderSpecificInfo (for AAC, the
// AudioSpecificConfig).
bool parseESDescriptor(std::span<const uint8_t> esds, uint8_t& objectType, Vector<uint8_t>& specificInfo)
{
    BoxReader reader(esds);
    reader.skip(4); // version and flags
    if (reader.u8() != 0x03)
        return false;
    descriptorLength(reader);
    reader.skip(2); // ES_ID
    uint8_t flags = reader.u8();
    if (flags & 0x80)
        reader.skip(2);
    if (flags & 0x40)
        reader.skip(reader.u8());
    if (flags & 0x20)
        reader.skip(2);
    if (reader.u8() != 0x04)
        return false;
    descriptorLength(reader);
    objectType = reader.u8();
    reader.skip(12); // stream type, buffer size, bitrates
    if (reader.ok() && reader.remaining() && reader.u8() == 0x05) {
        size_t length = descriptorLength(reader);
        specificInfo = Vector<uint8_t> { reader.take(length) };
    }
    return reader.ok();
}

// AudioSpecificConfig: the rate and channels (overriding the sample entry's,
// which MP4 writers often leave at 2 and 44.1 or 48 kHz).
void parseAudioSpecificConfig(std::span<const uint8_t> config, unsigned& sampleRate, unsigned& channels)
{
    if (config.size() < 2)
        return;
    uint32_t bits = (config[0] << 24) | (config[1] << 16) | (config.size() > 2 ? config[2] << 8 : 0) | (config.size() > 3 ? config[3] : 0);
    int position = 0;
    auto take = [&](int count) {
        uint32_t value = (bits << position) >> (32 - count);
        position += count;
        return value;
    };
    uint32_t objectType = take(5);
    if (objectType == 31)
        take(6);
    static constexpr unsigned rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
    uint32_t rateIndex = take(4);
    if (rateIndex < std::size(rates))
        sampleRate = rates[rateIndex];
    else if (rateIndex == 15)
        return; // an explicit 24-bit rate: keep the sample entry's
    uint32_t channelConfiguration = take(4);
    if (channelConfiguration >= 1 && channelConfiguration <= 7)
        channels = channelConfiguration == 7 ? 8 : channelConfiguration;
}

// dOps (Opus in MP4, big-endian) to OpusHead (Ogg's, little-endian), what
// the Opus decoder takes.
Vector<uint8_t> opusHeadFromDOps(std::span<const uint8_t> dOps)
{
    BoxReader reader(dOps);
    reader.u8(); // version
    uint8_t channels = reader.u8();
    uint16_t preSkip = reader.u16();
    uint32_t inputRate = reader.u32();
    uint16_t gain = reader.u16();
    uint8_t family = reader.u8();
    if (!reader.ok())
        return { };
    Vector<uint8_t> head { std::span<const uint8_t> { reinterpret_cast<const uint8_t*>("OpusHead"), 8 } };
    head.append(1);
    head.append(channels);
    head.append(preSkip & 0xFF);
    head.append(preSkip >> 8);
    for (int i = 0; i < 4; ++i)
        head.append((inputRate >> (8 * i)) & 0xFF);
    head.append(gain & 0xFF);
    head.append(gain >> 8);
    head.append(family);
    if (family)
        head.append(reader.rest()); // stream counts and the channel mapping
    return head;
}

} // namespace

MP4StreamParser::MP4StreamParser(Client& client)
    : m_client(client)
{
}

MP4StreamParser::Track* MP4StreamParser::track(uint32_t id)
{
    for (auto& entry : m_tracks) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

bool MP4StreamParser::append(std::span<const uint8_t> bytes)
{
    m_buffer.append(bytes);
    size_t consumed = 0;
    bool ok = true;
    for (;;) {
        auto data = m_buffer.span().subspan(consumed);
        auto header = readHeader(data);
        if (!header)
            break;
        // Box types are four printable characters: anything else is not MP4.
        for (int shift = 24; shift >= 0; shift -= 8) {
            uint8_t character = (header->type >> shift) & 0xFF;
            if (character < 0x20 || character > 0x7E)
                ok = false;
        }
        if (!ok || !header->size || header->size < header->headerSize) {
            ok = false;
            break;
        }
        if (data.size() < header->size)
            break; // the rest arrives later
        auto body = data.subspan(header->headerSize, header->size - header->headerSize);
        if (header->type == fourCC("moov")) {
            if (!parseMovie(body)) {
                ok = false;
                break;
            }
        } else if (header->type == fourCC("moof")) {
            // Its samples are in the mdat after it: both must be here.
            auto next = readHeader(data.subspan(header->size));
            if (!next || data.size() < header->size + next->size)
                break;
            if (!parseFragment(data, header->size)) {
                ok = false;
                break;
            }
            if (next->type == fourCC("mdat"))
                consumed += next->size;
        }
        // ftyp, styp, sidx, emsg, free, skip, prft, a lone mdat: nothing to play.
        consumed += header->size;
    }
    if (consumed) {
        m_buffer.removeAt(0, consumed);
        m_bufferPosition += consumed;
    }
    return ok;
}

void MP4StreamParser::reset()
{
    m_bufferPosition += m_buffer.size();
    m_buffer.clear();
}

bool MP4StreamParser::parseMovie(std::span<const uint8_t> movie)
{
    m_tracks.clear();
    MediaTime duration = MediaTime::invalidTime();
    uint32_t movieTimescale = 0;
    std::span<const uint8_t> movieExtends;
    bool ok = forEachBox(movie, [&](uint32_t type, std::span<const uint8_t> body) {
        if (type == fourCC("mvhd")) {
            BoxReader reader(body);
            uint8_t version = reader.u8();
            reader.skip(3 + (version ? 16 : 8));
            movieTimescale = reader.u32();
            uint64_t length = version ? reader.u64() : reader.u32();
            bool unknown = !length || length == (version ? UINT64_MAX : UINT32_MAX);
            if (reader.ok() && movieTimescale && !unknown)
                duration = MediaTime(length, movieTimescale);
        } else if (type == fourCC("mvex"))
            movieExtends = body;
        else if (type == fourCC("trak")) {
            Track entry;
            BoxReader header(findBox(body, fourCC("tkhd")));
            uint8_t version = header.u8();
            header.skip(3 + (version ? 16 : 8));
            entry.id = header.u32();
            auto media = findBox(body, fourCC("mdia"));
            BoxReader mediaHeader(findBox(media, fourCC("mdhd")));
            uint8_t mediaVersion = mediaHeader.u8();
            mediaHeader.skip(3 + (mediaVersion ? 16 : 8));
            entry.timescale = mediaHeader.u32();
            BoxReader handler(findBox(media, fourCC("hdlr")));
            handler.skip(8);
            uint32_t handlerType = handler.u32();
            auto sampleTable = findBox(findBox(findBox(media, fourCC("minf")), fourCC("stbl")), fourCC("stsd"));
            if (!header.ok() || !mediaHeader.ok() || !entry.timescale || sampleTable.size() < 8)
                return;
            auto entries = sampleTable.subspan(8); // version, flags, entry count
            auto sampleEntry = readHeader(entries);
            if (!sampleEntry || sampleEntry->size > entries.size() || sampleEntry->size < sampleEntry->headerSize)
                return;
            auto description = entries.subspan(sampleEntry->headerSize, sampleEntry->size - sampleEntry->headerSize);
            // edts/elst: the first edit that plays media gives the offset.
            BoxReader edits(findBox(findBox(body, fourCC("edts")), fourCC("elst")));
            uint8_t editsVersion = edits.u8();
            edits.skip(3);
            uint32_t editCount = edits.u32();
            for (uint32_t i = 0; i < editCount && edits.ok(); ++i) {
                edits.skip(editsVersion ? 8 : 4); // segment duration
                int64_t mediaTime = editsVersion ? static_cast<int64_t>(edits.u64()) : static_cast<int64_t>(static_cast<int32_t>(edits.u32()));
                edits.skip(4); // rate
                if (edits.ok() && mediaTime >= 0) {
                    entry.timeOffset = mediaTime;
                    break;
                }
            }
            auto& info = entry.info;
            info.number = entry.id;
            if (handlerType == fourCC("vide")) {
                // VisualSampleEntry: 78 bytes, then the codec's boxes.
                BoxReader visual(description);
                visual.skip(24);
                info.width = visual.u16();
                info.height = visual.u16();
                visual.skip(50);
                if (!visual.ok())
                    return;
                auto boxes = visual.rest();
                info.kind = StreamTrack::Kind::Video;
                switch (sampleEntry->type) {
                case fourCC("avc1"):
                case fourCC("avc3"):
                    info.codecID = "V_MPEG4/ISO/AVC"_s;
                    info.codecPrivate = Vector<uint8_t> { findBox(boxes, fourCC("avcC")) };
                    break;
                case fourCC("vp09"):
                    info.codecID = "V_VP9"_s;
                    break;
                case fourCC("av01"):
                    info.codecID = "V_AV1"_s;
                    break;
                default:
                    return; // HEVC, encrypted, ...: not played
                }
            } else if (handlerType == fourCC("soun")) {
                // AudioSampleEntry: 28 bytes, then the codec's boxes.
                BoxReader audio(description);
                audio.skip(16);
                info.channels = audio.u16();
                audio.skip(6);
                info.sampleRate = audio.u32() >> 16;
                if (!audio.ok())
                    return;
                auto boxes = audio.rest();
                info.kind = StreamTrack::Kind::Audio;
                if (sampleEntry->type == fourCC("mp4a")) {
                    uint8_t objectType = 0;
                    Vector<uint8_t> config;
                    if (!parseESDescriptor(findBox(boxes, fourCC("esds")), objectType, config))
                        return;
                    // AAC (MPEG-4, and MPEG-2's three profiles).
                    if (objectType != 0x40 && objectType != 0x66 && objectType != 0x67 && objectType != 0x68)
                        return;
                    info.codecID = "A_AAC"_s;
                    parseAudioSpecificConfig(config.span(), info.sampleRate, info.channels);
                    info.codecPrivate = WTF::move(config);
                } else if (sampleEntry->type == fourCC("Opus")) {
                    info.codecID = "A_OPUS"_s;
                    info.codecPrivate = opusHeadFromDOps(findBox(boxes, fourCC("dOps")));
                    info.sampleRate = 48000;
                } else
                    return;
            } else
                return;
            m_tracks.append(WTF::move(entry));
        }
    });
    if (!ok)
        return false;
    // mvex, once the tracks are known: the fragments' duration and defaults.
    forEachBox(movieExtends, [&](uint32_t type, std::span<const uint8_t> body) {
        BoxReader reader(body);
        if (type == fourCC("mehd")) {
            uint8_t version = reader.u8();
            reader.skip(3);
            uint64_t length = version ? reader.u64() : reader.u32();
            if (reader.ok() && length && movieTimescale)
                duration = MediaTime(length, movieTimescale);
        } else if (type == fourCC("trex")) {
            reader.skip(4);
            uint32_t id = reader.u32();
            reader.skip(4); // sample description index
            uint32_t defaultDuration = reader.u32();
            uint32_t defaultSize = reader.u32();
            uint32_t defaultFlags = reader.u32();
            if (auto* entry = track(id); entry && reader.ok()) {
                entry->defaultDuration = defaultDuration;
                entry->defaultSize = defaultSize;
                entry->defaultFlags = defaultFlags;
            }
        }
    });
    Vector<StreamTrack> tracks;
    for (auto& entry : m_tracks)
        tracks.append(entry.info);
    m_client.didParseInitializationSegment(WTF::move(tracks), duration);
    return true;
}

bool MP4StreamParser::parseFragment(std::span<const uint8_t> data, size_t movieFragmentSize)
{
    // `data` starts with the moof; the mdat follows it.
    auto movieFragment = readHeader(data);
    auto body = data.subspan(movieFragment->headerSize, movieFragmentSize - movieFragment->headerSize);
    bool ok = true;
    forEachBox(body, [&](uint32_t type, std::span<const uint8_t> trackFragment) {
        if (type != fourCC("traf") || !ok)
            return;
        // tfhd: the track and its defaults for this fragment.
        BoxReader header(findBox(trackFragment, fourCC("tfhd")));
        uint32_t flags = header.u32() & 0xFFFFFF;
        uint32_t id = header.u32();
        auto* entry = track(id);
        if (!header.ok() || !entry)
            return; // a track not played
        std::optional<uint64_t> baseOffset;
        if (flags & 0x01)
            baseOffset = header.u64();
        if (flags & 0x02)
            header.skip(4);
        uint32_t defaultDuration = (flags & 0x08) ? header.u32() : entry->defaultDuration;
        uint32_t defaultSize = (flags & 0x10) ? header.u32() : entry->defaultSize;
        uint32_t defaultFlags = (flags & 0x20) ? header.u32() : entry->defaultFlags;
        // Offsets count from the moof, unless the stream gives an absolute one.
        int64_t base = 0;
        if (baseOffset)
            base = static_cast<int64_t>(*baseOffset) - static_cast<int64_t>(m_bufferPosition) - static_cast<int64_t>(data.data() - m_buffer.span().data());

        int64_t decodeTime = entry->nextDecodeTime;
        if (auto decodeTimeBox = findBox(trackFragment, fourCC("tfdt")); !decodeTimeBox.empty()) {
            BoxReader reader(decodeTimeBox);
            uint8_t version = reader.u8();
            reader.skip(3);
            decodeTime = version ? reader.u64() : reader.u32();
        }
        bool audio = entry->info.kind == StreamTrack::Kind::Audio;
        int64_t dataOffset = base;
        forEachBox(trackFragment, [&](uint32_t childType, std::span<const uint8_t> run) {
            if (childType != fourCC("trun") || !ok)
                return;
            BoxReader reader(run);
            uint8_t version = reader.u8();
            uint32_t runFlags = reader.read(3);
            uint32_t count = reader.u32();
            if (runFlags & 0x01)
                dataOffset = base + static_cast<int32_t>(reader.u32());
            std::optional<uint32_t> firstFlags;
            if (runFlags & 0x04)
                firstFlags = reader.u32();
            for (uint32_t i = 0; i < count && reader.ok(); ++i) {
                uint32_t duration = (runFlags & 0x100) ? reader.u32() : defaultDuration;
                uint32_t size = (runFlags & 0x200) ? reader.u32() : defaultSize;
                uint32_t sampleFlags = (runFlags & 0x400) ? reader.u32() : (!i && firstFlags) ? *firstFlags : defaultFlags;
                int64_t compositionOffset = 0;
                if (runFlags & 0x800)
                    compositionOffset = version ? static_cast<int64_t>(static_cast<int32_t>(reader.u32())) : static_cast<int64_t>(reader.u32());
                if (!reader.ok())
                    break;
                if (dataOffset < 0 || static_cast<uint64_t>(dataOffset) + size > data.size()) {
                    ok = false;
                    return;
                }
                StreamFrame frame;
                frame.track = entry->id;
                frame.decodeTime = MediaTime(decodeTime - entry->timeOffset, entry->timescale);
                frame.time = MediaTime(decodeTime + compositionOffset - entry->timeOffset, entry->timescale);
                frame.duration = MediaTime(duration, entry->timescale);
                // sample_is_non_sync_sample; every audio frame starts afresh.
                frame.key = audio || !(sampleFlags & 0x10000);
                frame.data = Vector<uint8_t> { data.subspan(dataOffset, size) };
                m_client.didParseFrame(WTF::move(frame));
                dataOffset += size;
                decodeTime += duration;
            }
            if (!reader.ok())
                ok = false;
        });
        entry->nextDecodeTime = decodeTime;
    });
    return ok;
}

} // namespace ZeroG::Media

#endif // ENABLE(MEDIA_SOURCE) && defined(ZEROG_MEDIA)
