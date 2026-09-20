#pragma once

// Keep DLL visibility independent of Unreal platform headers.
#if defined(SKI_DOMAIN_SHARED) && defined(_WIN32)
#if defined(SKI_DOMAIN_EXPORTS)
#define SKI_DOMAIN_API __declspec(dllexport)
#else
#define SKI_DOMAIN_API __declspec(dllimport)
#endif
#else
#define SKI_DOMAIN_API
#endif
