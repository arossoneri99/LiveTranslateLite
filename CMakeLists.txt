cmake_minimum_required(VERSION 3.21)
project(LiveTranslateLite LANGUAGES CXX)

add_executable(LiveTranslateLite WIN32
    src/main.cpp
)

target_compile_features(LiveTranslateLite PRIVATE cxx_std_20)
target_compile_definitions(LiveTranslateLite PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
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
    # Static MSVC runtime: the resulting EXE does not require installing the VC++ runtime.
    set_property(TARGET LiveTranslateLite PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(LiveTranslateLite PRIVATE /W4 /permissive- /EHsc)
endif()
