/*
 * Copyright 2022, Nataliya Korovkina, malus.brandywine@gmail.com
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* Include config variables. */
#include <autoconf.h>

#include <stdio.h>

#include <simple/simple.h>
#include <simple-default/simple-default.h>

#include <allocman/allocman.h>
#include <allocman/bootstrap.h>
#include <allocman/vka.h>

#include <sel4platsupport/platsupport.h>
#include <sel4platsupport/io.h>

#include <sel4bench/arch/sel4bench.h>

#include <sel4runtime.h>
#include <sel4utils/process.h>
#include <sel4utils/helpers.h>

#include <utils/zf_log.h>
#include <sel4utils/sel4_zf_logif.h>

#include <sel4runtime/gen_config.h>


/* Init thread environment */

#define ALLOCATOR_VIRTUAL_POOL_SIZE ((1 << seL4_PageBits) * 200)
#define ALLOCATOR_STATIC_POOL_SIZE ((1 << seL4_PageBits) * 16)

static char allocator_mem_pool[ALLOCATOR_STATIC_POOL_SIZE];

seL4_BootInfo *info;
simple_t simple;
vka_t vka;
allocman_t *allocman;

sel4utils_alloc_data_t data;
reservation_t virtual_reservation;

seL4_CPtr cspace_cap;
seL4_CPtr pd_cap;
vspace_t vspace;


/* Child threads support*/

/* Stack size, in 4Kb pages */
#define THREAD_STACK_SIZE 4

void *thread_1_stack;
void *thread_2_stack;

vka_object_t tcb_thread1;
vka_object_t tcb_thread2;

uintptr_t tls1;
uintptr_t tls2;

vka_object_t sched_cntx1;
vka_object_t sched_cntx2;


/* tls region for Low prio and High prio threads */
char *tls_region1;
char *tls_region2;

/* IPC buffer for Low prio and High prio threads */
seL4_IPCBuffer *ipcbuf1;
seL4_IPCBuffer *ipcbuf2;

#define TIME_SLICE  100


/* Measurement procedure support */

/* Init Test size */
#define KB_INIT_TEST_SIZE           1

/* Delta */
#define KB_DELTA                    1

/* Group size, Number of Tests in a Group */
#define KB_GROUP_SIZE               30

/* Number of Test Groups in a sequence*/
#define KB_N_GROUPS                 5


/* How to calculate buffer size to keep the table of measured latencies:

 Buffer Size = KB_GROUP_SIZE * KB_N_GROUPS * sizeof(ccnt_t) bytes

 KB_TABLE_BUFFER_SIZE defines buffer size in 4K pages
*/

/* Buffer to save a table of measured latencies */
void *latencies_buf;
ccnt_t (*latencies)[KB_GROUP_SIZE][KB_N_GROUPS];

/* Size of latencies table in 4K pages */
#define KB_TABLE_BUFFER_SIZE    8

/* Notification object that Low prio and High prio threads use
 to send/receive a Signal */
vka_object_t ntfn;

/* Notification object that the Low prio thread uses to notify
   init thread about end of the test run */
vka_object_t ntfn_s;

/* Global variable that the Low prio thread uses to share
 a measured value with  init thread */
volatile ccnt_t latency;


/* Functions that are run as Low and High prio threads */
void low_prio(int argc, char **argv);
void high_prio(int argc, char **argv);



/* Both functions setup_threads() and config_thread() are used
 at setup phase of benchmark (before the measuring procedure runs) */

