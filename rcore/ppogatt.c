/* ppogatt.c
 * Pebble Protocol over GATT (Bluetooth LE) implementation
 * RebbleOS
 *
 * Author: Joshua Wise <joshua@joshuawise.com>
 *
 * The PPoGATT layer lies between the Bluetooth LE stack interface (which
 * handles the nuance of configuring the BLE peripheral itself, setting up
 * the GATT connection, handling NOTIFY / READ / WRITE characteristic
 * events, doing advertising, etc., etc., etc.) and the Pebble Protocol
 * implementation ("bluetooth.c"), which generates high-level Pebble
 * packets.
 *
 * PPoGATT is, at its essence, an implementation of a reliable in-order
 * protocol on top of an otherwise unreliable and unordered transport.  It
 * uses WRITE COMMAND and NOTIFY operations, depending on which end is
 * client and which end is server; those operations do not have
 * acknowledgements, so PPoGATT layers acknowledgements inside of its own
 * protocol.
 *
 * The PPoGATT protocol is a relatively simple shim around the Pebble
 * Protocol.  The first byte of a PPoGATT packet is a bitfield:
 *   data[7:0] = {seq[4:0], cmd[2:0]}
 *
 * cmd can have four values that we know of:
 *
 *   3'd0: Data packet with sequence `seq`.  Should be responded to with an
 *         ACK packet with the same sequence.  If a packet in sequence is
 *         missing, do not respond with any ACKs until the missing sequenced
 *         packet is retransmitted.
 *   3'd1: ACK for data packet with sequence `seq`.
 *   3'd2: Reset request. [has data unknown]
 *   3'd3: Reset ACK. [has data unknown]
 *
 * Sequences are increasing and repeating.
 */

#include "platform.h"
#include "FreeRTOS.h"
#include "task.h" /* xTaskCreate */
#include "queue.h" /* xQueueCreate */
#include "log.h" /* KERN_LOG */
#include "rbl_bluetooth.h"
#include "minilib.h"

#ifdef BLUETOOTH_IS_BLE

enum ppogatt_cmd {
    PPOGATT_CMD_DATA = 0,
    PPOGATT_CMD_ACK = 1,
    PPOGATT_CMD_RESET_REQ = 2,
    PPOGATT_CMD_RESET_ACK = 3,
};

#define STACK_SIZE_PPOGATT_RX (configMINIMAL_STACK_SIZE + 600)
#define STACK_SIZE_PPOGATT_TX (configMINIMAL_STACK_SIZE + 600)

static TaskHandle_t _task_ppogatt_rx = 0;
static StaticTask_t _task_ppogatt_rx_tcb;
static StackType_t  _task_ppogatt_rx_stack[STACK_SIZE_PPOGATT_RX];

static TaskHandle_t _task_ppogatt_tx = 0;
static StaticTask_t _task_ppogatt_tx_tcb;
static StackType_t  _task_ppogatt_tx_stack[STACK_SIZE_PPOGATT_TX];

/* XXX: could be optimized to save memory and support more outstanding
 * packets by allocating from a variable-sized pool, so we don't waste a
 * whole queue entry when we potentially only need an ACK's worth of data */

#define PPOGATT_MTU 256

#define PPOGATT_RX_QUEUE_SIZE 4
#define PPOGATT_TX_QUEUE_SIZE 4

struct ppogatt_packet {
    uint32_t len;
    uint8_t buf[PPOGATT_MTU];
};

static QueueHandle_t         _queue_ppogatt_rx = 0;
static StaticQueue_t         _queue_ppogatt_rx_qcb;
static struct ppogatt_packet _queue_ppogatt_rx_buf[PPOGATT_RX_QUEUE_SIZE];

static QueueHandle_t         _queue_ppogatt_tx = 0;
static StaticQueue_t         _queue_ppogatt_tx_qcb;
static struct ppogatt_packet _queue_ppogatt_tx_buf[PPOGATT_TX_QUEUE_SIZE];

static void _ppogatt_rx_main(void *param) {
    while (1) {
        struct ppogatt_packet pkt;
        
        xQueueReceive(_queue_ppogatt_rx, &pkt, portMAX_DELAY); /* does not fail, since we wait forever */
        
        DRV_LOG("bt", APP_LOG_LEVEL_INFO, "RX %d bytes", pkt.len);
        xQueueSendToBack(_queue_ppogatt_tx, &pkt, portMAX_DELAY); /* just echo it */
    }
}

