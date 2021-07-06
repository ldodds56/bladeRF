/* This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (c) 2018 Nuand LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdint.h>
#include <stdbool.h>
#include "pkt_handler.h"
#include "pkt_retune2.h"
#include "nios_pkt_retune2.h"    /* Packet format definition */
#include "devices.h"
#include "debug.h"

#ifdef BLADERF_NIOS_LIBAD936X
void rfic_invalidate_frequency(bladerf_module module);
#endif  // BLADERF_NIOS_LIBAD936X

#ifdef BLADERF_NIOS_DEBUG
    volatile uint32_t pkt_retune2_error_count = 0;
#   define INCREMENT_ERROR_COUNT() do { pkt_retune2_error_count++; } while (0)
#else
#   define INCREMENT_ERROR_COUNT() do {} while (0)
#endif

/* The enqueue/dequeue routines require that this be a power of two */
#define RETUNE2_QUEUE_MAX   16
#define QUEUE_FULL          0xff
#define QUEUE_EMPTY         0xfe

#define BLADERF_TRIGGER_REG_ARM ((uint8_t)(1 << 0))
#define BLADERF_TRIGGER_REG_MASTER ((uint8_t)(1 << 2))
#define BLADERF_TRIGGER_REG_LINE ((uint8_t)(1 << 3))


/* State of items in the retune queue */
enum entry_state {
    ENTRY_STATE_INVALID = 0,  /* Marks entry invalid and not in use */
    ENTRY_STATE_NEW,          /* We have a new retune request to satisfy */
    ENTRY_STATE_SCHEDULED,    /* We've scheduled the timer interrupt for
                               * this entry and are awaiting the ISR */
    ENTRY_STATE_READY,        /* The timer interrupt has fired - we should
                               * handle this retune */
    ENTRY_STATE_DONE,         /* Retune is complete */
};

/* State of trigger vs scheduling */
enum trigger_state {
    TRIGGER_IDLE = 0,       
    TRIGGER_INIT,       
    TRIGGER_RUN,
    TRIGGER_DONE,
};

/* Struct holding all necessary info regarding trigger */
struct trigger_state_info {
    enum trigger_state state;       /* Holds state of trigger event */
    uint64_t timestamp;             /* Timestamp of start of trigger */
    uint8_t idx;                    /* Current offset in trigger queue */
    uint64_t period;                /* Period of retunes */
    uint64_t iter;
};

struct trigger_state_info rx_trigger_info = {.state = TRIGGER_IDLE, .timestamp = 0, .idx = 0};
struct trigger_state_info tx_trigger_info = {.state = TRIGGER_IDLE, .timestamp = 0, .idx = 0};

struct queue_entry {
    volatile enum entry_state state;
    fastlock_profile *profile;
    uint64_t timestamp;
};

static struct queue {
    uint8_t count;      /* Total number of items in the queue */
    uint8_t ins_idx;    /* Insertion index */
    uint8_t rem_idx;    /* Removal index */

    struct queue_entry entries[RETUNE2_QUEUE_MAX];
} rx_queue, tx_queue, rx_trigger_queue, tx_trigger_queue;

/* Returns queue size after enqueue operation, or QUEUE_FULL if we could
 * not enqueue the requested item */
static inline uint8_t enqueue_retune(struct queue *q,
                                     fastlock_profile *p,
                                     uint64_t timestamp)
{
    uint8_t ret;

    if (q->count >= RETUNE2_QUEUE_MAX) {
        return QUEUE_FULL;
    }

    q->entries[q->ins_idx].profile = p;
    q->entries[q->ins_idx].state = ENTRY_STATE_NEW;
    q->entries[q->ins_idx].timestamp = timestamp;

    q->ins_idx = (q->ins_idx + 1) & (RETUNE2_QUEUE_MAX - 1);

    q->count++;
    ret = q->count;

    return ret;
}

/* Retune number of items left in the queue after the dequeue operation,
 * or QUEUE_EMPTY if there was nothing to dequeue */
static inline uint8_t dequeue_retune(struct queue *q, struct queue_entry *e)
{
    uint8_t ret;

    if (q->count == 0) {
        return QUEUE_EMPTY;
    }

    if (e != NULL) {
        memcpy(&e, &q->entries[q->rem_idx], sizeof(e[0]));
    }

    q->entries[q->rem_idx].state = ENTRY_STATE_DONE;
    q->rem_idx = (q->rem_idx + 1) & (RETUNE2_QUEUE_MAX - 1);

    q->count--;
    ret = q->count;

    return ret;
}

