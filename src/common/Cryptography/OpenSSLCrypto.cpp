/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "OpenSSLCrypto.h"
#include "Log.h"
#include <openssl/crypto.h> // NOTE: this import is NEEDED (even though some IDEs report it as unused)
#include <openssl/err.h>
#include <openssl/provider.h>
#include <string>

OSSL_PROVIDER* LegacyProvider;
OSSL_PROVIDER* DefaultProvider;

#if AC_PLATFORM == AC_PLATFORM_WINDOWS
#include <boost/dll/runtime_symbol_info.hpp>
#include <filesystem>
#include <Windows.h>

void SetupLibrariesForWindows()
{
    namespace fs = std::filesystem;

    fs::path programLocation{ boost::dll::program_location().remove_filename().string() };
    fs::path libLegacy{ boost::dll::program_location().remove_filename().string() + "/legacy.dll" };

    ASSERT(fs::exists(libLegacy), "Not found 'legacy.dll'. Please copy library 'legacy.dll' from OpenSSL default dir to '{}'", programLocation.generic_string());
    OSSL_PROVIDER_set_default_search_path(nullptr, programLocation.generic_string().c_str());
}
#endif

bool OpenSSLCrypto::threadsSetup()
{
#if AC_PLATFORM == AC_PLATFORM_WINDOWS
    SetupLibrariesForWindows();
#endif
    LegacyProvider = OSSL_PROVIDER_load(nullptr, "legacy");
    DefaultProvider = OSSL_PROVIDER_load(nullptr, "default");

    if (LegacyProvider && DefaultProvider)
        return true;

    // Without the legacy provider RC4 (client<->world packet encryption) is unavailable and the first client
    // connection would die later in Acore::Crypto::ARC4::ARC4, so report the reason loudly here.
    // On Windows legacy.dll imports libcrypto-3-x64.dll, which is resolved by *name* through the normal DLL search
    // order (exe dir, System32, PATH). If another libcrypto-3-x64.dll of a different OpenSSL version is found first
    // (Git, MySQL, poppler, ... on PATH) legacy.dll cannot be loaded.
    char errBuf[256] = {};
    ERR_error_string_n(ERR_peek_last_error(), errBuf, sizeof(errBuf));

    LOG_ERROR("server", "OpenSSL: failed to load the '{}' provider (needed for RC4 / world client encryption). Runtime library: {}. OpenSSL error: {}",
        LegacyProvider ? "default" : "legacy", OpenSSL_version(OPENSSL_VERSION), std::string(errBuf));

#if AC_PLATFORM == AC_PLATFORM_WINDOWS
    char libcryptoPath[MAX_PATH] = {};
    if (HMODULE libcrypto = GetModuleHandleA("libcrypto-3-x64.dll"))
        GetModuleFileNameA(libcrypto, libcryptoPath, MAX_PATH);

    LOG_ERROR("server", "OpenSSL: libcrypto-3-x64.dll was loaded from '{}'. legacy.dll in the server directory must come from the SAME OpenSSL version - "
        "copy the matching libcrypto-3-x64.dll and libssl-3-x64.dll next to the executable so they win the DLL search order.", std::string(libcryptoPath));
#endif

    return false;
}

void OpenSSLCrypto::threadsCleanup()
{
    OSSL_PROVIDER_unload(LegacyProvider);
    OSSL_PROVIDER_unload(DefaultProvider);
    OSSL_PROVIDER_set_default_search_path(nullptr, nullptr);
}
