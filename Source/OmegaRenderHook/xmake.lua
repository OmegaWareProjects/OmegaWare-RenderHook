add_requires("safetyhook")

target("OmegaRenderHook")
    set_kind("static")
    set_targetdir(path.join("$(projectdir)", "Build", is_mode("debug") and "Debug" or "Release", "$(arch)"))
    set_languages("c++latest")
    set_encodings("utf-8")
    add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN", "UNICODE", "_UNICODE")

    add_includedirs(path.join(os.scriptdir(), ".."), { public = true })
    add_files(path.join(os.scriptdir(), "**.cpp"))

    local strVulkanSdk = os.getenv("VULKAN_SDK")
    if strVulkanSdk then
        add_defines("OMEGARENDERHOOK_VULKAN=1", { public = true })
        add_includedirs(path.join(strVulkanSdk, "Include"), { public = true })
    end

    add_packages("safetyhook", { public = true })
    add_syslinks("user32", "d3d11", "d3d12", "d3dcompiler", "dxgi", "dxguid", "gdi32", "opengl32", { public = true })
target_end()