/* Get the state of the next item in the retune queue */
static inline struct queue_entry* peek_next_retune(struct queue *q)
{
    if (q == NULL) {
        return NULL;
    } else if (q->count == 0) {
        return NULL;
    } else {
        return &q->entries[q->rem_idx];
    }
}

/* Get the queue element at the given offset relative to the removal index */
static inline struct queue_entry* peek_next_retune_offset(struct queue *q,
                                                           uint8_t offset)
{
    if (q == NULL) {
        return NULL;
    } else if (q->count == 0) {
        return NULL;
    } else {
        return &q->entries[(q->rem_idx + offset) & (RETUNE2_QUEUE_MAX - 1)];
    }
}

/* The retune interrupt may fire while this call is occuring, so we should
 * perform these operations in an order that minimizes the race window, and
 * does not cause the race to be problematic. It's fine if the last retune
 * occurs before we can cancel it. */
static void reset_queue(struct queue *q)
{
    unsigned int i;

    q->count = 0;

    for (i = 0; i < RETUNE2_QUEUE_MAX; i++) {
        q->entries[i].state = ENTRY_STATE_INVALID;
    }

    q->rem_idx = q->ins_idx = 0;
}

static inline void profile_load(bladerf_module module, fastlock_profile *p)
{
    if (p == NULL) {
        return;
    }

    adi_fastlock_load(module, p);
}

static inline void profile_load_scheduled(struct queue *q,
                                          bladerf_module module)
{
    struct queue_entry *e;
    uint32_t i;
    uint8_t  used = 0;

    if ((q == NULL) || (q->count == 0)) {
        return;
    }

    /* Check the contents of the retune queue and load all the profiles we can
     * without causing them to step on each other. This should reduce retune
     * times in most scenarios because the profile will have already been
     * loaded into the RFFE when it becomes time to retune. */
    for (i = 0; i < q->count; i++) {
        e = peek_next_retune_offset(q, i);
        if( e != NULL ) {
            if (e->state == ENTRY_STATE_NEW) {
                if ( !(used & (1 << e->profile->profile_num)) ) {
                    /* Profile slot is available in RFFE, fill it */
                    profile_load(module, e->profile);
                    /* Mark profile slot used */
                    used |= 1 << e->profile->profile_num;
                }
            }
        }
    }
}

static inline void profile_activate(bladerf_module module, fastlock_profile *p)
{
    if (p == NULL) {
        return;
    }

#ifdef BLADERF_NIOS_LIBAD936X
    /* Invalidate current frequency knowledge */
    rfic_invalidate_frequency(module);
#endif  // BLADERF_NIOS_LIBAD936X

    /* Activate the RFFE fast lock profile */
    adi_fastlock_recall(module, p);

    /* Adjust the RFFE port */
    adi_rfport_select(p);

    /* Adjust the RF switches */
    adi_rfspdt_select(module, p);
}

static inline void retune_isr(struct queue *q, uint8_t offset)
{
    struct queue_entry *e = peek_next_retune_offset(q, offset);
    if (e != NULL) {
        if (e->state == ENTRY_STATE_SCHEDULED) {
            e->state = ENTRY_STATE_READY;
        } else {
            INCREMENT_ERROR_COUNT();
        }
    }
}


#ifndef BLADERF_NIOS_PC_SIMULATION
static void retune_rx(void *context)
{
    /* Handle the ISR */
    if (rx_trigger_info.state == TRIGGER_IDLE || rx_trigger_info.state == TRIGGER_DONE) {
        retune_isr(&rx_queue, 0); // Use schedule queue
    } else {
        retune_isr(&rx_trigger_queue, rx_trigger_info.idx); // Use trigger queue
    }

    /* Clear the interrupt */
    timer_tamer_clear_interrupt(BLADERF_MODULE_RX);
}

static void retune_tx(void *context)
{
    /* Handle the ISR */
    if (tx_trigger_info.state == TRIGGER_IDLE || tx_trigger_info.state == TRIGGER_DONE) { 
        retune_isr(&tx_queue, 0); // Use schedule queue
    } else {
        retune_isr(&tx_trigger_queue, tx_trigger_info.idx); // Use trigger queue
    }
    /* Clear the interrupt */
    timer_tamer_clear_interrupt(BLADERF_MODULE_TX);
}
#endif


void pkt_retune2_init()
{
    reset_queue(&rx_queue);
    reset_queue(&tx_queue);

#ifndef BLADERF_NIOS_PC_SIMULATION

    /* Register RX Time Tamer ISR */
    alt_ic_isr_register(
        RX_TAMER_IRQ_INTERRUPT_CONTROLLER_ID,
        RX_TAMER_IRQ,
        retune_rx,
        NULL,
        NULL
    ) ;

    /* Register TX Time Tamer ISR */
    alt_ic_isr_register(
        TX_TAMER_IRQ_INTERRUPT_CONTROLLER_ID,
        TX_TAMER_IRQ,
        retune_tx,
        NULL,
        NULL
    ) ;
#endif
}