/* XXX: PPoGATT TX thread probably doesn't have a ppogatt_packet queue, but
 * instead has a produce/consume buffer, and also a ACK-needed / ACK-sent
 * pair of chasing counters.  It prioritizes sending an ACK if one is
 * needed.  (Note that ACK-needed is both a counter and a flag; if a datum
 * is retransmitted, even if we think we've sent an ACK, they might not have
 * heard it, so we'd bump the ACK-needed flag without incrementing the
 * counter.)
 *
 * The produce side of the data produce/consume makes sense, but the consume
 * has multiple pointers for various sequence numbers past.  Note that the
 * *rx* thread bumps forward the consume pointers once ACKs come back in. 
 * The TX thread also probably needs to remember when it needs to
 * retransmit the outstanding packets...
 */

static void _ppogatt_tx_main(void *param) {
    while (1) {
        struct ppogatt_packet pkt;
        int rv;
        
        xQueueReceive(_queue_ppogatt_tx, &pkt, portMAX_DELAY); /* does not fail, since we wait forever */
        
        while (ble_ppogatt_tx(pkt.buf, pkt.len) < 0) {
            rv = ulTaskNotifyTake(pdTRUE /* clear on exit */, pdMS_TO_TICKS(250) /* try again, even if the stack wedges */);
            if (rv == 0) {
                DRV_LOG("bt", APP_LOG_LEVEL_ERROR, "warning: BLE stack did not notify TX ready?");
            }
        }
    }
}

static void _ppogatt_callback_txready() {
    BaseType_t woken = pdFALSE;
    
    vTaskNotifyGiveFromISR(_task_ppogatt_tx, &woken);
    
    portYIELD_FROM_ISR(woken);
}

static struct ppogatt_packet _ppogatt_rx_msg;

static void _ppogatt_callback_rx(const uint8_t *buf, size_t len) {
    BaseType_t woken = pdFALSE;
    
    memcpy(_ppogatt_rx_msg.buf, buf, len);
    _ppogatt_rx_msg.len = len;
    
    /* If it fails, we'll retransmit later -- ignore the return. */
    (void) xQueueSendFromISR(_queue_ppogatt_rx, &_ppogatt_rx_msg, &woken);
    
    portYIELD_FROM_ISR(woken);
}

/* Main entry for PPoGATT code ... to be called at boot, and whenever a
 * PPoGATT connection is reset.  */
void ppogatt_init() {
    /* Shut down anything pending before we start clearing queues. */
    if (_task_ppogatt_rx) {
        vTaskDelete(_task_ppogatt_rx);
        _task_ppogatt_rx = 0;
    }
    
    if (_task_ppogatt_tx) {
        vTaskDelete(_task_ppogatt_tx);
        _task_ppogatt_rx = 0;
    }
    
    /* Kill off the queues. */
    if (_queue_ppogatt_rx) {
        QueueHandle_t oldq = _queue_ppogatt_rx;
        _queue_ppogatt_rx = 0;
        vQueueDelete(oldq);
    }

    if (_queue_ppogatt_tx) {
        QueueHandle_t oldq = _queue_ppogatt_tx;
        _queue_ppogatt_tx = 0;
        vQueueDelete(oldq);
    }
    
    /* Create new queues. */
    _queue_ppogatt_rx = xQueueCreateStatic(PPOGATT_RX_QUEUE_SIZE, sizeof(struct ppogatt_packet), (void *)_queue_ppogatt_rx_buf, &_queue_ppogatt_rx_qcb);
    _queue_ppogatt_tx = xQueueCreateStatic(PPOGATT_TX_QUEUE_SIZE, sizeof(struct ppogatt_packet), (void *)_queue_ppogatt_tx_buf, &_queue_ppogatt_tx_qcb);
    
    /* Start up the PPoGATT tasks. */
    _task_ppogatt_rx = xTaskCreateStatic(_ppogatt_rx_main, "PPoGATT rx", STACK_SIZE_PPOGATT_RX, NULL, tskIDLE_PRIORITY + 4UL, _task_ppogatt_rx_stack, &_task_ppogatt_rx_tcb);
    _task_ppogatt_tx = xTaskCreateStatic(_ppogatt_tx_main, "PPoGATT tx", STACK_SIZE_PPOGATT_TX, NULL, tskIDLE_PRIORITY + 4UL, _task_ppogatt_tx_stack, &_task_ppogatt_tx_tcb);
    
    /* Point the ISRs at us. */
    ble_ppogatt_set_callback_rx(_ppogatt_callback_rx);
    ble_ppogatt_set_callback_txready(_ppogatt_callback_txready);
}

/*** PPoGATT <-> BT stack ***/

void bt_device_request_tx(uint8_t *data, uint16_t len) {
}

#endif
