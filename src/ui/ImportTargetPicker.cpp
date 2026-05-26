#include "ImportTargetPicker.h"
#include "DuskAlerts.h"

#include <algorithm>
#include <cmath>

namespace duskstudio
{
namespace
{
constexpr int kRowH        = 36;
constexpr int kRowGap      = 2;
constexpr int kListPad     = 8;
constexpr int kFooterH     = 44;
constexpr int kHeaderH     = 76;
constexpr int kPanelW      = 540;
constexpr int kPanelH      = 560;

enum class Bucket : int { MatchEmpty = 0, MatchOccupied = 1, Mismatch = 2 };

struct SortRecord
{
    int    trackIndex;
    Bucket bucket;
};

Bucket bucketFor (const Track::Mode trackMode,
                    int numRegions,
                    bool isMidi,
                    int  numChannels)
{
    if (isMidi)
    {
        if (trackMode == Track::Mode::Midi)
            return numRegions == 0 ? Bucket::MatchEmpty : Bucket::MatchOccupied;
        return Bucket::Mismatch;
    }
    const auto wanted = (numChannels == 2) ? Track::Mode::Stereo : Track::Mode::Mono;
    if (trackMode == wanted)
        return numRegions == 0 ? Bucket::MatchEmpty : Bucket::MatchOccupied;
    return Bucket::Mismatch;
}

juce::String trackDisplayName (const Track& t, int idx)
{
    const auto raw = t.name.trim();
    if (raw.isEmpty() || raw == juce::String (idx + 1))
        return juce::String ("Trk ") + juce::String (idx + 1);
    return raw;
}

juce::String modeBadgeText (Track::Mode m)
{
    switch (m)
    {
        case Track::Mode::Mono:   return juce::String ("[Mono]");
        case Track::Mode::Stereo: return juce::String ("[Stereo]");
        case Track::Mode::Midi:   return juce::String ("[MIDI]");
    }
    return juce::String ("[?]");
}

juce::String summaryLine (const ImportTargetPicker::FileSummary& s)
{
    if (s.isMidi)
    {
        return juce::String ("MIDI \xc2\xb7 ") + juce::String (s.numMidiNotes)
             + juce::String (" notes \xc2\xb7 ")
             + juce::String ((int) std::llround ((double) s.lengthTicks
                                                  / (double) kMidiTicksPerQuarter))
             + juce::String (" beats");
    }
    const double seconds = (s.sampleRate > 0.0)
                              ? (double) s.lengthSamples / s.sampleRate
                              : 0.0;
    const auto khz = juce::String ((int) std::llround (s.sampleRate / 1000.0));
    const auto chs = s.numChannels == 2 ? juce::String ("stereo") : juce::String ("mono");
    return khz + juce::String (" k\xc2\xb7") + chs
         + juce::String (" \xc2\xb7 ") + juce::String (seconds, 1) + juce::String (" s");
}
} // namespace

struct ImportTargetPicker::Row final : public juce::Component
{
    Row (ImportTargetPicker& ownerRef, int indexInList)
        : owner (ownerRef), listIndex (indexInList)
    {
        setInterceptsMouseClicks (true, false);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        const bool selected   = (owner.selectedRowIdx == listIndex);
        const bool muted      = (bucket == Bucket::Mismatch);

        // Selection background.
        if (selected)
        {
            g.setColour (juce::Colour (0xff35404a));
            g.fillRoundedRectangle (bounds, 3.0f);
        }

        // Colour swatch on the left.
        auto swatch = bounds.removeFromLeft (5.0f).reduced (0.5f, 5.0f);
        g.setColour (colour.withAlpha (muted ? 0.5f : 1.0f));
        g.fillRoundedRectangle (swatch, 1.5f);

        auto body = bounds.reduced (8.0f, 4.0f);

        const auto fg     = muted ? juce::Colour (0xff707078) : juce::Colour (0xffe0e0e4);
        const auto fgDim  = muted ? juce::Colour (0xff505058) : juce::Colour (0xffa0a0a8);

        g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        g.setColour (fg);
        const auto nameArea = body.removeFromLeft (160.0f);
        g.drawText (displayName, nameArea, juce::Justification::centredLeft, true);

        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.setColour (fgDim);
        const auto badgeArea = body.removeFromLeft (70.0f);
        g.drawText (badgeText, badgeArea, juce::Justification::centredLeft, false);

        const auto occArea = body.removeFromLeft (90.0f);
        g.drawText (occupancy, occArea, juce::Justification::centredLeft, false);

        // Recommended pill OR mismatch hint.
        if (recommended)
        {
            const auto pill = body.removeFromLeft (110.0f).reduced (2.0f, 6.0f);
            g.setColour (juce::Colour (0xff2a5a3a));
            g.fillRoundedRectangle (pill, 3.0f);
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
            g.drawText ("RECOMMENDED", pill, juce::Justification::centred, false);
        }
        else if (! mismatchHint.isEmpty())
        {
            g.setColour (juce::Colour (0xffd0a060));
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawText (mismatchHint, body, juce::Justification::centredLeft, true);
        }
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        owner.selectRow (listIndex);
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        owner.selectRow (listIndex);
        owner.commitSelection();
    }

