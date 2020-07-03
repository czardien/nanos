/*-
 * Copyright (c) 2016 Microsoft Corp.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#ifndef _HYPERV_VAR_H_
#define _HYPERV_VAR_H_

static inline u64 rdmsr(u32 idx)
{
    u32 lo, hi, index = idx;
    asm volatile("rdmsr" : "=a" (lo), "=d" (hi) : "c" (index));
    return lo | ((u64)hi << 32);
}

static inline void wrmsr(u32 msr, u64 newval)
{
    u32 low = newval;
    u32 high = newval >> 32;
    __asm __volatile("wrmsr" : : "a" (low), "d" (high), "c" (msr));
}

uint64_t	hypercall_post_message(bus_addr_t msg_paddr);
uint64_t	hypercall_signal_event(bus_addr_t monprm_paddr);

#define VMBUS_DRIVER_NAME_MAX 16
typedef struct vmbus_driver {
    struct list l;
    const struct hyperv_guid *type;
    vmbus_device_probe probe;
} *vmbus_driver;

#endif	/* !_HYPERV_VAR_H_ */
