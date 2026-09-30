cmake_minimum_required(VERSION 3.21)
project(LiveTranslateLite LANGUAGES CXX)

add_executable(LiveTranslateLite WIN32
    src/main.cpp
)

target_compile_features(LiveTranslateLite PRIVATE cxx_std_20)

target_compile_definitions(LiveTranslateLite PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

target_link_libraries(LiveTranslateLite PRIVATE
    user32
    gdi32
    shell32
    ole32
    oleaut32
    winhttp
    uiautomationcore
)

if(MSVC)
    # Build the C/C++ runtime into the EXE so the target PC does not need
    # a separate Visual C++ runtime installation.
    set_property(TARGET LiveTranslateLite PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # /permissive- was intentionally removed here. The application does not
    # need it, and keeping the compiler settings conservative avoids needless
    # friction with Windows SDK generated COM headers.
    target_compile_options(LiveTranslateLite PRIVATE /W4 /EHsc)
endif()
