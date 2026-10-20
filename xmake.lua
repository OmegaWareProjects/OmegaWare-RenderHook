set_project("OmegaWare-RenderHook")
set_version("0.1.0")

add_rules("mode.debug", "mode.release")

if is_mode("debug") then
    set_symbols("debug", "edit")
end

set_languages("c++latest")
set_warnings("all")
set_encodings("utf-8")
set_runtimes(is_mode("debug") and "MTd" or "MT")
add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN", "UNICODE", "_UNICODE")

includes("Source/OmegaRenderHook/xmake.lua")

local strOutputDir = path.join("Build", is_mode("debug") and "Debug" or "Release", "$(arch)")

target("TestOverlay")
    set_kind("shared")
    set_targetdir(strOutputDir)
    set_filename("OmegaRenderHook-TestOverlay.dll")
    add_deps("OmegaRenderHook")
    add_files("Source/Tools/TestOverlay/**.cpp")
target_end()

target("Injector")
    set_kind("binary")
    set_targetdir(strOutputDir)
    set_filename("OmegaRenderHook-Injector.exe")
    add_files("Source/Tools/Injector/**.cpp")
    add_syslinks("user32")
target_end()

target("SelfTest")
    set_kind("binary")
    set_targetdir(strOutputDir)
    set_filename("OmegaRenderHook-SelfTest.exe")
    add_deps("OmegaRenderHook")
    add_files("Source/SelfTest/**.cpp")
target_end()
