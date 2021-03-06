/*
 * Copyright (c) 2010, Stefan Lankes, RWTH Aachen University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the name of the University nor the names of its contributors
 *      may be used to endorse or promote products derived from this
 *      software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <eduos/stdio.h>
#include <eduos/string.h>
#include <eduos/stdlib.h>
#include <eduos/tasks.h>
#include <eduos/errno.h>
#include <eduos/processor.h>
#include <eduos/time.h>
#include <asm/gdt.h>
#include <asm/tss.h>
#include <asm/page.h>

gdt_ptr_t				gp;
static tss_t			task_state_segment __attribute__ ((aligned (PAGE_SIZE)));
// currently, our kernel has full access to the ioports
static gdt_entry_t		gdt[GDT_ENTRIES] = {[0 ... GDT_ENTRIES-1] = {0, 0, 0, 0, 0, 0}};

/* 
 * This is defined in entry.asm. We use this to properly reload
 * the new segment registers
 */
extern void gdt_flush(void);

extern const void boot_stack;

void set_kernel_stack(void)
{
	task_t* curr_task = current_task;

#ifdef CONFIG_X86_32
	task_state_segment.esp0 = (size_t) curr_task->stack + KERNEL_STACK_SIZE - 16; // => stack is 16byte aligned
#else
	task_state_segment.rsp0 = (size_t) curr_task->stack + KERNEL_STACK_SIZE - 16; // => stack is 16byte aligned
#endif
}

size_t get_kernel_stack(void)
{
	task_t* curr_task = current_task;

	return (size_t) curr_task->stack + KERNEL_STACK_SIZE - 16;
}

/* Setup a descriptor in the Global Descriptor Table */
void gdt_set_gate(int num, unsigned long base, unsigned long limit,
			  unsigned char access, unsigned char gran)
{
	configure_gdt_entry(&gdt[num], base, limit, access, gran);
}

void configure_gdt_entry(gdt_entry_t *dest_entry, unsigned long base, unsigned long limit,
		unsigned char access, unsigned char gran)
{
	/* Setup the descriptor base address */
	dest_entry->base_low = (base & 0xFFFF);
	dest_entry->base_middle = (base >> 16) & 0xFF;
	dest_entry->base_high = (base >> 24) & 0xFF;

	/* Setup the descriptor limits */
	dest_entry->limit_low = (limit & 0xFFFF);
	dest_entry->granularity = ((limit >> 16) & 0x0F);

	/* Finally, set up the granularity and access flags */
	dest_entry->granularity |= (gran & 0xF0);
	dest_entry->access = access;
}

/* 
 * This will setup the special GDT
 * pointer, set up the entries in our GDT, and then
 * finally call gdt_flush() in our assembler file in order
 * to tell the processor where the new GDT is and update the
 * new segment registers 
 */
void gdt_install(void)
{
	unsigned long gran_ds, gran_cs, limit;
	int num = 0;

	memset(&task_state_segment, 0x00, sizeof(tss_t));

#ifdef CONFIG_X86_32
	gran_cs = gran_ds = GDT_FLAG_32_BIT | GDT_FLAG_4K_GRAN;
	limit = 0xFFFFFFFF;
#elif defined(CONFIG_X86_64)
	gran_cs = GDT_FLAG_64_BIT;
	gran_ds = 0;
	limit = 0;
#else
#error invalid mode
#endif

	/* Setup the GDT pointer and limit */
	gp.limit = (sizeof(gdt_entry_t) * GDT_ENTRIES) - 1;
	gp.base = (size_t) &gdt;

	/* Our NULL descriptor */
	gdt_set_gate(num++, 0, 0, 0, 0);

	/* 
	 * The second entry is our Code Segment. The base address
	 * is 0, the limit is 4 GByte, it uses 4KByte granularity,
	 * and is a Code Segment descriptor.
	 */
	gdt_set_gate(num++, 0, limit,
		GDT_FLAG_RING0 | GDT_FLAG_SEGMENT | GDT_FLAG_CODESEG | GDT_FLAG_PRESENT, gran_cs);

	/* 
	 * The third entry is our Data Segment. It's EXACTLY the
	 * same as our code segment, but the descriptor type in
	 * this entry's access byte says it's a Data Segment 
	 */
	gdt_set_gate(num++, 0, limit,
		GDT_FLAG_RING0 | GDT_FLAG_SEGMENT | GDT_FLAG_DATASEG | GDT_FLAG_PRESENT, gran_ds);

	/*
	 * Create code segment for 32bit user-space applications (ring 3)
	 */
	gdt_set_gate(num++, 0, 0xFFFFFFFF,
		GDT_FLAG_RING3 | GDT_FLAG_SEGMENT | GDT_FLAG_CODESEG | GDT_FLAG_PRESENT, GDT_FLAG_32_BIT | GDT_FLAG_4K_GRAN);

	/*
	 * Create data segment for user-space applications (ring 3)
	 */
	gdt_set_gate(num++, 0, limit,
		GDT_FLAG_RING3 | GDT_FLAG_SEGMENT | GDT_FLAG_DATASEG | GDT_FLAG_PRESENT, gran_ds);

#ifdef CONFIG_X86_64
	/*
	 * Create code segment for 64bit user-space applications (ring 3)
	 */
	gdt_set_gate(num++, 0, limit,
		GDT_FLAG_RING3 | GDT_FLAG_SEGMENT | GDT_FLAG_CODESEG | GDT_FLAG_PRESENT, gran_cs);

	task_state_segment.rsp0 = (size_t) &boot_stack - 0x10;
	gdt_set_gate(num++, (unsigned long) (&task_state_segment), sizeof(tss_t)-1,
			GDT_FLAG_PRESENT | GDT_FLAG_TSS | GDT_FLAG_RING0, gran_ds);
#elif defined(CONFIG_X86_32)
	/* set default values */
	task_state_segment.eflags = 0x1202;
	task_state_segment.ss0 = 0x10;			// data segment
	task_state_segment.esp0 = (size_t) &boot_stack - 0x10;
	task_state_segment.cs = 0x0b;
	task_state_segment.ss = task_state_segment.ds = task_state_segment.es = task_state_segment.fs = task_state_segment.gs = 0x13;
	gdt_set_gate(num++, (unsigned long) (&task_state_segment), sizeof(tss_t)-1,
			GDT_FLAG_PRESENT | GDT_FLAG_TSS | GDT_FLAG_RING0, gran_ds);
#endif

	/* Flush out the old GDT and install the new changes! */
	gdt_flush();
}
