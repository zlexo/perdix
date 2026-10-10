# Sourced by the scripts that run the engine's programs on macOS: the
# Vulkan loader is found through the SDK's DYLD_LIBRARY_PATH, which the
# system strips from every program started through a protected binary, and
# a script started through env or bash is one. VULKAN_SDK, exported beside
# it, survives: the loader's folder is put back from it. Without it, the
# newest SDK in ~/VulkanSDK is set up. Returns 1, saying why, without one.
vulkan_environment() {
    [ "$(uname -s)" = Darwin ] || return 0
    if [ -n "${VULKAN_SDK:-}" ]; then
        export DYLD_LIBRARY_PATH="$VULKAN_SDK/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
        return 0
    fi
    local setup
    setup=$(ls -d "$HOME"/VulkanSDK/*/setup-env.sh 2>/dev/null | sort -V | tail -n 1)
    if [ -z "$setup" ]; then
        echo "no Vulkan SDK in ~/VulkanSDK, so there is no Vulkan loader" >&2
        return 1
    fi
    # The SDK's script reads variables that may be unset.
    set +u
    # shellcheck disable=SC1090 # the SDK's own script
    source "$setup" >/dev/null
    set -u
}
