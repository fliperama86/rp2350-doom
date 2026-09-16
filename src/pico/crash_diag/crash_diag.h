#pragma once

// Also consumed by the stackless assembler handlers.
#define CRASH_FAULT 1
#define CRASH_ENGINE 2
#define CRASH_OOM 3
#define CRASH_PANIC 4
#define CRASH_ASSERT 5
#define CRASH_HEAP 6

#define CR_KIND 0
#define CR_PC 4
#define CR_LR 8
#define CR_SP 12
#define CR_CFSR 16
#define CR_HFSR 20
#define CR_MMFAR 24
#define CR_BFAR 28
#define CR_EXC_RETURN 32
#define CR_XPSR 36
#define CR_DETAIL 40
#define CR_EXTRA 44
#define CR_IPSR 48
#define CR_FRAME_VALID 52
#define CR_SIZE 64

#ifndef __ASSEMBLER__
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t kind; // Published LAST, after a memory barrier; 0 = no record.
    uint32_t pc, lr, sp, cfsr, hfsr, mmfar, bfar;
    uint32_t exc_return, xpsr, detail, extra, ipsr, frame_valid;
    uint32_t reserved[2];
} picodoom_crash_record_t;

extern volatile picodoom_crash_record_t picodoom_crash[2];
// PC is the caller's return address for software stops, stacked PC for faults.
void picodoom_crash_stop(uint32_t kind, uint32_t detail, uint32_t extra)
    __attribute__((noreturn));

#ifdef __cplusplus
}
#else
#define CHECK_CR_FIELD(field, offset) \
    _Static_assert(offsetof(picodoom_crash_record_t, field) == offset, "crash ABI: " #field)
CHECK_CR_FIELD(kind, CR_KIND);
CHECK_CR_FIELD(pc, CR_PC);
CHECK_CR_FIELD(lr, CR_LR);
CHECK_CR_FIELD(sp, CR_SP);
CHECK_CR_FIELD(cfsr, CR_CFSR);
CHECK_CR_FIELD(hfsr, CR_HFSR);
CHECK_CR_FIELD(mmfar, CR_MMFAR);
CHECK_CR_FIELD(bfar, CR_BFAR);
CHECK_CR_FIELD(exc_return, CR_EXC_RETURN);
CHECK_CR_FIELD(xpsr, CR_XPSR);
CHECK_CR_FIELD(detail, CR_DETAIL);
CHECK_CR_FIELD(extra, CR_EXTRA);
CHECK_CR_FIELD(ipsr, CR_IPSR);
CHECK_CR_FIELD(frame_valid, CR_FRAME_VALID);
_Static_assert(sizeof(picodoom_crash_record_t) == CR_SIZE, "crash record stride");
#undef CHECK_CR_FIELD
#endif
#endif
