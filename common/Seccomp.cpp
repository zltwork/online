/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
/*
 * Code to lock-down the environment of the processes we run, to avoid
 * exotic or un-necessary system calls to be used to break containment.
 */

#include <config.h>
#include <dlfcn.h>
#include <ftw.h>
#include <linux/audit.h>
#include <linux/filter.h>
#if DISABLE_SECCOMP == 0
#include <linux/seccomp.h>
#endif
#include <malloc.h>
#include <signal.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#include <utime.h>

#include <common/Log.hpp>
#include <common/SigUtil.hpp>
#include "Seccomp.hpp"

#if DISABLE_SECCOMP == 0
#ifndef SYS_SECCOMP
#  define SYS_SECCOMP 1
#endif

#if defined(__x86_64__)
#  define AUDIT_ARCH_NR AUDIT_ARCH_X86_64
#  define REG_SYSCALL   REG_RAX
#else
#  error "Platform does not support seccomp filtering yet - unsafe."
#endif

extern "C" {

static void handleSysSignal(int /* signal */,
                            siginfo_t *info,
                            void *context)
{
	ucontext_t *uctx = static_cast<ucontext_t *>(context);

    Log::signalLogPrefix();
    Log::signalLog("SIGSYS trapped with code: ");
    Log::signalLogNumber(info->si_code);
    Log::signalLog(" and context ");
    Log::signalLogNumber(reinterpret_cast<size_t>(context));
    Log::signalLog("\n");

	if (info->si_code != SYS_SECCOMP || !uctx)
		return;

	unsigned int syscall = uctx->uc_mcontext.gregs[REG_SYSCALL];

    Log::signalLogPrefix();
    Log::signalLog(" seccomp trapped signal, un-authorized sys-call: ");
    Log::signalLogNumber(syscall);
    Log::signalLog("\n");

    SigUtil::dumpBacktrace();

    _exit(1);
}

} // extern "C"
#endif

namespace Seccomp {

bool lockdown(Type type)
{
    (void)type; // so far just the kit.

#if DISABLE_SECCOMP == 0
    #define ACCEPT_SYSCALL(name) \
        BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, __NR_##name, 0, 1), \
        BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW)

    #define KILL_SYSCALL(name) \
        BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, __NR_##name, 0, 1), \
        BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_TRAP)

    struct sock_filter filterCode[] = {
        // Check our architecture is correct.
        BPF_STMT(BPF_LD+BPF_W+BPF_ABS,  offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, AUDIT_ARCH_NR, 1, 0),
        BPF_STMT(BPF_RET+BPF_K,         SECCOMP_RET_KILL),

        // Load sycall number
        BPF_STMT(BPF_LD+BPF_W+BPF_ABS,  offsetof(struct seccomp_data, nr)),

        // ------------------------------------------------------------
        // ---   First white-list the syscalls we frequently use.   ---
        // ------------------------------------------------------------
        ACCEPT_SYSCALL(recvfrom),
        ACCEPT_SYSCALL(write),
        ACCEPT_SYSCALL(futex),

        // glibc's 'poll' has to answer for this lot:
        ACCEPT_SYSCALL(epoll_wait),
        ACCEPT_SYSCALL(epoll_ctl),
        ACCEPT_SYSCALL(epoll_create),
        ACCEPT_SYSCALL(close),
        ACCEPT_SYSCALL(nanosleep),

        // ------------------------------------------------------------
        // --- Now block everything that we don't like the look of. ---
        // ------------------------------------------------------------

        // FIXME: should we bother blocking calls that have early
        // permission checks we don't meet ?

#if 0
        // cf. eg. /usr/include/asm/unistd_64.h ...
        KILL_SYSCALL(ioctl),
        KILL_SYSCALL(mincore),
        KILL_SYSCALL(shmget),
        KILL_SYSCALL(shmat),
        KILL_SYSCALL(shmctl),
#endif
        KILL_SYSCALL(getitimer),
        KILL_SYSCALL(setitimer),
        KILL_SYSCALL(sendfile),
        KILL_SYSCALL(shutdown),
        KILL_SYSCALL(listen),  // server sockets
        KILL_SYSCALL(accept),  // server sockets
#if 0
        KILL_SYSCALL(wait4),
#endif
        KILL_SYSCALL(kill),   // !
        KILL_SYSCALL(shmctl),
        KILL_SYSCALL(ptrace), // tracing
        KILL_SYSCALL(capset),
        KILL_SYSCALL(uselib),
        KILL_SYSCALL(personality), // !
        KILL_SYSCALL(vhangup),
        KILL_SYSCALL(modify_ldt), // !
        KILL_SYSCALL(pivot_root), // !
        KILL_SYSCALL(chroot),
        KILL_SYSCALL(acct),   // !
        KILL_SYSCALL(sync),   // I/O perf.
        KILL_SYSCALL(mount),
        KILL_SYSCALL(umount2),
        KILL_SYSCALL(swapon),
        KILL_SYSCALL(swapoff),
        KILL_SYSCALL(reboot), // !
        KILL_SYSCALL(sethostname),
        KILL_SYSCALL(setdomainname),
        KILL_SYSCALL(tkill),
        KILL_SYSCALL(mbind), // vm bits
        KILL_SYSCALL(set_mempolicy), // vm bits
        KILL_SYSCALL(get_mempolicy), // vm bits
        KILL_SYSCALL(kexec_load),
        KILL_SYSCALL(add_key),     // kernel keyring
        KILL_SYSCALL(request_key), // kernel keyring
        KILL_SYSCALL(keyctl),      // kernel keyring
        KILL_SYSCALL(inotify_init),
        KILL_SYSCALL(inotify_add_watch),
        KILL_SYSCALL(inotify_rm_watch),
        KILL_SYSCALL(unshare),
        KILL_SYSCALL(splice),
        KILL_SYSCALL(tee),
        KILL_SYSCALL(vmsplice), // vm bits
        KILL_SYSCALL(move_pages), // vm bits
        KILL_SYSCALL(accept4), // server sockets
        KILL_SYSCALL(inotify_init1),
        KILL_SYSCALL(perf_event_open), // profiling
        KILL_SYSCALL(fanotify_init),
        KILL_SYSCALL(fanotify_mark),
#ifdef __NR_seccomp
        KILL_SYSCALL(seccomp), // no further fiddling
#endif
#ifdef __NR_bpf
        KILL_SYSCALL(bpf),     // no further fiddling
#endif

        // allow the rest.
        BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW)
    };

    struct sock_fprog filter = {
        sizeof(filterCode)/sizeof(filterCode[0]), // length
        filterCode
    };

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
    {
        LOG_ERR("Cannot turn off acquisition of new privileges for us & children");
        return false;
    }
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &filter))
    {
        LOG_ERR("Failed to install seccomp syscall filter");
        return false;
    }

    // Trap, log, and exit on failure
    struct sigaction action;

    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    action.sa_handler = reinterpret_cast<__sighandler_t>(handleSysSignal);

    sigaction(SIGSYS, &action, nullptr);

    LOG_TRC("Install seccomp filter successfully.");

    return true;
