#ifndef ASTRA_EXPORT_HPP
#define ASTRA_EXPORT_HPP

// AstraScheduler public symbol 可见性控制（R-110）。
// Supported Configuration 仅 64-bit Linux：Tier-1 GCC 13+ / Clang 17+（R-111）。

#if !defined(__linux__) || (__SIZEOF_POINTER__ != 8)
#error "AstraScheduler 仅支持 64-bit Linux（R-111 / D-167）。"
#endif

#if defined(__GNUC__) || defined(__clang__)
#define ASTRA_EXPORT __attribute__((visibility("default")))
#define ASTRA_NO_EXPORT __attribute__((visibility("hidden")))
#else
#error "AstraScheduler 仅支持 Linux GCC/Clang 工具链（R-111 / D-167）。"
#endif

// R-110 / D-167：库要求 exception-enabled 编译，不支持 -fno-exceptions。
// core 不要求 RTTI（不限制 -fno-rtti 的使用）。
#if !defined(__EXCEPTIONS)
#error "AstraScheduler 不支持 -fno-exceptions 编译（R-110 / D-167）。"
#endif

#endif  // ASTRA_EXPORT_HPP
