#include "ThreadName.h"

#if defined(_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
 #include <pthread.h>
 #include <string.h>
#endif

namespace dusk
{
void setCurrentThreadName (const char* name) noexcept
{
    if (name == nullptr) return;
#if defined(__linux__)
    // pthread_setname_np rejects a longer name outright instead of truncating it.
    char truncated[16] {};
    memcpy (truncated, name, strnlen (name, sizeof truncated - 1));
    pthread_setname_np (pthread_self(), truncated);
#elif defined(__APPLE__)
    pthread_setname_np (name);
#elif defined(_WIN32)
    // SetThreadDescription arrived in Windows 10 1607, so it is looked up rather
    // than linked: an older kernel32 would refuse to load the app at all.
    using SetDescription = HRESULT (WINAPI*) (HANDLE, PCWSTR);
    static const SetDescription setDescription = []
    {
        const HMODULE kernel = GetModuleHandleW (L"kernel32.dll");
        if (kernel == nullptr) return SetDescription {};
        return reinterpret_cast<SetDescription> (
            reinterpret_cast<void*> (GetProcAddress (kernel, "SetThreadDescription")));
    }();
    if (setDescription == nullptr) return;
    wchar_t wide[64] {};
    if (MultiByteToWideChar (CP_UTF8, 0, name, -1, wide, (int) (sizeof wide / sizeof wide[0])) == 0)
        return;
    setDescription (GetCurrentThread(), wide);
#else
    (void) name;
#endif
}
} // namespace dusk
