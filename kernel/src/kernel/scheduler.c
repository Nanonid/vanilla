/// Copyright (C) StrawberryHacker

#include "scheduler.h"
#include "nvic.h"
#include "systick.h"
#include "cpu.h"
#include "panic.h"
#include "print.h"

#include <stddef.h>

volatile struct thread* curr_thread;
volatile struct thread* next_thread;

struct thread* idle;

/// Main CPU runqueue structure
struct rq cpu_rq = {0};

/// Knernel tick will increment each time the SysTick handler runs
volatile u64 tick = 0;
volatile u32 reschedule_pending = 0;

/// The function which is starting the scheduler is defined in contex.s. It 
/// sets up the stack for the first thread to run, switches from MSP to PSP, and
/// places the thread function in the LR. This will start executing the first
/// thread
void scheduler_run(void);

/// The scheduler has to add the idle thread
extern struct thread* new_thread(struct thread_info* thread_info);

/// The idle thread is scheduled by the lowest priority scheduling class
static void idle_thread(void* arg) {
	printl("Idle thread");
	while (1);
}

/// Iterates through all the scheduling classes picks the next thread to run
static struct thread* core_scheduler(void) {

	// The first scheduling class is the real-time class
	const struct scheduling_class* class = &rt_class;

	// Go through all the scheduling classes and check if it offers a thread
	for (class = &rt_class; class != NULL; class = class->next) {
		struct thread* thread = class->pick_thread(&cpu_rq);

		if (thread != NULL) {
			return thread;
		}
	}
	panic("Core scheduler error");
	
	return NULL;
}

/// Configures the SysTick and PendSV interrupt priorities, add the idle thread
/// and starts the scheduler
void scheduler_start(void) {

	// Systick interrupt should be disabled
	systick_set_priority(NVIC_PRI_6);
	pendsv_set_priority(NVIC_PRI_7);
	
	// Add the IDLE to the list
	struct thread_info idle_info = {
		.name       = "Idle",
		.stack_size = 100,
		.thread     = idle_thread,
		.arg        = NULL,
		.class      = IDLE
	};

	// Make the idle and test thread
	idle = new_thread(&idle_info);

	cpsid_f();
	systick_set_rvr(SYSTICK_RVR);
	systick_enable(1);

	// The `scheduler_run` does not care about the `curr_thread`. However it 
	// MUST be set in order for the cotext switch to work. If the `curr_thread`
	// is not set, this will give an hard fault.
	next_thread = core_scheduler();
	curr_thread = next_thread;

	// Check if the idle thread is present
	if (cpu_rq.idle == NULL) {
		panic("Idle not present");
	}

	// Start executing the first thread
	scheduler_run();
}

void systick_handler(void) {
	
	// Compute the runtime of the current running thread
	if (reschedule_pending) {
		reschedule_pending = 0;

		// Calculate the runtime
		u32 cvr = systick_get_cvr();
		tick += (u64)(SYSTICK_RVR - cvr);
	} else {
		// No reschedule is pending so the runtime is SYSTICK_RVR
		tick += SYSTICK_RVR;
	}

	// Go over the delay queue and move threads with expired delays back into 
	// the running queue
	struct dlist_node* iter = cpu_rq.sleep_q.first;

	while (iter) {
		u64 tick_to_wake = ((struct thread *)iter->obj)->tick_to_wake;
		if (tick_to_wake < tick) {

			// Remove the thread for the delay queue
			dlist_remove(iter, &cpu_rq.sleep_q);

			// Place the thread back into the running list
			struct thread* t = (struct thread *)iter->obj;
			t->class->enqueue(t, &cpu_rq);
			t->tick_to_wake = 0;
		} else {
			// The tick to wake is higher than the current kernel tick. It 
			// should not be removed from the sleep_q
			break;
		}
		iter = iter->next;
	}

	if (curr_thread->tick_to_wake == 0) {
		// The current thread has to be enqueued again
		curr_thread->class->enqueue((struct thread *)curr_thread, &cpu_rq);
	}

	// Call the core scheduler
	next_thread = core_scheduler();

	// Pend the PendSV handler
	pendsv_set_pending();
}

/// The reschedule will pend the SysTick interrupt. This will compute the
/// actual runtime and run the scheduler
void reschedule(void) {
	reschedule_pending = 1;
	systick_set_pending();
}

/// This will enqueue the thread into the sorted `sleep_q` list. The first 
/// node will have to lowest tick to wake

/// first
///  400 600 1000
void scheduler_enqueue_delay(struct thread* thread) {
	struct dlist_node* iter = cpu_rq.sleep_q.first;

	if (iter == NULL) {
		dlist_insert_first(&thread->rq_node, &cpu_rq.sleep_q);
	} else {
		while (iter != NULL) {
			// Check if the `iter` node has a higher `tick_to_wake` value than
			// the thread to insert. If that is the case, insert the thread 
			// befor `iter`
			struct thread* t = (struct thread *)iter->obj;
			if (t->tick_to_wake > thread->tick_to_wake) {
				dlist_insert_before(&thread->rq_node, iter, &cpu_rq.sleep_q);
				break;
			}
			iter = iter->next;
		}

		if (iter == NULL) {
			dlist_insert_last(&thread->rq_node, &cpu_rq.sleep_q);
		}
	}
}
