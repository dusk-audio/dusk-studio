#include "MultiImportTargetPicker.h"

#include <algorithm>
#include <cmath>

namespace duskstudio
{
namespace
{
constexpr int kHeaderH = 64;
constexpr int kFooterH = 44;
constexpr int kRowH    = 52;
constexpr int kListPad = 6;
constexpr int kPanelW  = 640;
constexpr int kPanelMinH = 240;
// No artificial upper cap - EmbeddedModal clamps to the host's bounds, so
// the modal grows to fit the file count (1..16) and only the host window's
// height becomes the ceiling. Up to 16 rows = ~880 px, fits comfortably on
// any reasonable display.

juce::String trackDisplayName (const Track& t, int idx)
{
    const auto raw = t.name.trim();
    if (raw.isEmpty() || raw == juce::String (idx + 1))
        return juce::String ("Track ") + juce::String (idx + 1);
    return juce::String ("Track ") + juce::String (idx + 1) + ": " + raw;
}

juce::String modeBadge (Track::Mode m)
{
    switch (m)
    {
        case Track::Mode::Mono:   return juce::String ("Mono");
        case Track::Mode::Stereo: return juce::String ("Stereo");
        case Track::Mode::Midi:   return juce::String ("MIDI");
    }
    return {};
}

juce::String formatDuration (double seconds)
{
    if (seconds < 60.0)
        return juce::String (seconds, 1) + " s";

    const auto total = (juce::int64) std::round (seconds);
    const int h = (int) (total / 3600);
    const int m = (int) ((total % 3600) / 60);
    const int s = (int) (total % 60);
    if (h > 0)
        return juce::String (h) + ":"
                + juce::String (m).paddedLeft ('0', 2) + ":"
                + juce::String (s).paddedLeft ('0', 2);
    return juce::String (m) + ":" + juce::String (s).paddedLeft ('0', 2);
}

juce::String summaryText (const ImportTargetPicker::FileSummary& s)
{
    if (s.isMidi)
    {
        juce::String out = "MIDI";
        if (s.numMidiNotes > 0) out += " - " + juce::String (s.numMidiNotes) + " notes";
        return out;
    }
    const double secs = (s.sampleRate > 0.0)
                          ? (double) s.lengthSamples / s.sampleRate
                          : 0.0;
    juce::String channels;
    if      (s.numChannels == 0) channels = "unknown";
    else if (s.numChannels == 1) channels = "mono";
    else if (s.numChannels == 2) channels = "stereo";
    else                         channels = juce::String (s.numChannels) + " channels";
    return juce::String (s.sampleRate / 1000.0, 1) + " kHz - "
            + channels + " - " + formatDuration (secs);
}

bool modeMatches (Track::Mode m, const ImportTargetPicker::FileSummary& s)
{
    if (s.isMidi)             return m == Track::Mode::Midi;
    if (s.numChannels == 2)   return m == Track::Mode::Stereo;
    if (s.numChannels == 1)   return m == Track::Mode::Mono;
    return false;   // 0 or >2 - no clean audio match, force "will flip" badge
}
} // namespace

struct MultiImportTargetPicker::Row : public juce::Component
{
    Row (Session& s, const ImportTargetPicker::FileSummary& summary,
         std::function<void()> onPick)
        : session (s), pickCallback (std::move (onPick))
    {
        nameLabel.setText (summary.file.getFileName(), juce::dontSendNotification);
        nameLabel.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        nameLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe0e0e0));
        addAndMakeVisible (nameLabel);

        summaryLabel.setText (summaryText (summary), juce::dontSendNotification);
        summaryLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
        summaryLabel.setColour (juce::Label::textColourId, juce::Colour (0xff909094));
        addAndMakeVisible (summaryLabel);

        trackPicker.addItem ("Choose track...", 1);
        for (int i = 0; i < Session::kNumTracks; ++i)
        {
            const auto& t = session.track (i);
            const auto mode = (Track::Mode) t.mode.load (std::memory_order_relaxed);
            const bool match = modeMatches (mode, summary);
            juce::String label = trackDisplayName (t, i) + "  (" + modeBadge (mode) + ")";
            if (! match) label += "  - mode will flip";
            trackPicker.addItem (label, i + 2);   // IDs 2..17
        }
        trackPicker.setSelectedId (1, juce::dontSendNotification);
        trackPicker.onChange = [this] { if (pickCallback) pickCallback(); };
        addAndMakeVisible (trackPicker);
    }

    int chosenTrack() const
    {
        const int id = trackPicker.getSelectedId();
        return id <= 1 ? -1 : (id - 2);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (juce::Colour (0xff202028));
        g.fillRoundedRectangle (r, 4.0f);
        const bool assigned = chosenTrack() >= 0;
        const auto dot = juce::Rectangle<float> (8.0f, getHeight() / 2.0f - 4.0f, 8.0f, 8.0f);
        g.setColour (assigned ? juce::Colour (0xff3fd070)
                              : juce::Colour (0xff505058));
        g.fillEllipse (dot);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (24, 6);
        const int pickerW = juce::jmin (260, area.getWidth() / 2);
        trackPicker.setBounds (area.removeFromRight (pickerW).reduced (0, 4));
        area.removeFromRight (12);
        nameLabel   .setBounds (area.removeFromTop (area.getHeight() / 2));
        summaryLabel.setBounds (area);
    }

    Session& session;
    juce::Label nameLabel;
    juce::Label summaryLabel;
    juce::ComboBox trackPicker;
    std::function<void()> pickCallback;
};

