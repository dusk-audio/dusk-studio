#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../../session/MarkerEditActions.h"
#include "../../../session/Session.h"

#include <cstdint>
#include <string>

namespace duskstudio::scenario
{
namespace
{
constexpr std::int64_t kSecond = 48000;

bool markerIs (const Session& session, int index, const char* name, std::int64_t at)
{
    const auto& markers = session.getMarkers();
    return index >= 0 && index < (int) markers.size()
        && markers[(std::size_t) index].name == name
        && markers[(std::size_t) index].timelineSamples == at;
}

std::string describe (const Session& session)
{
    std::string text;
    for (const auto& marker : session.getMarkers())
        text += " " + marker.name.toStdString() + "@" + std::to_string (marker.timelineSamples);
    return text.empty() ? " (none)" : text;
}

// M and the ruler menu add through AddMarkerAction, a pill drag lands as a
// MoveMarkerAction, and Delete is a RemoveMarkerAction; every one of them is a
// single undo step, and the list stays in timeline order throughout.
ScenarioResult addMoveRemoveUndo (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();

    undo.beginNewTransaction();
    auto* first = new AddMarkerAction (session, 2 * kSecond);
    ctx.expect (undo.perform (first), "adding a marker was refused");
    ctx.expect (markerIs (session, 0, "Marker 1", 2 * kSecond),
                "an unnamed marker did not take the next default name:" + describe (session));

    undo.beginNewTransaction();
    auto* verse = new AddMarkerAction (session, kSecond, "Verse");
    ctx.expect (undo.perform (verse), "adding a named marker was refused");
    ctx.expect (verse->insertedIndex() == 0, "a marker added earlier on the timeline was not inserted first");
    ctx.expect (markerIs (session, 0, "Verse", kSecond) && markerIs (session, 1, "Marker 1", 2 * kSecond),
                "the markers are not in timeline order:" + describe (session));

    // The drag moves the pill live and commits the move on release.
    session.getMarkers()[1].timelineSamples = kSecond / 2;
    undo.beginNewTransaction();
    ctx.expect (undo.perform (new MoveMarkerAction (session, "Marker 1", 2 * kSecond, kSecond / 2)),
                "committing a drag was refused");
    ctx.expect (undo.undo(), "undoing the drag was refused");
    ctx.expect (markerIs (session, 0, "Verse", kSecond) && markerIs (session, 1, "Marker 1", 2 * kSecond),
                "undoing the drag did not put the marker back:" + describe (session));
    ctx.expect (undo.redo(), "redoing the drag was refused");
    ctx.expect (markerIs (session, 0, "Marker 1", kSecond / 2) && markerIs (session, 1, "Verse", kSecond),
                "a marker dragged past another did not re-sort:" + describe (session));

    undo.beginNewTransaction();
    ctx.expect (undo.perform (new RemoveMarkerAction (session, 1)), "deleting a marker was refused");
    ctx.expect (session.getMarkers().size() == 1 && markerIs (session, 0, "Marker 1", kSecond / 2),
                "delete removed the wrong marker:" + describe (session));
    ctx.expect (undo.undo() && markerIs (session, 1, "Verse", kSecond),
                "undoing the delete did not restore the marker where it was:" + describe (session));

    ctx.expect (undo.undo() && markerIs (session, 0, "Verse", kSecond)
                    && markerIs (session, 1, "Marker 1", 2 * kSecond),
                "undoing the drag again did not put the marker back:" + describe (session));
    ctx.expect (undo.undo() && session.getMarkers().size() == 1
                    && markerIs (session, 0, "Marker 1", 2 * kSecond),
                "undoing the named add did not remove just that marker:" + describe (session));
    ctx.expect (undo.undo() && session.getMarkers().empty(),
                "undoing the first add left a marker behind:" + describe (session));

    // Rename is its own undo step in the tape strip, applied through this.
    session.addMarker (kSecond, "Verse");
    session.renameMarker (0, "Chorus");
    ctx.expect (markerIs (session, 0, "Chorus", kSecond), "the rename did not take:" + describe (session));
    return ctx.verdict();
}

// Rewind and Forward taps: previous and next marker, strictly before or after
// the playhead, falling back to bar 1 going back and staying put past the last.
ScenarioResult rewindForwardJump (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();

    transport.setPlayhead (2 * kSecond);
    engine.jumpToPrevMarker();
    ctx.expect (transport.getPlayhead() == 0, "Rewind with no markers did not go to bar 1");
    transport.setPlayhead (2 * kSecond);
    engine.jumpToNextMarker();
    ctx.expect (transport.getPlayhead() == 2 * kSecond, "Forward with no markers moved the playhead");

    session.addMarker (kSecond, "Verse");
    session.addMarker (3 * kSecond, "Chorus");

    transport.setPlayhead (2 * kSecond);
    engine.jumpToPrevMarker();
    ctx.expect (transport.getPlayhead() == kSecond, "Rewind did not land on the previous marker");
    engine.jumpToPrevMarker();
    ctx.expect (transport.getPlayhead() == 0,
                "Rewind from the first marker did not go to bar 1");

    engine.jumpToNextMarker();
    ctx.expect (transport.getPlayhead() == kSecond, "Forward from bar 1 did not land on the first marker");
    engine.jumpToNextMarker();
    ctx.expect (transport.getPlayhead() == 3 * kSecond, "Forward did not step to the next marker");
    engine.jumpToNextMarker();
    ctx.expect (transport.getPlayhead() == 3 * kSecond, "Forward overshot the last marker");

    // Sitting on a marker, Rewind steps to the one before it rather than
    // restating the current position.
    engine.jumpToPrevMarker();
    ctx.expect (transport.getPlayhead() == kSecond, "Rewind on a marker did not step back past it");

    // A jump during playback is also where Stop comes back to.
    engine.play();
    ctx.pump (1);
    engine.jumpToNextMarker();
    ctx.expect (transport.getPlayhead() == 3 * kSecond && transport.getRollStart() == 3 * kSecond,
                "a jump during playback did not become the roll start");
    engine.stop();
    return ctx.verdict();
}

const ScenarioRegistrar addRegistrar { Scenario {
    "marker.add_move_remove_undo", { "marker", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return addMoveRemoveUndo (ctx); } } };
const ScenarioRegistrar jumpRegistrar { Scenario {
    "marker.rewind_forward_jump", { "marker", "transport" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return rewindForwardJump (ctx); } } };
} // namespace
} // namespace duskstudio::scenario
