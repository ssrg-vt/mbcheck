#pragma once
// anchors.h — compile-time anchor tables derived from:
//   docs/kernel-nb-anchor-apis-grouped.csv  (inline asm + ext function rows)
//
// An "anchor" is any instruction or external-function call that carries a
// guaranteed hardware (or compiler) memory ordering on AArch64 Linux kernels.
//
// Two tables are provided:
//   kAsmAnchors  — AArch64 inline-asm mnemonics found in kernel IR
//   kExtAnchors  — external (declared) function name prefixes
//
// Volatile-load / volatile-store constants are provided for READ_ONCE /
// WRITE_ONCE detection; both resolve to compiler-only ordering.

#include <array>
#include <cstdint>
#include <string_view>

namespace anchors {

// ---------------------------------------------------------------------------
// Ordering strength (higher numeric value = stronger guarantee)
// ---------------------------------------------------------------------------
enum class Ordering : uint8_t {
    None         = 0,
    CompilerOnly = 1,   // compiler barrier only, no hardware fence
    Relaxed      = 2,   // no ordering (plain atomic op, relaxed variant)
    Release      = 3,   // store-release or release barrier
    Acquire      = 4,   // load-acquire or acquire barrier
    SC           = 5,   // sequentially consistent (full barrier / RCW)
};

inline constexpr const char *orderingStr(Ordering o) noexcept {
    switch (o) {
    case Ordering::SC:           return "SC";
    case Ordering::Acquire:      return "acquire";
    case Ordering::Release:      return "release";
    case Ordering::Relaxed:      return "relaxed";
    case Ordering::CompilerOnly: return "compiler-only";
    default:                     return "none";
    }
}

// ---------------------------------------------------------------------------
// AnchorKind — structural role of an AArch64 instruction w.r.t. memory
//
//   ThreadBarrier : full-thread barrier (dmb/dsb) — not tied to any specific
//                   memory address; orders all prior/subsequent accesses.
//   Store         : pure store to a memory address (stlr, str, st*).
//   RMW           : read-modify-write atomic (ld*al, cas*, swp*) — always
//                   has a store component even though it also reads.
//   Load          : pure load-acquire (ldar) — reads from memory, no store.
// ---------------------------------------------------------------------------
enum class AnchorKind : uint8_t {
    ThreadBarrier,
    Store,
    RMW,
    Load,
};

// ---------------------------------------------------------------------------
// Inline-asm anchors
//
// mnemonic : normalised lower-case AArch64 mnemonic, one or two words.
//            Two-word entries ("dmb ish") are placed before their one-word
//            prefix ("dmb") so a scan in table order finds the most specific
//            match first (though the pass uses longest-match logic).
// label    : display string (same as mnemonic for asm rows).
// ordering : strongest ordering this instruction guarantees.
// ---------------------------------------------------------------------------
struct AsmAnchor {
    std::string_view mnemonic;
    std::string_view label;
    Ordering         ordering;
    AnchorKind       kind;
};

inline constexpr std::array<AsmAnchor, 56> kAsmAnchors = {{
    // ---- SC (sequentially consistent) ----
    // Two-word barrier mnemonics first
    { "dmb ish",  "dmb ish",  Ordering::SC, AnchorKind::ThreadBarrier },
    { "dmb osh",  "dmb osh",  Ordering::SC, AnchorKind::ThreadBarrier },
    { "dsb sy",   "dsb sy",   Ordering::SC, AnchorKind::ThreadBarrier },
    // DSB variants (audit #1a — kernel-noLTO sweep: 103 × "dsb ish")
    { "dsb ish",  "dsb ish",  Ordering::SC, AnchorKind::ThreadBarrier },
    // Single-word LSE atomics (acquire+release variants)
    { "casal",   "casal",   Ordering::SC, AnchorKind::RMW },
    { "ldaddal", "ldaddal", Ordering::SC, AnchorKind::RMW },
    { "ldclral", "ldclral", Ordering::SC, AnchorKind::RMW },
    { "ldeoral", "ldeoral", Ordering::SC, AnchorKind::RMW },
    { "ldsetal", "ldsetal", Ordering::SC, AnchorKind::RMW },
    { "swpal",   "swpal",   Ordering::SC, AnchorKind::RMW },

    // ---- acquire ----
    { "dmb ishld", "dmb ishld", Ordering::Acquire, AnchorKind::ThreadBarrier },
    { "dmb oshld", "dmb oshld", Ordering::Acquire, AnchorKind::ThreadBarrier },
    // DSB variants (audit #1a — kernel-noLTO sweep: 42 × "dsb ld")
    { "dsb ld",    "dsb ld",    Ordering::Acquire, AnchorKind::ThreadBarrier },
    { "ldadda",    "ldadda",    Ordering::Acquire, AnchorKind::RMW },
    { "ldar",      "ldar",      Ordering::Acquire, AnchorKind::Load },
    // Acquire-exclusive loads (LL/SC acquire patterns, ldaxr=ldar+exclusive)
    { "ldaxr",     "ldaxr",     Ordering::Acquire, AnchorKind::Load },
    { "ldaxrb",    "ldaxrb",    Ordering::Acquire, AnchorKind::Load },
    { "ldaxrh",    "ldaxrh",    Ordering::Acquire, AnchorKind::Load },
    // Byte/halfword acquire loads (smp_load_acquire on u8/u16)
    { "ldarb",     "ldarb",     Ordering::Acquire, AnchorKind::Load },
    { "ldarh",     "ldarh",     Ordering::Acquire, AnchorKind::Load },
    // RCPC acquire loads (ARMv8.3 ldapr — same ordering as ldar for our purposes)
    { "ldapr",     "ldapr",     Ordering::Acquire, AnchorKind::Load },
    { "ldaprb",    "ldaprb",    Ordering::Acquire, AnchorKind::Load },
    { "ldaprh",    "ldaprh",    Ordering::Acquire, AnchorKind::Load },
    { "ldclra",    "ldclra",    Ordering::Acquire, AnchorKind::RMW },
    { "ldeora",    "ldeora",    Ordering::Acquire, AnchorKind::RMW },
    { "ldseta",    "ldseta",    Ordering::Acquire, AnchorKind::RMW },
    { "casa",      "casa",      Ordering::Acquire, AnchorKind::RMW },
    { "cas",       "cas",       Ordering::Acquire, AnchorKind::RMW },

    // ---- release ----
    { "dmb ishst", "dmb ishst", Ordering::Release, AnchorKind::ThreadBarrier },
    { "dmb oshst", "dmb oshst", Ordering::Release, AnchorKind::ThreadBarrier },
    // DSB variants (audit #1a — kernel-noLTO sweep: 413 × "dsb ishst", 194 × "dsb st")
    { "dsb ishst", "dsb ishst", Ordering::Release, AnchorKind::ThreadBarrier },
    { "dsb st",    "dsb st",    Ordering::Release, AnchorKind::ThreadBarrier },
    { "ldaddl",    "ldaddl",    Ordering::Release, AnchorKind::RMW },
    { "ldclrl",    "ldclrl",    Ordering::Release, AnchorKind::RMW },
    { "ldeorl",    "ldeorl",    Ordering::Release, AnchorKind::RMW },
    { "ldsetl",    "ldsetl",    Ordering::Release, AnchorKind::RMW },
    { "casl",      "casl",      Ordering::Release, AnchorKind::RMW },
    { "stlr",      "stlr",      Ordering::Release, AnchorKind::Store },
    // Store-exclusive-release (LL/SC release patterns, e.g. atomic_fetch_sub_release)
    { "stlxr",     "stlxr",     Ordering::Release, AnchorKind::Store },
    { "stlxrb",    "stlxrb",    Ordering::Release, AnchorKind::Store },
    { "stlxrh",    "stlxrh",    Ordering::Release, AnchorKind::Store },
    // Byte/halfword release stores (smp_store_release on u8/u16)
    { "stlrb",     "stlrb",     Ordering::Release, AnchorKind::Store },
    { "stlrh",     "stlrh",     Ordering::Release, AnchorKind::Store },

    // ---- relaxed ----
    { "ldadd", "ldadd", Ordering::Relaxed, AnchorKind::RMW },
    { "ldclr", "ldclr", Ordering::Relaxed, AnchorKind::RMW },
    { "ldeor", "ldeor", Ordering::Relaxed, AnchorKind::RMW },
    { "ldr",   "ldr",   Ordering::Relaxed, AnchorKind::Load },
    { "ldset", "ldset", Ordering::Relaxed, AnchorKind::RMW },
    { "stadd", "stadd", Ordering::Relaxed, AnchorKind::Store },
    { "stclr", "stclr", Ordering::Relaxed, AnchorKind::Store },
    { "steor", "steor", Ordering::Relaxed, AnchorKind::Store },
    { "str",   "str",   Ordering::Relaxed, AnchorKind::Store },
    { "stset", "stset", Ordering::Relaxed, AnchorKind::Store },
    // Exclusive pair (LL/SC for 128-bit) — audit #1a
    { "ldxp",  "ldxp",  Ordering::Relaxed, AnchorKind::Load  },
    { "stxp",  "stxp",  Ordering::Relaxed, AnchorKind::Store },
    { "stlxp", "stlxp", Ordering::Release, AnchorKind::Store },
}};

// ---------------------------------------------------------------------------
// External-function anchors
//
// prefix   : lower-case prefix matched against declared function names.
//            Longest prefix wins (e.g. "_raw_spin_unlock" before
//            "_raw_spin_lock").  The pass uses longest-match scanning.
// label    : display string, mirrors the CSV "(ext)" annotation.
// ordering : ordering guaranteed by calling this function.
// ---------------------------------------------------------------------------
struct ExtAnchor {
    std::string_view prefix;
    std::string_view label;
    Ordering         ordering;
};

inline constexpr std::array<ExtAnchor, 19> kExtAnchors = {{
    // ---- SC ----
    { "synchronize_rcu",  "synchronize_rcu (ext)",  Ordering::SC },
    { "__get_cpu_var",    "__get_cpu_var (ext)",     Ordering::SC },
    { "__put_cpu_var",    "__put_cpu_var (ext)",     Ordering::SC },
    // call_rcu(): callback-defer.  Its *implementation* contains an SC RMW
    // (ldaddal) on the callback list, but for the CALLER's data this provides
    // no cross-thread ordering: the callback runs later in a different thread
    // and any data races involving the caller's data are between the caller
    // and concurrent readers of that data — call_rcu does NOT serialize that.
    // Removed from anchor coverage; see also kExtAnchorBlocklist below for
    // the fallback path through kExtFnSummary.
    { "wake_up",          "wake_up (ext)",           Ordering::SC },
    // G2.4: percpu_ref release (final put) acts as a release barrier on the
    // reference-counted object — readers that obtained a ref via percpu_ref_get
    // observe the writer's prior stores when the last put drains the counter.
    { "percpu_ref_kill",  "percpu_ref_kill (ext)",   Ordering::SC },

    // ---- acquire ----
    // "_raw_spin_unlock" (17 chars) must appear before "_raw_spin_lock" (14)
    // so that longest-match correctly classifies unlock variants.
    { "wait_for_completion", "wait_for_completion (ext)", Ordering::Acquire },
    { "read_seqcount",       "read_seqcount (ext)",       Ordering::Acquire },
    { "_raw_spin_lock",      "_raw_spin_lock (ext)",      Ordering::Acquire },
    // G2.4: bit-spinlock acquire on folio/page lock bit.  Internally uses
    // load-acquire on the lock bit + spin loop.
    { "folio_lock",          "folio_lock (ext)",          Ordering::Acquire },
    { "lock_buffer",         "lock_buffer (ext)",         Ordering::Acquire },
    // G2.4: rcu_seq_start is the writer-side begin marker that publishes
    // "writer in progress" with release semantics.  rcu_seq_end is matching
    // release on completion.  Both make new state visible to seq snapshots.
    { "rcu_seq_start",       "rcu_seq_start (ext)",       Ordering::Acquire },

    // ---- release ----
    { "_raw_spin_unlock", "_raw_spin_unlock (ext)", Ordering::Release },
    { "write_seqcount",   "write_seqcount (ext)",   Ordering::Release },
    { "complete",         "complete (ext)",          Ordering::Release },
    // G2.4: bit-spinlock release; matches folio_lock / lock_buffer.
    { "folio_unlock",     "folio_unlock (ext)",      Ordering::Release },
    { "unlock_buffer",    "unlock_buffer (ext)",     Ordering::Release },
    { "rcu_seq_end",      "rcu_seq_end (ext)",       Ordering::Release },

    // ---- compiler-only ----
    // "__rcu_read_unlock" (17) before "__rcu_read_lock" (15)
    { "__rcu_read_unlock", "__rcu_read_unlock (ext)", Ordering::CompilerOnly },
    { "__rcu_read_lock",   "__rcu_read_lock (ext)",   Ordering::CompilerOnly },
}};

// ---------------------------------------------------------------------------
// Volatile load / store constants  (READ_ONCE, WRITE_ONCE)
//
// Both lower to a compiler-only barrier: the volatile qualifier prevents the
// compiler from eliminating or reordering the access, but emits no hardware
// fence on AArch64.  (rcu_dereference also lowers to load volatile but its
// hardware ordering comes from the surrounding RCU read-side critical section,
// not from the load itself — so compiler-only is the conservative choice.)
// ---------------------------------------------------------------------------
inline constexpr std::string_view kVolatileLoadLabel    = "load volatile ptr";
inline constexpr std::string_view kVolatileStoreLabel   = "store volatile ptr";
inline constexpr Ordering         kVolatileLoadOrdering  = Ordering::CompilerOnly;
inline constexpr Ordering         kVolatileStoreOrdering = Ordering::CompilerOnly;

// ---------------------------------------------------------------------------
// isBlocklistedExtAnchor — should this external function NEVER be treated
// as a memory-ordering anchor?
//
// Background: kExtFnSummary contains 10 280 entries scraped from kernel
// builds.  Each entry records the strongest memory-ordering instruction
// emitted by the function's body.  For utility functions that internally
// touch atomic counters / per-cpu state, this records `ldaddal/SC` even
// though the ordering is private to the function's own bookkeeping and does
// NOT serialize the caller's data with concurrent threads.
//
// Treating such calls as anchors generates structural false positives:
// CausalDetectPass builds an `AnchorBracket` around them; FunctionPairsPass
// pairs every other accessor of the surrounding data with the "anchor"
// function; heuristics later try (and often fail) to suppress the pair.
//
// This helper short-circuits the entire downstream pipeline by refusing to
// emit an AnchorHit for these names.  The blocklist is intentionally
// conservative — only families that are *categorically* non-synchronizing
// for the caller's data are listed:
//
//   • Callback-defer (call_rcu, queue_work, mod_timer, hrtimer_start, ...)
//     — schedules a function to run later; the SC RMW is on the callback
//       list, not the caller's data.
//   • Pure log/warn (printk, dump_stack, __might_sleep) — no shared-data
//     side-effects relevant to the caller.
//   • Pure string/mem builtins (memcpy, memset, str*, snprintf, ...) —
//     compiler builtins; their atomic-looking ordering is incidental.
//   • Time/jiffies helpers (ktime_get*, jiffies_to_*, *_to_jiffies) — read
//     monotonic counters; no ordering of *caller's* shared state.
//
// Functions that are sometimes-benign-sometimes-not (allocators, RNG,
// dev_set_name, etc.) are intentionally NOT blocklisted — they still emit
// anchors and BenignAnchorHeuristic decides per-pair whether to suppress.
// ---------------------------------------------------------------------------
inline bool isBlocklistedExtAnchor(std::string_view name) noexcept {
    auto starts = [&](std::string_view p) {
        return name.size() >= p.size() &&
               name.compare(0, p.size(), p) == 0;
    };
    auto eq = [&](std::string_view p) { return name == p; };

    // ---- callback-defer (callback runs in a different thread later) ----
    if (eq("call_rcu") || eq("call_srcu") ||
        starts("call_rcu_tasks") || eq("queue_rcu_work") ||
        eq("kvfree_call_rcu"))
        return true;
    if (starts("queue_work") || starts("queue_delayed_work") ||
        starts("schedule_work") || starts("schedule_delayed_work") ||
        eq("kblockd_schedule_work") || eq("kthread_queue_work") ||
        eq("mod_delayed_work_on") || eq("mod_delayed_work"))
        return true;
    if (starts("irq_work_queue") || starts("tasklet_schedule") ||
        eq("tasklet_hi_schedule"))
        return true;
    if (starts("add_timer") || starts("mod_timer") ||
        starts("hrtimer_start"))
        return true;

    // ---- pure log / warn / debug ----
    if (eq("_printk") || eq("__warn_printk") || eq("printk_deferred") ||
        eq("dump_stack") || eq("dump_stack_lvl") ||
        eq("__might_sleep") || eq("___might_sleep") ||
        eq("might_resched"))
        return true;

    // ---- pure string / mem / format builtins ----
    if (starts("memcpy") || starts("memset") || starts("memmove") ||
        starts("memcmp") || starts("memchr") ||
        starts("__memcpy") || starts("__memset") || starts("__memmove"))
        return true;
    if (starts("strcpy") || starts("strncpy") || starts("strscpy") ||
        starts("strlcpy") || starts("strlcat") ||
        eq("strlen") || eq("strnlen") || eq("strcmp") || eq("strncmp") ||
        eq("strchr") || eq("strrchr") || eq("strstr") || eq("strcasecmp") ||
        eq("strncasecmp") || starts("sized_strscpy"))
        return true;
    if (starts("snprintf") || starts("scnprintf") || starts("sprintf") ||
        starts("vsnprintf") || starts("vscnprintf") || starts("vsprintf"))
        return true;

    // ---- time / jiffies / ktime (read monotonic counters) ----
    if (starts("jiffies_to_") ||
        eq("usecs_to_jiffies") || eq("msecs_to_jiffies") ||
        eq("__msecs_to_jiffies") || eq("__usecs_to_jiffies") ||
        eq("nsecs_to_jiffies") || eq("nsecs_to_jiffies64") ||
        eq("get_jiffies_64"))
        return true;

    // ---- refcount / lockref bookkeeping (internal counters) ----
    // lockref_get / lockref_put inc/dec the embedded refcount via casal —
    // pure refcount bookkeeping, doesn't synchronize caller's payload data.
    if (eq("lockref_get") || eq("lockref_put") ||
        eq("lockref_get_not_zero") || eq("lockref_put_or_lock") ||
        eq("lockref_get_not_dead") || eq("lockref_put_return"))
        return true;

    // ---- rbtree internal rotations ----
    // __rb_insert_augmented / __rb_erase_augmented perform pointer fix-ups
    // under the caller's lock; the internal cas is colour-bit bookkeeping.
    if (eq("__rb_insert_augmented") || eq("__rb_erase_color") ||
        eq("__rb_insert_color") || eq("rb_insert_color") ||
        eq("rb_erase"))
        return true;
    if (starts("ktime_get") || starts("ktime_to_") ||
        eq("ktime_get_real_ts64") || eq("ktime_get_coarse"))
        return true;

    // ---- CPU feature register readers (read static feature bitmap) ----
    // read_sanitised_ftr_reg() reads arm64_ftr_regs[].sys_val — a static
    // array.  Its internal RMW (atomic update on first call's cache fill)
    // is not a synchronization primitive for the caller's data.  Surfaces
    // as 16 FP "No" verdicts on KVM trap-handler pairs.
    if (eq("read_sanitised_ftr_reg") || eq("read_cpuid_effective_cachetype") ||
        starts("cpus_have_") || starts("cpu_have_") ||
        eq("system_supports_") || starts("__read_sysreg_by_encoding"))
        return true;

    // ---- T2.4: subsystem init / registration helpers ----
    // These run during driver probe under device_lock / subsys-init mutex;
    // their internal atomics are bookkeeping (refcount, idr alloc, list
    // splice) not synchronization of the caller's payload.
    if (eq("timer_init_key") || eq("hrtimer_init") ||
        eq("init_timer_key") || eq("__init_work") ||
        eq("mutex_init_generic") || eq("__mutex_init") ||
        eq("__init_rwsem") || eq("__init_swait_queue_head") ||
        eq("__init_waitqueue_head") || eq("init_completion") ||
        eq("tty_port_init") || eq("device_initialize") ||
        eq("device_register") || eq("device_add") ||
        eq("platform_device_register") ||
        eq("platform_device_register_full") ||
        eq("devm_of_platform_populate") || eq("devm_of_phy_get") ||
        eq("dev_err_probe") || eq("_dev_err") || eq("_dev_warn") ||
        eq("_dev_info") || eq("_dev_notice") ||
        eq("set_normalized_timespec64") || eq("uart_get_rs485_mode"))
        return true;

    // ---- T2.4 follow-on: subsystem registration / publish helpers ----
    // Identified by no_cases_analysis_v6.md as remaining FP sources.
    // Each maintains its own internal locking (subsystem-global mutex)
    // for the published list; not a synchronization primitive on caller
    // data.
    if (eq("serial8250_register_8250_port") ||
        eq("serial8250_unregister_port") ||
        eq("register_console") || eq("unregister_console") ||
        eq("__pr_flush") ||
        eq("irq_domain_instantiate") ||
        starts("irq_domain_create_") || starts("irq_domain_add_") ||
        eq("power_supply_register") || eq("power_supply_unregister") ||
        eq("led_trigger_register") || eq("led_trigger_unregister") ||
        eq("led_classdev_register") || eq("led_classdev_unregister"))
        return true;

    return false;
}

} // namespace anchors
