#include "Sf2Reader.h"

#include "../../foundation/Text.h"

namespace duskstudio
{
const Sf2Generator* Sf2Zone::find(uint16_t oper) const noexcept
{
    for (const auto& g : gens)
        if (g.oper == oper)
            return &g;
    return nullptr;
}

namespace
{
// Fixed record sizes from the SF2 2.04 spec.
constexpr int kPhdrSize = 38;
constexpr int kBagSize  = 4;
constexpr int kGenSize  = 4;
constexpr int kInstSize = 22;
constexpr int kShdrSize = 46;

// A cursor over an in-memory byte span. All SF2 multibyte fields are
// little-endian.
struct Cursor
{
    const uint8_t* p { nullptr };
    size_t         size { 0 };
    size_t         pos  { 0 };

    bool canRead(size_t n) const noexcept { return pos + n <= size; }

    uint8_t  u8()  noexcept { return p[pos++]; }
    int8_t   s8()  noexcept { return (int8_t) p[pos++]; }
    uint16_t u16() noexcept
    {
        const uint16_t v = (uint16_t) (p[pos] | (p[pos + 1] << 8));
        pos += 2;
        return v;
    }
    uint32_t u32() noexcept
    {
        const uint32_t v = (uint32_t) (p[pos] | (p[pos + 1] << 8)
                                       | (p[pos + 2] << 16) | (p[pos + 3] << 24));
        pos += 4;
        return v;
    }
    std::string fixedStr(int n)
    {
        // SF2 name fields are NUL-padded ASCII up to n bytes.
        const char* s = (const char*) (p + pos);
        int len = 0;
        while (len < n && s[len] != '\0') ++len;
        std::string out (s, (size_t) len);
        pos += (size_t) n;
        return out;
    }
};

// One sub-chunk of a LIST: 4-char id + raw bytes. The pdta records are
// parsed out of these.
struct SubChunk
{
    char           id[5] { 0, 0, 0, 0, 0 };
    const uint8_t* data { nullptr };
    uint32_t       size { 0 };
};

bool idEquals(const char* a, const char* b) noexcept
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

// Resolve the per-record zone list from the bag/gen tables. `bagStart`
// and `bagEnd` are indices into the bag table for one preset/instrument;
// each bag entry's genNdx points into the gen table. Reads gens for
// each zone up to the next bag's genNdx.
std::vector<Sf2Zone> buildZones(int bagStart, int bagEnd,
                                 const std::vector<uint16_t>& bagGenNdx,
                                 const std::vector<Sf2Generator>& gens)
{
    std::vector<Sf2Zone> zones;
    const int maxBag = (int) bagGenNdx.size();
    for (int b = bagStart; b < bagEnd; ++b)
    {
        // A zone needs bag[b] (its genNdx) AND bag[b+1] (the next zone's
        // genNdx, used as this zone's end). A malformed bank can have a
        // header bag index past the bag table - bail rather than read OOB.
        if (b < 0 || b + 1 >= maxBag) break;
        const int genStart = juce::jlimit(0, (int) gens.size(),
                                           (int) bagGenNdx[(size_t) b]);
        const int genEnd   = juce::jlimit(0, (int) gens.size(),
                                           (int) bagGenNdx[(size_t) b + 1]);
        Sf2Zone z;
        for (int g = genStart; g < genEnd; ++g)
            z.gens.push_back(gens[(size_t) g]);
        zones.push_back(std::move(z));
    }
    return zones;
}
} // namespace

Sf2File readSf2(const juce::File& file)
{
    Sf2File out;

    juce::FileInputStream in (file);
    if (! in.openedOk())
    {
        out.error = ("Could not open " + file.getFileName()).toStdString();
        return out;
    }

    // RIFF header: 'RIFF' <u32 size> 'sfbk'.
    char riff[4], form[4];
    if (in.read(riff, 4) != 4 || ! idEquals(riff, "RIFF"))
    {
        out.error = "Not a RIFF file";
        return out;
    }
    in.readInt();   // overall size (ignored)
    if (in.read(form, 4) != 4 || ! idEquals(form, "sfbk"))
    {
        out.error = "Not an SF2 (sfbk) file";
        return out;
    }

    juce::MemoryBlock pdtaBytes;

    // Walk the three top-level LIST chunks.
    while (! in.isExhausted())
    {
        char ck[4];
        if (in.read(ck, 4) != 4) break;
        const std::uint32_t ckSize = (std::uint32_t) in.readInt();
        const std::int64_t  ckBody = in.getPosition();

        if (idEquals(ck, "LIST"))
        {
            char listType[4];
            if (in.read(listType, 4) != 4) break;

            if (idEquals(listType, "sdta"))
            {
                // Walk sub-chunks for 'smpl' (+ optional 'sm24'); record
                // their file ranges, skip the (large) PCM bytes.
                const std::int64_t sdtaEnd = ckBody + (std::int64_t) ckSize;
                while (in.getPosition() + 8 <= sdtaEnd)
                {
                    char sid[4];
                    if (in.read(sid, 4) != 4) break;
                    const std::uint32_t sSize = (std::uint32_t) in.readInt();
                    const std::int64_t  sBody = in.getPosition();
                    if (idEquals(sid, "smpl"))
                    {
                        out.smplOffset = sBody;
                        // Clamp to the file's real remainder: smplSize is the
                        // bound writeSampleWav validates shdr ranges against,
                        // so it must never exceed what the file can deliver.
                        out.smplSize   = juce::jlimit ((std::int64_t) 0,
                                                        (std::int64_t) (in.getTotalLength() - sBody),
                                                        (std::int64_t) sSize);
                    }
                    else if (idEquals(sid, "sm24"))
                    {
                        out.sm24Offset = sBody;
                        out.sm24Size   = (std::int64_t) sSize;
                    }
                    in.setPosition(sBody + (std::int64_t) sSize
                                   + ((sSize & 1) ? 1 : 0));   // pad to even
                }
            }
            else if (idEquals(listType, "pdta"))
            {
                // Slurp the whole pdta body - it's metadata, small even
                // for a 140 MB GM bank. Clamp to the bytes actually left in
                // the file: a crafted ckSize (~2 GB) must not size an
                // allocation the read can never fill.
                const int bodyLen = (int) juce::jlimit (
                    (std::int64_t) 0,
                    (std::int64_t) (in.getTotalLength() - in.getPosition()),
                    (std::int64_t) ckSize - 4);   // minus the listType
                if (bodyLen > 0)
                {
                    pdtaBytes.setSize((size_t) bodyLen);
                    const int got = in.read(pdtaBytes.getData(), bodyLen);
                    // Truncated file: shrink to what we actually read so the
                    // sub-chunk parser never walks into uninitialised tail
                    // bytes (setSize preserves the leading data on shrink).
                    if (got < bodyLen)
                        pdtaBytes.setSize((size_t) juce::jmax(0, got));
                }
            }
        }

        // Advance to the next chunk (chunks pad to even length).
        in.setPosition(ckBody + (std::int64_t) ckSize + ((ckSize & 1) ? 1 : 0));
    }

    if (pdtaBytes.getSize() == 0)
    {
        out.error = "SF2 has no pdta chunk";
        return out;
    }

    // Split pdta into its named sub-chunks.
    SubChunk phdr, pbag, pgen, inst, ibag, igen, shdr;
    {
        Cursor c { (const uint8_t*) pdtaBytes.getData(), pdtaBytes.getSize(), 0 };
        while (c.canRead(8))
        {
            SubChunk sc;
            sc.id[0] = (char) c.u8(); sc.id[1] = (char) c.u8();
            sc.id[2] = (char) c.u8(); sc.id[3] = (char) c.u8();
            sc.size  = c.u32();
            sc.data  = c.p + c.pos;
            if (! c.canRead(sc.size)) break;
            c.pos += sc.size + ((sc.size & 1) ? 1 : 0);

            if      (idEquals(sc.id, "phdr")) phdr = sc;
            else if (idEquals(sc.id, "pbag")) pbag = sc;
            else if (idEquals(sc.id, "pgen")) pgen = sc;
            else if (idEquals(sc.id, "inst")) inst = sc;
            else if (idEquals(sc.id, "ibag")) ibag = sc;
            else if (idEquals(sc.id, "igen")) igen = sc;
            else if (idEquals(sc.id, "shdr")) shdr = sc;
        }
    }

    if (phdr.data == nullptr || inst.data == nullptr || shdr.data == nullptr)
    {
        out.error = "SF2 pdta missing required sub-chunks";
        return out;
    }

    // Samples (shdr)
    {
        const int n = (int) (shdr.size / kShdrSize);
        Cursor c { shdr.data, shdr.size, 0 };
        for (int i = 0; i < n; ++i)
        {
            Sf2Sample s;
            s.name            = c.fixedStr(20);
            s.start           = c.u32();
            s.end             = c.u32();
            s.startLoop       = c.u32();
            s.endLoop         = c.u32();
            s.sampleRate      = c.u32();
            s.originalPitch   = c.u8();
            s.pitchCorrection = c.s8();
            s.sampleLink      = c.u16();
            s.sampleType      = c.u16();
            // Last record is the "EOS" terminal sentinel - drop it.
            if (i == n - 1 && dusk::text::startsWith(s.name, "EOS")) break;
            out.samples.push_back(std::move(s));
        }
    }

    // Instrument generator zones
    std::vector<uint16_t> ibagGenNdx;
    std::vector<Sf2Generator> igens;
    {
        const int n = (int) (igen.size / kGenSize);
        Cursor c { igen.data, igen.size, 0 };
        for (int i = 0; i < n; ++i)
        {
            Sf2Generator g;
            g.oper   = c.u16();
            g.amount = c.u16();
            igens.push_back(g);
        }
        const int nb = (int) (ibag.size / kBagSize);
        Cursor cb { ibag.data, ibag.size, 0 };
        for (int i = 0; i < nb; ++i)
        {
            ibagGenNdx.push_back(cb.u16());   // genNdx
            cb.u16();                          // modNdx (ignored)
        }
    }
    {
        const int n = (int) (inst.size / kInstSize);
        Cursor c { inst.data, inst.size, 0 };
        std::vector<std::string>  names;
        std::vector<int>          bagNdx;
        for (int i = 0; i < n; ++i)
        {
            names.push_back(c.fixedStr(20));
            bagNdx.push_back((int) c.u16());
        }
        // n includes the terminal "EOI"; real instruments are [0, n-1).
        for (int i = 0; i + 1 < n; ++i)
        {
            Sf2Instrument ins;
            ins.name  = names[(size_t) i];
            ins.zones = buildZones(bagNdx[(size_t) i], bagNdx[(size_t) i + 1],
                                    ibagGenNdx, igens);
            out.instruments.push_back(std::move(ins));
        }
    }

    // Preset generator zones
    std::vector<uint16_t> pbagGenNdx;
    std::vector<Sf2Generator> pgens;
    {
        const int n = (int) (pgen.size / kGenSize);
        Cursor c { pgen.data, pgen.size, 0 };
        for (int i = 0; i < n; ++i)
        {
            Sf2Generator g;
            g.oper   = c.u16();
            g.amount = c.u16();
            pgens.push_back(g);
        }
        const int nb = (int) (pbag.size / kBagSize);
        Cursor cb { pbag.data, pbag.size, 0 };
        for (int i = 0; i < nb; ++i)
        {
            pbagGenNdx.push_back(cb.u16());
            cb.u16();
        }
    }
    {
        const int n = (int) (phdr.size / kPhdrSize);
        Cursor c { phdr.data, phdr.size, 0 };
        std::vector<std::string>  names;
        std::vector<uint16_t>     progs, banks;
        std::vector<int>          bagNdx;
        for (int i = 0; i < n; ++i)
        {
            names.push_back(c.fixedStr(20));
            progs.push_back(c.u16());
            banks.push_back(c.u16());
            bagNdx.push_back((int) c.u16());
            c.u32(); c.u32(); c.u32();   // library / genre / morphology
        }
        for (int i = 0; i + 1 < n; ++i)   // skip terminal "EOP"
        {
            Sf2Preset pr;
            pr.name   = names[(size_t) i];
            pr.preset = progs[(size_t) i];
            pr.bank   = banks[(size_t) i];
            pr.zones  = buildZones(bagNdx[(size_t) i], bagNdx[(size_t) i + 1],
                                    pbagGenNdx, pgens);
            out.presets.push_back(std::move(pr));
        }
    }

    out.ok = true;
    return out;
}
} // namespace duskstudio