    ImportTargetPicker& owner;
    int listIndex = -1;
    int trackIndex = 0;
    Bucket bucket = Bucket::Mismatch;
    bool recommended = false;
    juce::Colour colour;
    juce::String displayName;
    juce::String badgeText;
    juce::String occupancy;
    juce::String mismatchHint;
};

ImportTargetPicker::ImportTargetPicker (Session& s,
                                            FileSummary fileSummary,
                                            juce::int64 timelineStartSamples,
                                            double      sampleRate,
                                            float       bpm,
                                            int         bpb,
                                            int         displayMode,
                                            int         preferredTrackIndex,
                                            std::function<void (int)> commit,
                                            std::function<void()> cancel)
    : session (s),
      summary (std::move (fileSummary)),
      timelineStart (timelineStartSamples),
      sessionSampleRate (sampleRate),
      sessionBpm (bpm),
      beatsPerBar (bpb),
      timeDisplayMode (displayMode),
      onCommit (std::move (commit)),
      onCancel (std::move (cancel))
{
    headerTitle.setText (summary.file.getFileName(), juce::dontSendNotification);
    headerTitle.setColour (juce::Label::textColourId, juce::Colours::white);
    headerTitle.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
    addAndMakeVisible (headerTitle);

    headerSubtitle.setText (summaryLine (summary), juce::dontSendNotification);
    headerSubtitle.setColour (juce::Label::textColourId, juce::Colour (0xffb0b0b8));
    headerSubtitle.setFont (juce::Font (juce::FontOptions (12.0f)));
    addAndMakeVisible (headerSubtitle);

    const auto placeAtText = juce::String ("Place at ")
                              + duskstudio::formatSamplePosition (
                                  timelineStart, sessionSampleRate, sessionBpm,
                                  beatsPerBar,
                                  (TimeDisplayMode) timeDisplayMode);
    headerPlaceAt.setText (placeAtText, juce::dontSendNotification);
    headerPlaceAt.setColour (juce::Label::textColourId, juce::Colour (0xff909094));
    headerPlaceAt.setFont (juce::Font (juce::FontOptions (11.0f)));
    addAndMakeVisible (headerPlaceAt);

    // Build per-track records + sort.
    std::vector<SortRecord> records;
    records.reserve (Session::kNumTracks);
    for (int i = 0; i < Session::kNumTracks; ++i)
    {
        const auto& t = session.track (i);
        const auto mode = (Track::Mode) t.mode.load (std::memory_order_relaxed);
        const int regionCount = summary.isMidi
                                   ? (int) t.midiRegions.current().size()
                                   : (int) t.regions.size();
        records.push_back ({ i, bucketFor (mode, regionCount,
                                              summary.isMidi, summary.numChannels) });
    }
    std::stable_sort (records.begin(), records.end(),
        [] (const SortRecord& a, const SortRecord& b)
        {
            if (a.bucket != b.bucket) return (int) a.bucket < (int) b.bucket;
            return a.trackIndex < b.trackIndex;
        });

    rows.reserve (records.size());
    for (size_t r = 0; r < records.size(); ++r)
    {
        const auto rec = records[r];
        const auto& t = session.track (rec.trackIndex);
        const auto mode = (Track::Mode) t.mode.load (std::memory_order_relaxed);
        const int regionCount = summary.isMidi
                                   ? (int) t.midiRegions.current().size()
                                   : (int) t.regions.size();

        auto row = std::make_unique<Row> (*this, (int) r);
        row->trackIndex  = rec.trackIndex;
        row->bucket      = rec.bucket;
        row->colour      = t.colour;
        row->displayName = trackDisplayName (t, rec.trackIndex);
        row->badgeText   = modeBadgeText (mode);
        row->occupancy   = regionCount == 0
                              ? juce::String ("empty")
                              : juce::String (regionCount)
                                  + (regionCount == 1 ? juce::String (" region")
                                                       : juce::String (" regions"));
        if (rec.bucket == Bucket::Mismatch)
        {
            const auto target = summary.isMidi
                                  ? juce::String ("MIDI")
                                  : (summary.numChannels == 2
                                          ? juce::String ("Stereo")
                                          : juce::String ("Mono"));
            row->mismatchHint = juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x90 will switch to "))
                              + target;
        }
        listContainer.addAndMakeVisible (row.get());
        rows.push_back (std::move (row));
    }

    if (! rows.empty())
    {
        // Default recommendation: top of the best bucket. If the caller
        // supplied a preferred track index (e.g. user drop-on-track),
        // override the recommendation to that row when it's present in
        // the list so the visual cue + Import button match user intent.
        recommendedRowIdx = 0;
        if (preferredTrackIndex >= 0 && preferredTrackIndex < Session::kNumTracks)
        {
            for (size_t i = 0; i < rows.size(); ++i)
            {
                if (rows[i]->trackIndex == preferredTrackIndex)
                {
                    recommendedRowIdx = (int) i;
                    break;
                }
            }
        }
        rows[(size_t) recommendedRowIdx]->recommended = true;
        selectedRowIdx = recommendedRowIdx;
    }

    listViewport.setViewedComponent (&listContainer, false);
    listViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (listViewport);

    cancelButton.onClick = [this] { if (onCancel) onCancel(); };
    importButton.onClick = [this] { commitSelection(); };
    importButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a5a3a));
    importButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    addAndMakeVisible (cancelButton);
    addAndMakeVisible (importButton);

    // setSize last — all children + listContainer rows are now created
    // and addAndMakeVisible'd, so the resized() that setSize fires lays
    // everything out. EmbeddedModal::show will only call setBounds
    // (position-only on a fixed-size panel), and setBounds with no size
    // change doesn't trigger resized() — meaning rows would never be
    // positioned if setSize ran at the top of the ctor.
    setSize (kPanelW, kPanelH);
}