void config_thread(vka_object_t *tcb, const char *thread_name, seL4_Word prio,
                   vka_object_t *sc)
{

    int error = 0;
#ifdef CONFIG_KERNEL_MCS

    error = vka_alloc_sched_context(&vka, sc);
    ZF_LOGF_IF(error, "Failed to allocate schedcontext\n");

    error = seL4_SchedControl_Configure(simple_get_sched_ctrl(&simple, 0),
                                        sc->cptr, TIME_SLICE * US_IN_S, TIME_SLICE * US_IN_S, 0, 0);
    ZF_LOGF_IF(error != seL4_NoError, "Failed to configure schedcontext\n");

    error = seL4_SchedContext_Bind(sc->cptr, tcb->cptr);
    ZF_LOGF_IF(error != seL4_NoError, "Failed to bind schedcontext\n");

    error = seL4_TCB_Configure(tcb->cptr,  cspace_cap, seL4_NilData, pd_cap, seL4_NilData, 0, 0);
    ZF_LOGF_IFERR(error, "Failed to configure the new TCB object\n");

#else
    error = seL4_TCB_Configure(tcb->cptr, seL4_CapNull,  cspace_cap, seL4_NilData, pd_cap, seL4_NilData, 0, 0);
    ZF_LOGF_IFERR(error, "Failed to configure the new TCB object\n");
#endif

    error = seL4_TCB_SetPriority(tcb->cptr, simple_get_tcb(&simple), prio);
    ZF_LOGF_IFERR(error, "Failed to set the priority for the new TCB object\n");

    NAME_THREAD(tcb->cptr, thread_name);

}


void setup_threads()
{
    int error = 0;

    /* Prepare a page for IPC biffers of the child threads */
    ipcbuf1 = vspace_new_pages(&vspace, seL4_AllRights, 1, seL4_PageBits);
    ipcbuf2 = vspace_new_pages(&vspace, seL4_AllRights, 1, seL4_PageBits);

    /* Prepare some pages for stack of the child threads */
    thread_1_stack = vspace_new_pages(&vspace, seL4_AllRights, THREAD_STACK_SIZE, seL4_PageBits);
    thread_2_stack = vspace_new_pages(&vspace, seL4_AllRights, THREAD_STACK_SIZE, seL4_PageBits);

    /* create a thread local storage (TLS) region for the child threads */
    tls_region1 = vspace_new_pages(&vspace, seL4_AllRights, CONFIG_SEL4RUNTIME_STATIC_TLS >> seL4_PageBits, seL4_PageBits);
    tls_region2 = vspace_new_pages(&vspace, seL4_AllRights, CONFIG_SEL4RUNTIME_STATIC_TLS >> seL4_PageBits, seL4_PageBits);

    tls1 = sel4runtime_write_tls_image(tls_region1);
    tls2 = sel4runtime_write_tls_image(tls_region2);

    /* Allocate thread objects */

    error = vka_alloc_tcb(&vka, &tcb_thread1);
    ZF_LOGF_IFERR(error, "Failed to allocate new TCB for Low Prio Thread\n");

    error = vka_alloc_tcb(&vka, &tcb_thread2);
    ZF_LOGF_IFERR(error, "Failed to allocate new TCB for High Prio Thread\n");

    /* Configure the threads */

    config_thread(&tcb_thread1, "Low prio", 254, &sched_cntx1);
    config_thread(&tcb_thread2, "High prio", 255, &sched_cntx2);

}


/* remove_threads() is envoked after the measuring proceudre. */
void remove_threads()
{
    vka_free_object(&vka, &tcb_thread1);
    vka_free_object(&vka, &tcb_thread2);
}



/* Starting thread:
 reset instruction pointer,
 reset stack pointer stack and
 make it runnable.

 start_threads() is a part of measuring procedure */

/* Support of parameters to be passed to Low prio and
 High prio threads */

#define MAX_ARGS_LOW    4
char *argv1[MAX_ARGS_LOW];
char argv_strings1[MAX_ARGS_LOW][WORD_STRING_SIZE];
seL4_Word argc1 = MAX_ARGS_LOW;

#define MAX_ARGS_HIGH    1
char *argv2[MAX_ARGS_HIGH];
char argv_strings2[MAX_ARGS_HIGH][WORD_STRING_SIZE];
seL4_Word argc2 = MAX_ARGS_HIGH;


/* Block of not optimized functions */

#pragma GCC push_options
#pragma GCC optimize ("O0")