#else // DISABLE_SECCOMP == 0
     LOG_WRN("Warning: this binary was compiled with disabled seccomp-bpf.");
     return true;
#endif // DISABLE_SECCOMP == 0
}

} // namespace Seccomp

namespace Rlimit {

void setRLimit(rlim_t confLim, int resource, const std::string &resourceText, const std::string &unitText)
{
    rlim_t lim = confLim;
    if (lim <= 0)
        lim = RLIM_INFINITY;
    const std::string limTextWithUnit((lim == RLIM_INFINITY) ? "unlimited" : std::to_string(lim) + " " + unitText);
    if (resource != RLIMIT_FSIZE && resource != RLIMIT_NOFILE)
    {
        /* FIXME Currently the RLIMIT_FSIZE handling is non-ideal, and can
         * lead to crashes of the kit processes due to not handling signal
         * 25 gracefully.  Let's disable for now before there's a more
         * concrete plan.
         * Similar issues with RLIMIT_NOFILE
         */
        rlimit rlim = { lim, lim };
        if (setrlimit(resource, &rlim) != 0)
            LOG_SYS("Failed to set " << resourceText << " to " << limTextWithUnit << ".");
        if (getrlimit(resource, &rlim) == 0)
        {
            const std::string setLimTextWithUnit((rlim.rlim_max == RLIM_INFINITY) ? "unlimited" : std::to_string(rlim.rlim_max) + " " + unitText);
            LOG_INF(resourceText << " is " << setLimTextWithUnit << " after setting it to " << limTextWithUnit << ".");
        }
        else
            LOG_SYS("Failed to get " << resourceText << ".");
    }
    else
        LOG_INF("Ignored setting " << resourceText << " to " << limTextWithUnit << ".");
}

bool handleSetrlimitCommand(const std::vector<std::string>& tokens)
{
    if (tokens.size() == 3 && tokens[0] == "setconfig")
    {
        if (tokens[1] == "limit_virt_mem_mb")
        {
            setRLimit(std::stoi(tokens[2]) * 1024 * 1024, RLIMIT_AS, "RLIMIT_AS", "bytes");
        }
        else if (tokens[1] == "limit_stack_mem_kb")
        {
            setRLimit(std::stoi(tokens[2]) * 1024, RLIMIT_STACK, "RLIMIT_STACK", "bytes");
        }
        else if (tokens[1] == "limit_file_size_mb")
        {
            setRLimit(std::stoi(tokens[2]) * 1024 * 1024, RLIMIT_FSIZE, "RLIMIT_FSIZE", "bytes");
        }
        else if (tokens[1] == "limit_num_open_files")
        {
            setRLimit(std::stoi(tokens[2]), RLIMIT_NOFILE, "RLIMIT_NOFILE", "files");
        }
        else
            return false;

        return true;
    }
    return false;
}

} // namespace Rlimit

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