ImportTargetPicker::~ImportTargetPicker() = default;

void ImportTargetPicker::selectRow (int index)
{
    if (index < 0 || index >= (int) rows.size()) return;
    selectedRowIdx = index;
    for (auto& r : rows) r->repaint();
}

void ImportTargetPicker::commitSelection()
{
    if (selectedRowIdx < 0 || selectedRowIdx >= (int) rows.size()) return;
    const auto& r = *rows[(size_t) selectedRowIdx];
    const int trackIndex = r.trackIndex;
    const bool needsModeFlip = (r.bucket == Bucket::Mismatch);

    if (! needsModeFlip)
    {
        if (onCommit) onCommit (trackIndex);
        return;
    }

    // Mode-flip needed (e.g. dropping a MIDI file on an audio track,
    // or vice versa). Explicit confirm so the user doesn't silently
    // discover their track changed mode after the fact.
    auto& trackRef = session.track (trackIndex);
    const auto currentMode = (Track::Mode) trackRef.mode.load (std::memory_order_relaxed);
    const Track::Mode newMode = summary.isMidi
                                   ? Track::Mode::Midi
                                   : (summary.numChannels == 2
                                          ? Track::Mode::Stereo
                                          : Track::Mode::Mono);

    auto modeName = [] (Track::Mode m) -> juce::String
    {
        switch (m)
        {
            case Track::Mode::Midi:   return "MIDI";
            case Track::Mode::Stereo: return "Stereo";
            case Track::Mode::Mono:   default: return "Mono";
        }
    };

    auto* host = getTopLevelComponent();
    if (host == nullptr) host = this;

    const auto title   = juce::String ("Switch track to ") + modeName (newMode) + "?";
    const auto message = juce::String ("Track ") + juce::String (trackIndex + 1)
                       + " is currently in " + modeName (currentMode)
                       + " mode. Importing this " + (summary.isMidi ? "MIDI" : "audio")
                       + " file will switch the track to "
                       + modeName (newMode) + " mode. Proceed?";

    juce::Component::SafePointer<juce::Component> safeHost (host);
    Session* sessionPtr = &session;
    auto commitCb       = onCommit;
    auto isMidiCopy     = summary.isMidi;
    auto numChCopy      = summary.numChannels;
    showDuskConfirm (*host, title, message,
                       /*primary*/   "Switch",
                       /*onPrimary*/ [safeHost, sessionPtr, trackIndex, commitCb,
                                       isMidiCopy, numChCopy]
                       {
                           if (safeHost.getComponent() == nullptr || sessionPtr == nullptr) return;
                           const Track::Mode m = isMidiCopy
                                                    ? Track::Mode::Midi
                                                    : (numChCopy == 2
                                                            ? Track::Mode::Stereo
                                                            : Track::Mode::Mono);
                           sessionPtr->track (trackIndex).mode.store ((int) m,
                                                                        std::memory_order_relaxed);
                           if (commitCb) commitCb (trackIndex);
                       },
                       /*secondary*/   "Cancel",
                       /*onSecondary*/ {});
}

void ImportTargetPicker::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1a1a20));
}

void ImportTargetPicker::resized()
{
    auto bounds = getLocalBounds().reduced (12, 10);

    // Header.
    {
        auto header = bounds.removeFromTop (kHeaderH);
        headerTitle.setBounds    (header.removeFromTop (22));
        headerSubtitle.setBounds (header.removeFromTop (18));
        headerPlaceAt.setBounds  (header.removeFromTop (16));
    }
    bounds.removeFromTop (6);

    // Footer.
    auto footer = bounds.removeFromBottom (kFooterH);
    {
        auto row = footer.reduced (0, 8);
        importButton.setBounds (row.removeFromRight (110).reduced (2));
        row.removeFromRight (8);
        cancelButton.setBounds (row.removeFromRight (90).reduced (2));
    }
    bounds.removeFromBottom (4);

    listViewport.setBounds (bounds);
    int y = kListPad;
    const int rowW = bounds.getWidth() - 18;   // leave room for scrollbar
    for (auto& rPtr : rows)
    {
        rPtr->setBounds (0, y, rowW, kRowH);
        y += kRowH + kRowGap;
    }
    listContainer.setSize (rowW, juce::jmax (y + kListPad, bounds.getHeight()));
}
} // namespace duskstudio