static inline void perform_work(struct queue *schedule_queue, struct queue *trigger_queue, struct trigger_state_info *trigger_info, bladerf_module module)
{    
    uint8_t trigger_ctl;
    switch (module) {
        case BLADERF_MODULE_TX:
            trigger_ctl = tx_trigger_ctl_read();
            break;
        case BLADERF_MODULE_RX:
            trigger_ctl = rx_trigger_ctl_read();
            break;
        default:
            return;
    } 
    bool trigger_rearmed = ((trigger_ctl & BLADERF_TRIGGER_REG_LINE) == BLADERF_TRIGGER_REG_LINE);
    bool trigger_fired = ((trigger_ctl & BLADERF_TRIGGER_REG_LINE) == 0);

    struct queue_entry *e = NULL;  
    switch(trigger_info->state) {
        case TRIGGER_INIT:
            // Reset schedule queue to prevent overlapping retunes
            // TODO: Revisit this behavior
            //reset_queue(schedule_queue);

            trigger_info->idx = 0;
            trigger_info->iter = 0;
            if (trigger_rearmed) {
                trigger_info->state = TRIGGER_IDLE;
            } else {
                e = peek_next_retune(trigger_queue);  //TODO: Assumes at least 1
                trigger_info->state = TRIGGER_RUN;
            }
            break;
        case TRIGGER_RUN:
            // Stop trigger retunes if trigger was rearmed
            if (trigger_rearmed) {
                // Reset trigger state
                trigger_info->state = TRIGGER_IDLE;
                // Reset current entry if applicable
                if (trigger_info->idx < trigger_queue->count) {
                    struct queue_entry* current_entry = peek_next_retune_offset(trigger_queue, trigger_info->idx);
                    current_entry->state = ENTRY_STATE_NEW;
                }
            } else {
                // Check if all retunes are finished
                if (trigger_queue->count == trigger_info->idx) {
                    if (trigger_info->period == 0) {
                        trigger_info->state = TRIGGER_DONE;
                    }
                    trigger_info->idx = 0;
                    trigger_info->iter = trigger_info->iter + 1;
                }
                e = peek_next_retune_offset(trigger_queue, trigger_info->idx);
            }
            break;
        
        case TRIGGER_IDLE:
            if (trigger_queue->count != 0 && trigger_fired) { 
                trigger_info->state = TRIGGER_INIT;
                trigger_info->timestamp = time_tamer_read(module); // Shouldn't leave this here but for testing
            } else {
                e = peek_next_retune(schedule_queue);
            }
            break;
        
        case TRIGGER_DONE:
            e = peek_next_retune(schedule_queue);
            if (trigger_rearmed) {
                trigger_info->state = TRIGGER_IDLE;
            }
            break;
    }

    if (e == NULL) {
        return;
    }

    switch (e->state) {
        case ENTRY_STATE_NEW:

            /* Load the fast lock profile into the RFFE */
            profile_load(module, e->profile);

            /* Schedule the retune */
            e->state = ENTRY_STATE_SCHEDULED;
            if (trigger_info->state == TRIGGER_IDLE || trigger_info->state == TRIGGER_DONE) {
                tamer_schedule(module, e->timestamp);
            } else {
                tamer_schedule(module, e->timestamp + trigger_info->timestamp + trigger_info->iter * trigger_info->period);
            }

            break;

        case ENTRY_STATE_SCHEDULED:

            /* Nothing to do.
             * Waiting for this entry to become ready */
            break;

        case ENTRY_STATE_READY:

            /* Activate the fast lock profile for this retune */
            profile_activate(module, e->profile);

            // TODO: Set up states so this is masK?
            if (trigger_info->state == TRIGGER_IDLE || trigger_info->state == TRIGGER_DONE) {
                /* Drop the item from the queue */
                dequeue_retune(schedule_queue, NULL);
            } else {
                trigger_info->idx += 1;
                e->state = ENTRY_STATE_NEW;
            }

            break;

        default:
            INCREMENT_ERROR_COUNT();
            break;
    }
}

void pkt_retune2_work(void)
{
    perform_work(&rx_queue, &rx_trigger_queue, &rx_trigger_info, BLADERF_MODULE_RX);
    perform_work(&tx_queue, &tx_trigger_queue, &tx_trigger_info, BLADERF_MODULE_TX);
}

