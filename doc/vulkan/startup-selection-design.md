# Viewer startup backend selection

FS_RENDER_BACKEND is read once. Missing/empty/opengl selects OpenGL; vulkan selects Vulkan when LL_VULKAN_RUNTIME is
built, otherwise selects OpenGL with a reason. Unknown values select OpenGL with a diagnostic. Changing the environment
later cannot switch the selected policy or installed API.

LLViewerWindow passes the native flag before graphics object creation. Win32 installs a complete candidate renderer and
initializes capabilities before shader compilation. Failure may fall back to GL only before publication. Getters expose
the installed native set; otherwise they preserve the existing GL factories and ownership. A prior backend allocation
or WGL context prevents native activation. isVulkanRenderBackendActive() is authoritative when startup fallback occurs.

OpenGL is the default; no saved setting or system environment is modified by the supplied process-local launchers.
The 12 selector/factory tests cover runtime-on/off requests, immutable selection and factory ordering/lifetime. Actual
native activation/fallback is covered by viewer_vulkan_activation_test; mock factory tests do not prove GPU behavior.
