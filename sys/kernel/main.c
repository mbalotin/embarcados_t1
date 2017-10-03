/**
 * @file main.c
 * @author Sergio Johann Filho
 * @date January 2016
 *
 * @section LICENSE
 *
 * This source code is licensed under the GNU General Public License,
 * Version 2.  See the file 'doc/license/gpl-2.0.txt' for more details.
 *
 * @section DESCRIPTION
 *
 * The HellfireOS realtime operating system kernel.
 *
 */

#include <hal.h>
#include <libc.h>
#include <kprintf.h>
#include <malloc.h>
#include <queue.h>
#include <kernel.h>
#include <panic.h>
#include <scheduler.h>
#include <task.h>
#include <processor.h>
#include <main.h>
#include <ecodes.h>

static void print_config(void)
{
    kprintf("\n===========================================================");
    kprintf("\nHellfireOS %s [%s, %s]", KERN_VER, __DATE__, __TIME__);
    kprintf("\nEmbedded Systems Group - GSE, PUCRS - [2007 - 2017]");
    kprintf("\n===========================================================\n");
    kprintf("\narch:          %s", CPU_ARCH);
    kprintf("\nsys clk:       %d kHz", CPU_SPEED/1000);
    if (TIME_SLICE != 0)
        kprintf("\ntime slice:    %d us", TIME_SLICE);
    kprintf("\nheap size:     %d bytes", sizeof(krnl_heap));
    kprintf("\nmax tasks:     %d\n", MAX_TASKS);
}

static void clear_tcb(void)
{
    uint16_t i;

    for(i = 0; i < MAX_TASKS; i++)
    {
        krnl_task = &krnl_tcb[i];
        krnl_task->id = -1;
        memset(krnl_task->name, 0, sizeof(krnl_task->name));
        krnl_task->state = TASK_IDLE;
        krnl_task->priority = 0;
        krnl_task->priority_rem = 0;
        krnl_task->delay = 0;
        krnl_task->rtjobs = 0;
        krnl_task->bgjobs = 0;
        krnl_task->deadline_misses = 0;
        krnl_task->period = 0;
        krnl_task->capacity = 0;
        krnl_task->deadline = 0;
        krnl_task->capacity_rem = 0;
        krnl_task->deadline_rem = 0;
        krnl_task->ptask = NULL;
        krnl_task->pstack = NULL;
        krnl_task->stack_size = 0;
        krnl_task->other_data = 0;
    }

    krnl_tasks = 0;
    krnl_current_task = 0;
    krnl_schedule = 0;
}

static void clear_pcb(void)
{
    krnl_pcb.sched_rt = sched_rma;
    krnl_pcb.sched_be = sched_priorityrr;
    krnl_pcb.coop_cswitch = 0;
    krnl_pcb.preempt_cswitch = 0;
    krnl_pcb.interrupts = 0;
    krnl_pcb.tick_time = 0;
}

static void init_queues(void)
{
    krnl_run_queue = hf_queue_create(MAX_TASKS);
    if (krnl_run_queue == NULL) panic(PANIC_OOM);
    krnl_delay_queue = hf_queue_create(MAX_TASKS);
    if (krnl_delay_queue == NULL) panic(PANIC_OOM);
    krnl_rt_queue = hf_queue_create(MAX_TASKS);
    if (krnl_rt_queue == NULL) panic(PANIC_OOM);

    krnl_tarefas_aper = hf_queue_create(MAX_TASKS);
    if (krnl_tarefas_aper  == NULL) panic(PANIC_OOM);
}

static void idletask(void)
{
    kprintf("\nKERNEL: free heap: %d bytes", krnl_free);
    kprintf("\nKERNEL: HellfireOS is running\n");

    hf_schedlock(0);

    for (;;)
    {
        _cpu_idle();
    }
}

/**
 * @brief Escalonador de Tarefas Aperiódicas
 * return void
 * Verifica se há tarefas na fila de Aperiódicas para serem executadas (hf_queue_count());
 *   Se há tarefas, então pegar a primeira da fila (hf_queue_get()) e verificar se há jobs restantes;
 *      Se há jobs então faz:
 *          - decrementa o número de jobs;
 *          - configurar pontos de retorno no inicio do loop e escalonar a tarefa aperiódica;
 *      Se não há jobs então faz:
 *          - remove da fila (hf_queue_remhead());
 *          - volta para verificação de task aperiódicas;
 *   Se não há tarefas, então chamar hf_yield() e continuar laço de repetição;
*/