void start_threads(int loops)
{
    register int error = 0;
    seL4_UserContext context = {0};
    register size_t context_size = sizeof(seL4_UserContext) / sizeof(seL4_Word);

    /* Low prio thread */

    sel4utils_create_word_args(argv_strings1, argv1, argc1,
                               (seL4_Word) ntfn.cptr, (seL4_Word) ntfn_s.cptr,
                               (seL4_Word) &latency, (seL4_Word) loops);

    error = sel4utils_arch_init_local_context((sel4utils_thread_entry_fn) low_prio, (void *) argc1, (void *) argv1,
                                              ipcbuf1,
                                              thread_1_stack + THREAD_STACK_SIZE * (1 << seL4_PageBits),
                                              &context);
    ZF_LOGF_IFERR(error, "Failed to init local context for Low Prio thread\n");

    error = seL4_TCB_WriteRegisters(tcb_thread1.cptr, 0, 0, context_size, &context);
    ZF_LOGF_IFERR(error, "Failed to write Low Prio thread's register set\n");


    /* High prio thread */

    sel4utils_create_word_args(argv_strings2, argv2, argc2,
                               (seL4_Word) ntfn.cptr);

    error = sel4utils_arch_init_local_context((sel4utils_thread_entry_fn) high_prio, (void *) argc2, (void *) argv2,
                                              ipcbuf2,
                                              thread_2_stack + THREAD_STACK_SIZE * (1 << seL4_PageBits),
                                              &context);
    ZF_LOGF_IFERR(error, "Failed to init local context for High Prio thread\n");


    error = seL4_TCB_WriteRegisters(tcb_thread2.cptr, 0, 0, context_size, &context);
    ZF_LOGF_IFERR(error, "Failed to write High Prio thread's register set\n");


    /* set IPC buffer pointer & TLS base for the new threads */

    error = sel4runtime_set_tls_variable(tls1, __sel4_ipc_buffer, ipcbuf1);
    ZF_LOGF_IF(error, "Failed to set ipc buffer in TLS of Low Prio thread\n");
    error = seL4_TCB_SetTLSBase(tcb_thread1.cptr, tls1);
    ZF_LOGF_IF(error, "Failed to set TLS base for Low Prio thread\n");

    error = sel4runtime_set_tls_variable(tls2, __sel4_ipc_buffer, ipcbuf2);
    ZF_LOGF_IF(error, "Failed to set ipc buffer in TLS of High Prio thread\n");
    /* set the TLS base of the new thread */
    error = seL4_TCB_SetTLSBase(tcb_thread2.cptr, tls2);
    ZF_LOGF_IF(error, "Failed to set TLS base for High Prio thread\n");


    /* Start low prio thread to start measuring loop */
    error = seL4_TCB_Resume(tcb_thread1.cptr);
    ZF_LOGF_IFERR(error, "Failed to start Low Prio thread\n");

    /* Start high prio thread so it wait on notification */
    error = seL4_TCB_Resume(tcb_thread2.cptr);
    ZF_LOGF_IFERR(error, "Failed to start High Prio thread\n");

}


/* Suspend the threads */
void suspend_threads()
{
    register int error = 0;

    error = seL4_TCB_Suspend(tcb_thread2.cptr);
    ZF_LOGF_IFERR(error, "Failed to suspend High Prio thread\n");
    error = seL4_TCB_Suspend(tcb_thread1.cptr);
    ZF_LOGF_IFERR(error, "Failed to suspend Low Prio thread\n");

}


/* Setting up the loops of the measuring procedure */
void run_test()

{
    register int loops = KB_INIT_TEST_SIZE;
    register int j = 0;
    register int k = 0;

    do {
        k = 0;

        do {
            start_threads(loops);
            seL4_Wait(ntfn_s.cptr, NULL);
            suspend_threads();

            (*latencies)[k][j] = latency;

        } while (++k < KB_GROUP_SIZE);

        loops += KB_DELTA;


    } while (++j < KB_N_GROUPS);

}

/* End of Block of not optimized functions */

#pragma GCC pop_options



/* Bench Init: takes care of init thread, bootstrapping stuff */