void pkt_retune2(struct pkt_buf *b)
{
    int status = -1;
    bladerf_module module;
    uint8_t flags;
    uint64_t timestamp;
    uint64_t start_time;
    uint64_t end_time;
    uint64_t duration = 0;
    uint16_t nios_profile;
    uint8_t rffe_profile;
    uint8_t port;
    uint8_t spdt;
    fastlock_profile *profile;

    flags = NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS;

    nios_pkt_retune2_unpack(b->req, &module, &timestamp,
                            &nios_profile, &rffe_profile, &port, &spdt);

    switch (module) {
        case BLADERF_MODULE_RX:
            profile = &fastlocks_rx[nios_profile];
            break;
        case BLADERF_MODULE_TX:
            profile = &fastlocks_tx[nios_profile];
            break;
        default:
            profile = NULL;
    }

    if (profile == NULL) {
        INCREMENT_ERROR_COUNT();
        status = -1;
    } else {
        /* Update the fastlock profile data */
        profile->profile_num = rffe_profile;
        profile->port = port;
        profile->spdt = spdt;
    }

    start_time = time_tamer_read(module);

    if (timestamp == NIOS_PKT_RETUNE2_NOW) {
        /* Fire off this retune operation now */
        switch (module) {
            case BLADERF_MODULE_RX:
            case BLADERF_MODULE_TX:

                /* Load the profile data into RFFE memory */
                profile_load(module, profile);

                /* Activate the fast lock profile for this retune */
                profile_activate(module, profile);

                flags |= NIOS_PKT_RETUNE2_RESP_FLAG_TSVTUNE_VALID;

                status = 0;
                break;

            default:
                INCREMENT_ERROR_COUNT();
                status = -1;
        }

    } else if (timestamp == NIOS_PKT_RETUNE2_CLEAR_QUEUE) {
        switch (module) {
            case BLADERF_MODULE_RX:
                reset_queue(&rx_queue);
                reset_queue(&rx_trigger_queue);
                status = 0;
                break;

            case BLADERF_MODULE_TX:
                reset_queue(&tx_queue);
                reset_queue(&tx_trigger_queue);
                status = 0;
                break;

            default:
                INCREMENT_ERROR_COUNT();
                status = -1;
        }
    } else if ((timestamp & NIOS_PKT_RETUNE2_TRIGGER_MASK) == NIOS_PKT_RETUNE2_TRIGGER_MASK) {
        // uint8_t queue_size;
        uint64_t relative_timestamp = timestamp & NIOS_PKT_RETUNE2_TRIGGER_TIMESTAMP_MASK;
        uint64_t period = (timestamp & NIOS_PKT_RETUNE2_TRIGGER_PERIOD_MASK) >> 16;
        switch (module) {
            case BLADERF_MODULE_RX:
                // queue_size = enqueue_retune(&rx_trigger_queue, profile, relative_timestamp);
                enqueue_retune(&rx_trigger_queue, profile, relative_timestamp);
                profile_load_scheduled(&rx_trigger_queue, module);
                status = 0;
                if (period != ((2<<15)-1)) rx_trigger_info.period = period;
                break;

            case BLADERF_MODULE_TX:
                // queue_size = enqueue_retune(&tx_trigger_queue, profile, relative_timestamp);
                enqueue_retune(&tx_trigger_queue, profile, relative_timestamp);
                profile_load_scheduled(&tx_trigger_queue, module);
                status = 0;
                if (period != ((2<<15)-1)) tx_trigger_info.period = period;
                break;

            default:
                INCREMENT_ERROR_COUNT();
                status = -1;
        }

        // if (queue_size == QUEUE_FULL) {
        //     status = -1;
        // } else {
            if (module == BLADERF_MODULE_RX) {
                status = rx_trigger_info.state;
            } else if (module == BLADERF_MODULE_TX) {
                status = tx_trigger_info.state;
            }
        // }
    } else {
        uint8_t queue_size;

        switch (module) {
            case BLADERF_MODULE_RX:
                queue_size = enqueue_retune(&rx_queue, profile, timestamp);
                profile_load_scheduled(&rx_queue, module);
                break;

            case BLADERF_MODULE_TX:
                queue_size = enqueue_retune(&tx_queue, profile, timestamp);
                profile_load_scheduled(&tx_queue, module);
                break;

            default:
                INCREMENT_ERROR_COUNT();
                queue_size = QUEUE_FULL;

        }

        if (queue_size == QUEUE_FULL) {
            status = -1;
        } else {
            status = 0;
        }
    }

    end_time = time_tamer_read(module);
    duration = end_time - start_time;

    if (status != 0) {
        flags &= ~(NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS);
    }

    nios_pkt_retune2_resp_pack(b->resp, duration, flags);
}