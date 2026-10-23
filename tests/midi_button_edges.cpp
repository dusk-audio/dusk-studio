#include <catch2/catch_test_macros.hpp>

#include "engine/MidiButtonEdges.h"

#include <array>

using namespace duskstudio;

namespace
{
using Inputs = std::array<MidiButtonEdges, 2>;

template <typename Callback>
void eachSource (Inputs& inputs, Callback callback)
{
    for (int input = 0; input < 2; ++input)
        for (int channel = 1; channel <= 16; ++channel)
            for (int number = 0; number < 128; ++number)
            {
                INFO ("input=" << input << " channel=" << channel << " number=" << number);
                callback (inputs[static_cast<std::size_t> (input)], input, channel, number);
            }
}
}

TEST_CASE ("MIDI button edges separate every input channel controller and note", "[midi][edges]")
{
    Inputs inputs {};
    eachSource (inputs, [] (auto& edges, int, int channel, int number)
    {
        REQUIRE (edges.controller (channel, number, 64));
        REQUIRE (edges.note (channel, number, 1));
    });
    eachSource (inputs, [] (auto& edges, int, int channel, int number)
    {
        REQUIRE_FALSE (edges.controller (channel, number, 127));
        REQUIRE_FALSE (edges.note (channel, number, 127));
    });
    REQUIRE_FALSE (inputs[0].controller (8, 64, 63));
    REQUIRE_FALSE (inputs[1].note (16, 127, 0));
    eachSource (inputs, [] (auto& edges, int input, int channel, int number)
    {
        REQUIRE (edges.controller (channel, number, 127) == (input == 0 && channel == 8 && number == 64));
        REQUIRE (edges.note (channel, number, 1) == (input == 1 && channel == 16 && number == 127));
    });
    eachSource (inputs, [] (auto& edges, int, int channel, int number)
    {
        REQUIRE_FALSE (edges.controller (channel, number, 0));
        REQUIRE_FALSE (edges.note (channel, number, 0));
    });
    eachSource (inputs, [] (auto& edges, int, int channel, int number)
    {
        REQUIRE (edges.controller (channel, number, 64));
        REQUIRE (edges.note (channel, number, 1));
    });
}

TEST_CASE ("MIDI button reset clears the complete input without rearming its neighbor", "[midi][edges]")
{
    Inputs inputs {};
    eachSource (inputs, [] (auto& edges, int, int channel, int number)
    {
        REQUIRE (edges.controller (channel, number, 127));
        REQUIRE (edges.note (channel, number, 127));
    });
    for (int resetInput = 0; resetInput < 2; ++resetInput)
    {
        inputs[static_cast<std::size_t> (resetInput)].reset();
        eachSource (inputs, [resetInput] (auto& edges, int input, int channel, int number)
        {
            REQUIRE (edges.controller (channel, number, 127) == (input == resetInput));
            REQUIRE (edges.note (channel, number, 1) == (input == resetInput));
        });
    }
    inputs[0].reset(); inputs[1].reset();
    eachSource (inputs, [] (auto& edges, int, int channel, int number)
    {
        REQUIRE_FALSE (edges.controller (channel, number, 63));
        REQUIRE_FALSE (edges.note (channel, number, 0));
        REQUIRE (edges.controller (channel, number, 64));
        REQUIRE (edges.note (channel, number, 1));
    });
}

TEST_CASE ("MIDI button edges reject invalid keys without changing any valid source", "[midi][edges]")
{
    Inputs inputs {};
    eachSource (inputs, [] (auto& edges, int, int channel, int number)
    {
        REQUIRE (edges.controller (channel, number, 64));
        REQUIRE (edges.note (channel, number, 1));
    });
    for (auto& edges : inputs)
        for (int channel : { -1, 0, 1, 16, 17, 127 })
            for (int number : { -1, 0, 63, 64, 127, 128 })
            {
                if (channel >= 1 && channel <= 16 && number >= 0 && number < 128) continue;
                INFO ("invalid channel=" << channel << " number=" << number);
                REQUIRE_FALSE (edges.controller (channel, number, 127));
                REQUIRE_FALSE (edges.note (channel, number, 1));
                REQUIRE_FALSE (edges.controller (channel, number, 0));
                REQUIRE_FALSE (edges.note (channel, number, 0));
            }
    eachSource (inputs, [] (auto& edges, int, int channel, int number)
    {
        REQUIRE_FALSE (edges.controller (channel, number, 127));
        REQUIRE_FALSE (edges.note (channel, number, 1));
    });
}