void kbench_init(const char *thread_name)
{
    int error = 0;

    info = platsupport_get_bootinfo();
    ZF_LOGF_IF(info == NULL, "Failed to get bootinfo\n");

    zf_log_set_tag_prefix("KanataBench: ");
    NAME_THREAD(seL4_CapInitThreadTCB, thread_name);

    simple_default_init_bootinfo(&simple, info);

    allocman = bootstrap_use_current_simple(&simple, ALLOCATOR_STATIC_POOL_SIZE, allocator_mem_pool);
    ZF_LOGF_IF(allocman == NULL, "Failed to initialize alloc manager\n");

    allocman_make_vka(&vka, allocman);

    /* create a vspace (virtual memory management interface). We pass
     * boot info not because it will use capabilities from it, but so
     * it knows the address and will add it as a reserved region */
    error = sel4utils_bootstrap_vspace_with_bootinfo_leaky(&vspace,
                                                           &data, simple_get_pd(&simple),
                                                           &vka, info);
    if (error) {
        ZF_LOGF("Failed to bootstrap vspace\n");
    }

    /* fill the allocator with virtual memory */
    void *vaddr;
    virtual_reservation = vspace_reserve_range(&vspace,
                                               ALLOCATOR_VIRTUAL_POOL_SIZE, seL4_AllRights, 1, &vaddr);
    if (virtual_reservation.res == 0) {
        ZF_LOGF("Failed to provide virtual memory for allocator\n");
    }

    bootstrap_configure_virtual_pool(allocman, vaddr,
                                     ALLOCATOR_VIRTUAL_POOL_SIZE, simple_get_pd(&simple));

    /* Enable serial device for output */
    platsupport_serial_setup_simple(&vspace, &simple, &vka);

    cspace_cap = simple_get_cnode(&simple);

    pd_cap = simple_get_pd(&simple);

}

/* Low prio thread function*/
void low_prio(int argc, char **argv)
{

    seL4_CPtr ntfn = (seL4_CPtr) atol(argv[0]);
    seL4_CPtr ntfn_s = (seL4_CPtr) atol(argv[1]);
    volatile ccnt_t *latency = (volatile ccnt_t *) atol(argv[2]);
    int loops = (int) atol(argv[3]);

    ccnt_t start, end;
    int i = 0;

    SEL4BENCH_READ_CCNT(start);

    do {
        seL4_Signal(ntfn);
    } while (++i < loops);

    SEL4BENCH_READ_CCNT(end);

    *latency = end - start;

    seL4_Signal(ntfn_s);
}



/* High prio thread function*/
void high_prio(int argc, char **argv)
{

    seL4_CPtr ntfn = (seL4_CPtr) atol(argv[0]);

    while (1) {
        seL4_Wait(ntfn, NULL);
    }


}


int main(void)
{

    int error = 0;

    int loops;

    /* Taking care of init thread */
    kbench_init("init thread");

    /* Set up and configure threads */
    setup_threads();

    /* Notification objects for communications:
     Low prio --> High prio and Low prio --> init */
    error = vka_alloc_notification(&vka, &ntfn);
    assert(error == seL4_NoError);
    error = vka_alloc_notification(&vka, &ntfn_s);
    assert(error == seL4_NoError);


    /* Allocate buffer for latencies, 8 4K pages */
    latencies_buf = vspace_new_pages(&vspace, seL4_AllRights, KB_TABLE_BUFFER_SIZE, seL4_PageBits);

    ZF_LOGF_IF(latencies_buf == NULL, "Failed to allocate pages for latencies\n");
    latencies = latencies_buf;

    /* Initialize performance counters (seL4_libs/libsel4bench) */
    sel4bench_init();

    /* Run measuring procedure */
    run_test();


    remove_threads();

    /* Report table of latencies */
    printf("Initial Test size: %d\n"
           "Delta: %d\n"
           "Number of Tests / Sample size of Accumulated latency: %d\n"
           "Number of Groups: %d\n"
           "Accumulated latencies (clock cycles): \n",
           KB_INIT_TEST_SIZE, KB_DELTA, KB_GROUP_SIZE, KB_N_GROUPS);

    for (int k = 0; k < KB_GROUP_SIZE; k++) {
        for (int j = 0; j < KB_N_GROUPS; j++) {
            printf("%lu ", (*latencies)[k][j]);
        }
        printf("\n");
    }

    printf("\nDone!\n");

    return 0;
}
