include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

function(duskstudio_check_shared_wayland result)
    set(${result} OFF PARENT_SCOPE)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT DUSKSTUDIO_JUCE_HAS_WAYLAND
       OR DGL_BACKEND STREQUAL "x11"
       OR NOT EXISTS "${DUSKSTUDIO_DAF_SOURCE_DIR}/dgl/WaylandInput.hpp")
        return()
    endif()

    find_package(PkgConfig QUIET)
    if(NOT PKG_CONFIG_FOUND)
        return()
    endif()
    pkg_check_modules(DUSK_NATIVE_WAYLAND QUIET wayland-client wayland-cursor xkbcommon libdecor-0)
    if(NOT DUSK_NATIVE_WAYLAND_FOUND)
        return()
    endif()

    cmake_push_check_state(RESET)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_REQUIRED_INCLUDES
        "${DUSKSTUDIO_JUCE_SOURCE_DIR}/modules"
        "${DUSKSTUDIO_DAF_SOURCE_DIR}/dgl"
        ${DUSK_NATIVE_WAYLAND_INCLUDE_DIRS})
    unset(DUSKSTUDIO_SHARED_WAYLAND_INPUT_API CACHE)
    check_cxx_source_compiles([=[
        #define JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED 1
        #define JUCE_WAYLAND 1
        #define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1
        #include <juce_gui_basics/juce_gui_basics.h>
        #include <WaylandInput.hpp>
        void probe(juce::WaylandWindowSystem& host, juce::ComponentPeer* peer, DGL::WaylandInput input)
        {
            auto* display = host.getDisplay();
            auto* seat = host.getSeat();
            juce::WaylandWindowSystem::InputForwarder forwarding {
                input.keyboard, input.pointer, input.data, input.consumed,
                input.hasKeyboardFocus, input.releaseFocus, input.reset
            };
            auto token = host.attachInputForwarder(forwarding);
            host.detachInputForwarder(token);
            auto* surface = host.getSurfaceForWindow(host.getWaylandWindowForPeer(peer));
            host.forgetInputSurface(surface);
            juce::WaylandSymbols::getInstance()->surfaceCommit(surface);
            juce::WaylandSymbols::getInstance()->displayFlush(display);
            (void)seat;
        }
    ]=] DUSKSTUDIO_SHARED_WAYLAND_INPUT_API)

    if(DUSKSTUDIO_SHARED_WAYLAND_INPUT_API)
        set(probe_dir "${CMAKE_CURRENT_BINARY_DIR}/native-wayland-probe")
        file(MAKE_DIRECTORY "${probe_dir}")
        file(WRITE "${probe_dir}/DafPluginInfo.h" [=[
            #define DAF_PLUGIN_BRAND "Dusk"
            #define DAF_PLUGIN_NAME "Native capability probe"
            #define DAF_PLUGIN_URI "urn:dusk:native-capability-probe"
            #define DAF_PLUGIN_HAS_UI 1
            #define DAF_PLUGIN_NUM_INPUTS 2
            #define DAF_PLUGIN_NUM_OUTPUTS 2
        ]=])
        set(CMAKE_REQUIRED_INCLUDES
            "${DUSKSTUDIO_DAF_SOURCE_DIR}"
            "${DUSKSTUDIO_DAF_SOURCE_DIR}/daf"
            "${DUSKSTUDIO_DAF_SOURCE_DIR}/dgl"
            "${DUSKSTUDIO_DAF_SOURCE_DIR}/dgl/src/pugl-upstream/include"
            "${probe_dir}")
        unset(DUSKSTUDIO_SHARED_WAYLAND_EDITOR_API CACHE)
        check_cxx_source_compiles([=[
            #define DAF_PLUGIN_TARGET_STATIC
            #include "daf/src/DafUIInternal.hpp"
            #include <type_traits>
            using App = DGL::Application;
            static_assert(std::is_constructible<App, App::ExternalWaylandDisplay>::value, "borrowed display");
            void probe(App& app, DGL::Window& child, void* display, void* seat, void* surface)
            {
                App::ExternalWaylandDisplay borrowed(display, seat);
                auto input = app.getWaylandInput();
                app.activateWaylandSurface(surface, "probe");
                app.setClipboardCallbacks({});
                child.setEmbeddedSurfaceGeometry(0, 0, 2, 2, 1.0);
                child.setEmbeddedVisible(false);
                child.getCompletedDisplayCount();
                DAF::UIExporter editor(nullptr, 0, 48000.0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                    nullptr, nullptr, 1.0, App::kTypeAuto, 0, 0xffffffff, "dusk-probe", App::kWindowSystemAuto, &app);
                editor.setWindowEmbeddedVisible(false);
                editor.setWindowEmbeddedSurfaceGeometry(0, 0, 2, 2, 1.0);
                editor.getCompletedDisplayCount();
                (void)borrowed;
                (void)input;
            }
        ]=] DUSKSTUDIO_SHARED_WAYLAND_EDITOR_API)
        if(DUSKSTUDIO_SHARED_WAYLAND_EDITOR_API)
            set(${result} ON PARENT_SCOPE)
        endif()
    endif()
    cmake_pop_check_state()
endfunction()
