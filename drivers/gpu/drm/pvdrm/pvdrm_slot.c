/*
  Copyright (C) 2014 Yusuke Suzuki <utatane.tea@gmail.com>

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
  THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <linux/atomic.h>
#include <linux/types.h>
#include <linux/semaphore.h>
#include <linux/string.h>

#include <xen/xen.h>
#include <xen/xenbus.h>
#include <xen/grant_table.h>
#include <xen/events.h>
#include <xen/page.h>
#include <xen/platform_pci.h>

#include "pvdrm_drm.h"
#include "pvdrm_slot.h"

static struct pvdrm_mapped* extract_mapped(struct pvdrm_slots* slots)
{
        return slots->mapped;
}

static bool is_used(struct pvdrm_slot* slot)
{
        return slot->code != PVDRM_UNUSED;
}

int pvdrm_slot_init(struct pvdrm_device* pvdrm)
{
	int i;
	int ret;
	struct pvdrm_slots* slots;
	struct xenbus_device* xbdev;
        struct pvdrm_mapped* mapped;
        spinlock_t* lock;
	struct semaphore* sema;

        BUILD_BUG_ON(sizeof(struct pvdrm_mapped) <= PAGE_SIZE);

        printk(KERN_INFO "PVDRM: Initializing pvdrm slots.\n");
	ret = 0;

	slots = kzalloc(sizeof(struct pvdrm_slots), GFP_KERNEL);
	if (!slots) {
		return -ENOMEM;
	}
	pvdrm->slots = slots;

	xbdev = pvdrm->dev->xbdev;

        sema = &slots->sema;
	sema_init(sema, PVDRM_SLOT_NR);

        lock = &slots->lock;
        spin_lock_init(lock);

	/* Allocate slot and counter ref. */
	{
		const uintptr_t vaddr = get_zeroed_page(GFP_NOIO | __GFP_HIGH);
		if (!vaddr) {
			ret = -ENOMEM;
			xenbus_dev_fatal(xbdev, ret, "allocating ring page");
			return ret;
		}

		ret = xenbus_grant_ring(xbdev, virt_to_mfn(vaddr));
		if (ret < 0) {
			xenbus_dev_fatal(xbdev, ret, "granting ring page");
			free_page(vaddr);
			return ret;
		}

		slots->ref = ret;
		slots->mapped = (void*)vaddr;
	}
	printk(KERN_INFO "PVDRM: Initialising pvdrm counter reference %u.\n", slots->ref);

        mapped = extract_mapped(slots);

	/* Init counter. */
        atomic_set(&mapped->count, 0);

        printk(KERN_INFO "PVDRM: Initialized pvdrm counter.\n");

	/* Init slots. */
	for (i = 0; i < PVDRM_SLOT_NR; ++i) {
                struct pvdrm_slot* slot = &mapped->slot[i];
                slot->__id = i;
                slot->code = PVDRM_UNUSED;
                mapped->ring[i] = (uint32_t)-1;
	}

        printk(KERN_INFO "PVDRM: Initialized pvdrm slots.\n");

	return 0;
}

struct pvdrm_slot* pvdrm_slot_alloc(struct pvdrm_device* pvdrm)
{
	int i;
	struct pvdrm_slots* slots;
	struct pvdrm_slot* slot;
	struct pvdrm_mapped* mapped;
	unsigned long flags;

	slots = pvdrm->slots;
        mapped = extract_mapped(slots);

	down(&slots->sema);
	spin_lock_irqsave(&slots->lock, flags);

	for (i = 0; i < PVDRM_SLOT_NR; ++i) {
		if (!is_used(&mapped->slot[i])) {
                        slot = &mapped->slot[i];
                        slot->code = PVDRM_HELD;
			break;
		}
	}

	BUG_ON(i == PVDRM_SLOT_NR);

	spin_unlock_irqrestore(&slots->lock, flags);

	/* Init slot. */
	pvdrm_fence_init(&slot->__fence);
	slot->ret = 0;

	return slot;
}

void pvdrm_slot_free(struct pvdrm_device* pvdrm, struct pvdrm_slot* slot)
{
	struct pvdrm_slots* slots;
	unsigned long flags;
	struct pvdrm_mapped* mapped;

	slots = pvdrm->slots;
        mapped = extract_mapped(slots);

	spin_lock_irqsave(&slots->lock, flags);

	BUG_ON(!is_used(slot));
        slot->code = PVDRM_UNUSED;

	spin_unlock_irqrestore(&slots->lock, flags);
	up(&slots->sema);
}

int pvdrm_slot_request(struct pvdrm_device* pvdrm, struct pvdrm_slot* slot)
{
	/* TODO: Implement it, emitting fence here. */
	struct pvdrm_slots* slots;
	int ret;
	struct pvdrm_mapped* mapped;

	slots = pvdrm->slots;
        mapped = extract_mapped(slots);

	BUG_ON(!is_used(slot));

	/* Request slot, increment counter. */
	wmb();
	atomic_inc(&mapped->count);

	/* Wait. */
	ret = pvdrm_fence_wait(&slot->__fence, false);
	if (ret) {
		return ret;
	}
	return slot->ret;
}

/* vim: set sw=8 ts=8 et tw=80 : */
