// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

/// The system calls of a process that decodes untrusted input: aglow's
/// decoder child and the decoding processes of aglow-mux.
namespace AfterglowProto {

// The architectures whose system calls the filter knows, little-endian all:
// it reads a descriptor from the low word of an argument. The macro is this
// header's own: it ends at the end of the header. A preprocessor test, as an
// unknown architecture may lack the calls that the filter names (SYS_mmap).
#if defined(__x86_64__)
#define AFTERGLOW_DECODE_FILTER_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define AFTERGLOW_DECODE_FILTER_ARCH AUDIT_ARCH_AARCH64
#elif defined(__riscv) && __riscv_xlen == 64
#define AFTERGLOW_DECODE_FILTER_ARCH AUDIT_ARCH_RISCV64
#elif defined(__powerpc64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define AFTERGLOW_DECODE_FILTER_ARCH AUDIT_ARCH_PPC64LE
#endif

/// Whether allowOnlyDecoding() knows the system calls of this processor.
#ifdef AFTERGLOW_DECODE_FILTER_ARCH
inline constexpr bool DecodeFilterKnown = true;
#else
inline constexpr bool DecodeFilterKnown = false;
#endif

/// Past this, the process makes only the system calls that decoding needs:
/// memory (mmap, munmap, mremap, mprotect, madvise, brk), a read of standard
/// input, a write of standard output or of a sanitizer's report on standard
/// error, the lowering of its own limits (prlimit64, getrlimit, setrlimit),
/// close, futex, getrandom, clock_gettime, rt_sigreturn, its end, and
/// sigaltstack(), which AddressSanitizer calls before each noreturn call
/// (_exit, longjmp, throw) and which changes only the caller's own signal
/// stack. Any other fails with ENOSYS, so that code that a corrupt file took
/// over can open, run or connect to nothing, and read no terminal. A system
/// call of another architecture kills it. False when the filter cannot be
/// set, and on an architecture that it does not know (DecodeFilterKnown).
/// The caller makes standard error /dev/null, but in a sanitizer build, and
/// closes every other descriptor first.
inline bool allowOnlyDecoding()
{
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return false;
#ifndef AFTERGLOW_DECODE_FILTER_ARCH
    return false;
#else
    constexpr std::uint32_t NativeArch = AFTERGLOW_DECODE_FILTER_ARCH;
    constexpr std::uint32_t Allow = SECCOMP_RET_ALLOW;
    constexpr std::uint32_t Refuse = SECCOMP_RET_ERRNO | (ENOSYS & SECCOMP_RET_DATA);
    // The low word of the first argument: a descriptor or a process id
    // (little endian, as the architectures here are).
    constexpr std::uint32_t FirstArgument = offsetof(seccomp_data, args);
    std::vector<sock_filter> filter;
    filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)));
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NativeArch, 1, 0));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)));
    // read() of standard input only.
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_read, 0, 4));
    filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, FirstArgument));
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, STDIN_FILENO, 0, 1));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, Allow));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, Refuse));
    // write() of standard output and standard error only.
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_write, 0, 5));
    filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, FirstArgument));
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, STDOUT_FILENO, 1, 0));
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, STDERR_FILENO, 0, 1));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, Allow));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, Refuse));
    // prlimit64() of this process only: the limits go down once the
    // image's header is read, and no hard limit can go up.
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_prlimit64, 0, 4));
    filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, FirstArgument));
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, Allow));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, Refuse));
#ifdef SYS_getrlimit
    for (const long call : {long(SYS_getrlimit), long(SYS_setrlimit)}) {
        filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, std::uint32_t(call), 0, 1));
        filter.push_back(BPF_STMT(BPF_RET | BPF_K, Allow));
    }
#endif
    for (const long call :
         {SYS_close, SYS_mmap, SYS_munmap, SYS_mremap, SYS_mprotect, SYS_madvise, SYS_brk, SYS_futex, SYS_getrandom,
          SYS_clock_gettime, SYS_rt_sigreturn, SYS_sigaltstack, SYS_exit, SYS_exit_group}) {
        filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, std::uint32_t(call), 0, 1));
        filter.push_back(BPF_STMT(BPF_RET | BPF_K, Allow));
    }
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, Refuse));
    const sock_fprog program{static_cast<unsigned short>(filter.size()), filter.data()};
    return ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
#endif
}

} // namespace AfterglowProto

#undef AFTERGLOW_DECODE_FILTER_ARCH