MultiImportTargetPicker::MultiImportTargetPicker (Session& s,
                                                    std::vector<ImportTargetPicker::FileSummary> sums,
                                                    juce::int64 timelineStartSamples,
                                                    std::function<void (std::vector<Assignment>)> commit,
                                                    std::function<void()> cancel)
    : session (s),
      summaries (std::move (sums)),
      timelineStart (timelineStartSamples),
      onCommit (std::move (commit)),
      onCancel (std::move (cancel))
{
    setOpaque (true);

    headerTitle.setText ("Import " + juce::String ((int) summaries.size()) + " files",
                          juce::dontSendNotification);
    headerTitle.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
    headerTitle.setColour (juce::Label::textColourId, juce::Colour (0xfff0f0f0));
    addAndMakeVisible (headerTitle);

    headerSubtitle.setText ("Pick a target track for each file. Same-track picks "
                              "are allowed - files stack at the playhead.",
                              juce::dontSendNotification);
    headerSubtitle.setFont (juce::Font (juce::FontOptions (11.5f)));
    headerSubtitle.setColour (juce::Label::textColourId, juce::Colour (0xff909094));
    addAndMakeVisible (headerSubtitle);

    listViewport.setViewedComponent (&listContainer, false);
    listViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (listViewport);

    for (const auto& sum : summaries)
    {
        auto row = std::make_unique<Row> (session, sum,
                                            [this] { rebuildImportEnabled(); repaint(); });
        listContainer.addAndMakeVisible (row.get());
        rows.push_back (std::move (row));
    }

    cancelButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff404048));
    cancelButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffd0d0d0));
    cancelButton.onClick = [this] { if (onCancel) onCancel(); };
    addAndMakeVisible (cancelButton);

    importButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff406030));
    importButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff70b050));
    importButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff707074));
    importButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xfff0f0f0));
    importButton.setClickingTogglesState (false);
    importButton.onClick = [this]
    {
        if (! importButton.isEnabled()) return;
        if (onCommit) onCommit (collectAssignments());
    };
    addAndMakeVisible (importButton);
    rebuildImportEnabled();

    // Exact-fit height: header + per-file rows + footer + outer padding.
    // EmbeddedModal clamps to its host's bounds, so on tiny windows the
    // viewport still scrolls. On normal windows all rows are visible at
    // once - no wasted scroll-band for 3 files in a 1080p modal.
    const int listH = (int) rows.size() * kRowH + kListPad * 2;
    const int wantH = juce::jmax (kPanelMinH,
                                     kHeaderH + listH + kFooterH + 24);
    setSize (kPanelW, wantH);
}

MultiImportTargetPicker::~MultiImportTargetPicker() = default;

void MultiImportTargetPicker::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff181820));
}

void MultiImportTargetPicker::resized()
{
    auto area = getLocalBounds().reduced (16, 12);

    auto header = area.removeFromTop (kHeaderH);
    headerTitle   .setBounds (header.removeFromTop (28));
    header.removeFromTop (4);
    headerSubtitle.setBounds (header.removeFromTop (28));
    area.removeFromTop (6);

    auto footer = area.removeFromBottom (kFooterH);
    footer.removeFromTop (8);
    const int btnW = 96;
    cancelButton.setBounds (footer.removeFromLeft  (btnW).reduced (0, 4));
    importButton.setBounds (footer.removeFromRight (btnW).reduced (0, 4));

    listViewport.setBounds (area);

    const int innerW = juce::jmax (0, listViewport.getMaximumVisibleWidth());
    const int innerH = (int) rows.size() * kRowH + kListPad;
    listContainer.setSize (innerW, innerH);
    int y = 0;
    for (auto& row : rows)
    {
        row->setBounds (0, y, innerW, kRowH - 2);
        y += kRowH;
    }
}

void MultiImportTargetPicker::rebuildImportEnabled()
{
    bool allAssigned = ! rows.empty();
    for (const auto& row : rows)
        if (row->chosenTrack() < 0) { allAssigned = false; break; }
    importButton.setEnabled (allAssigned);
    importButton.setToggleState (allAssigned, juce::dontSendNotification);
}

std::vector<MultiImportTargetPicker::Assignment>
MultiImportTargetPicker::collectAssignments() const
{
    std::vector<Assignment> out;
    out.reserve (rows.size());
    for (size_t i = 0; i < rows.size(); ++i)
    {
        Assignment a;
        a.file       = summaries[i].file;
        a.trackIndex = rows[i]->chosenTrack();
        a.isMidi     = summaries[i].isMidi;
        out.push_back (std::move (a));
    }
    return out;
}
} // namespace duskstudio
