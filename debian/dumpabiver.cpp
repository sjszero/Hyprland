#include <src/plugins/PluginAPI.hpp>
#include <openssl/sha.h>
#include <iostream>
#include <iomanip>
#include <sstream>

std::string hyprland_api() {
    return HYPRLAND_API_VERSION;
}

std::string hyprland_full_abi() {
    return __hyprland_api_get_client_hash();
}

std::string shorten_abi_hash(std::string full_hash) {
    static unsigned char hash[SHA_DIGEST_LENGTH];

    SHA1(
         reinterpret_cast<const unsigned char *>(full_hash.c_str()),
         full_hash.size(),
         hash
    );

    // Convert the raw bytes to a hexadecimal string
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (const auto& byte : hash) {
        ss << std::setw(2) << static_cast<int>(byte);
    }

    return ss.str().substr(0, 7);
}

int main() {
    const std::string abi_full_hash = hyprland_full_abi();
    const std::string abi_short_hash = shorten_abi_hash(abi_full_hash);
    const std::string api_version = hyprland_api();

    std::cout
        << "# __hyprland_api_get_hash() ~> \"" << abi_full_hash << "\""<< std::endl
        << "# HYPRLAND_ABI contains truncated sha1 of the above string" << std::endl
        << "HYPRLAND_ABI=hyprland-abi-" << abi_short_hash << std::endl
        << "HYPRLAND_API=hyprland-api-" << api_version << std::endl;
    return 0;
}