static void escalonadorAperiodico(void)
{
    int32_t rc;
    volatile int32_t status;

    for (;;)
    {

        //Habilitar interrupções;
        status = _di();

        //Salvar contexto da tarefa atual;
        krnl_task = &krnl_tcb[krnl_current_task];
        rc = setjmp(krnl_task->task_context);

        if (rc)
        {
            _ei(status);
            continue;
        }

        //Magic of satã
        if (krnl_task->pstack[0] != STACK_MAGIC)
        {
            panic(PANIC_STACK_OVERFLOW);
        }

        //Alterar o estado da tarefa atual;
        if (krnl_task->state == TASK_RUNNING)
            krnl_task->state = TASK_READY;



        //Verifica se há tarefas na fila de aperiódicas;
        if (hf_queue_count(krnl_tarefas_aper) > 0)
        {
            //Retirar a primeira tarefa aperiódica da fila;
            krnl_task = hf_queue_remhead(krnl_tarefas_aper);

            krnl_current_task = krnl_task->id;

            //Alterar o estado para Running;
            krnl_task->state = TASK_RUNNING;

            //Decrementar o número de jobs da tarefa;
            krnl_task->capacity = (krnl_task->capacity) - 1;

            //Verificar se ainda há jobs dela;
            if (krnl_task->capacity > 0)
            {
                //Adicionar a tarefa com job decrementado ao fim da fila de tarefas aperiódicas;
                if (hf_queue_addtail(krnl_tarefas_aper, krnl_task)) panic(PANIC_CANT_PLACE_RUN);
            }

            //Remover tarefa aperiódica que não possui mais jobs a serem realizados;
            else
            {
                hf_queue_remhead(krnl_tarefas_aper);
                //hf_kill(krnl_current_task);
            }

        }
        else
        {
            _ei(status);
            hf_yield();
        }
    }
}

//
//static void escalonadorAperiodico(void)
//{
//    dprintf("oi 1");
//    int32_t rc;
//    volatile int32_t status;
//
//    uint16_t krnl_aper_current_task = 0;
//    hf_schedlock(0);
//
//    for( ;; )
//    {
//        status = _di();
//
//        if (hf_queue_count(krnl_tarefas_aper) == 0)
//        {
//            _ei(status);
//            hf_yield();
//            continue;
//        }
//
//        krnl_task = &krnl_tcb[krnl_current_task];
//        rc = setjmp(krnl_task->task_context);
//
//        if (rc)
//        {
//            _ei(status);
//            hf_yield();
//            continue;
//        }
//
//        if (krnl_task->pstack[0] != STACK_MAGIC)
//        {
//            panic(PANIC_STACK_OVERFLOW);
//        }
//
//        if (krnl_task->state == TASK_RUNNING)
//            krnl_task->state = TASK_READY;
//
//        #if KERNEL_LOG >= 1
//        kprintf("\n[APER]%d %d %d %d %d ", krnl_aper_current_task, krnl_task->period, krnl_task->capacity, krnl_task->deadline, (uint32_t)_read_us());
//        #endif
//
//
//        if (krnl_tasks > 0)
//        {
//            //pegar id da proxima tas)
//            krnl_current_task = hf_queue_remhead(krnl_tarefas_aper);
//            krnl_current_task->state = TASK_RUNNING;
//            krnl_pcb.coop_cswitch++;
//
//            if (krnl_current_task->capacity > 0)
//            {
//                krnl_current_task->capacity =  krnl_current_task->capacity - 1;
//                if (hf_queue_addtail(krnl_tarefas_aper, krnl_current_task)) panic(PANIC_CANT_PLACE_RUN);
//                _restoreexec(krnl_task->task_context, status, krnl_current_task);
//            }
//            else
//            {
//                hf_kill(krnl_current_task);
//            }
//        }
//        else
//        {
//            panic(PANIC_NO_TASKS_LEFT);
//        }
//
//        _ei(status);
//    }
//}

/**
 * @internal
 * @brief HellfireOS kernel entry point and system initialization.
 *
 * @return should not return.
 *
 * We assume that the following machine state has been already set
 * before this routine.
 *	- Kernel BSS section is filled with 0.
 *	- Kernel stack is configured.
 *	- All interrupts are disabled.
 *	- Minimum page table is set. (MMU systems only)
 */
int main(void)
{
    static uint32_t oops=0xbaadd00d;

    dprintf("olar");
    _hardware_init();
    hf_schedlock(1);
    _di();
    kprintf("\nKERNEL: booting...");
    if (oops == 0xbaadd00d)
    {
        oops = 0;
        print_config();
        _vm_init();
        clear_tcb();
        clear_pcb();
        init_queues();
        _sched_init();
        _irq_init();
        _timer_init();
        _timer_reset();

        //hf_spawn(idletask, 0, 0, 0, "idle task", 1024);
        hf_spawn(escalonadorAperiodico,10,1,10, "Aperiodic task", 1024);

        _device_init();
        _task_init();
        app_main();
        _restoreexec(krnl_task->task_context, 1, krnl_current_task);
        panic(PANIC_ABORTED);
    }
    else
    {
        panic(PANIC_GPF);
    }

    return 0;
}

