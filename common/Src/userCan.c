/*
 * userCan.c
 *
 */

#include "userCan.h"
#include "stdbool.h"
#include <string.h>
#include AUTOGEN_HEADER_NAME(BOARD_NAME)
#include "can.h"
#include "debug.h"
#include "bsp.h"
#include "boardTypes.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "semphr.h"

#if IS_BOARD_F7_FAMILY
#include "userCanF7.h"
#elif IS_BOARD_F0_FAMILY
#include "userCanF0.h"
#else
#error no can header file for this board type
#endif

#define CAN_SEND_TIMEOUT_MS 10

#if IS_BOARD_NUCLEO_F0 || IS_BOARD_NUCLEO_F7
#define BOARD_DISABLE_CAN
#endif 

#if IS_BOARD_F7_FAMILY
#define CAN_P0_QUEUE_LEN 10
#define CAN_P1_QUEUE_LEN 15
#define CAN_P2_QUEUE_LEN 10
#define CAN_P3_QUEUE_LEN 15
#elif IS_BOARD_F0_FAMILY
#define CAN_P0_QUEUE_LEN 2
#define CAN_P1_QUEUE_LEN 2
#define CAN_P2_QUEUE_LEN 2
#define CAN_P3_QUEUE_LEN 2
#elif IS_BOARD_F0_FAMILY
#endif

typedef struct CAN_Message {
    uint32_t id;
    uint32_t len;
    uint8_t data[8];
} CAN_Message;

xQueueHandle CAN_Priority0_Queue;
xQueueHandle CAN_Priority1_Queue;
xQueueHandle CAN_Priority2_Queue;
xQueueHandle CAN_Priority3_Queue;
SemaphoreHandle_t CAN_Msg_Semaphore;

// Call into the autogenerated CAN file to send the DTC message
// Since the autogenerate CAN functions have the board name in them, we need
// this macro to create the right function name for the current board
#define DTC_SEND_FUNCTION CAT(CAT(sendCAN_,BOARD_NAME_UPPER),_DTC)

volatile bool canStarted = false;
volatile bool chargerCanStarted = false;

HAL_StatusTypeDef canInit(CAN_HandleTypeDef *hcan)
{
#ifdef BOARD_DISABLE_CAN
    return HAL_OK;
#else
#if IS_BOARD_F7_FAMILY
    if (F7_canInit(hcan) != HAL_OK) {
        return HAL_ERROR;
    }
#elif IS_BOARD_F0_FAMILY
    if (F0_canInit(hcan) != HAL_OK) {
        return HAL_ERROR;
    }
#else
#error canInit not defined for this board type
#endif

    CAN_Priority0_Queue = xQueueCreate(CAN_P0_QUEUE_LEN, sizeof(CAN_Message));
    if (CAN_Priority0_Queue == NULL) {
        ERROR_PRINT("Failed to create CAN P0 Queue\n");
        return HAL_ERROR;
    }

    CAN_Priority1_Queue = xQueueCreate(CAN_P1_QUEUE_LEN, sizeof(CAN_Message));
    if (CAN_Priority1_Queue == NULL) {
        ERROR_PRINT("Failed to create CAN P1 Queue\n");
        return HAL_ERROR;
    }

    CAN_Priority2_Queue = xQueueCreate(CAN_P2_QUEUE_LEN, sizeof(CAN_Message));
    if (CAN_Priority2_Queue == NULL) {
        ERROR_PRINT("Failed to create CAN P2 Queue\n");
        return HAL_ERROR;
    }

    CAN_Priority3_Queue = xQueueCreate(CAN_P3_QUEUE_LEN, sizeof(CAN_Message));
    if (CAN_Priority3_Queue == NULL) {
        ERROR_PRINT("Failed to create CAN P3 Queue\n");
        return HAL_ERROR;
    }

    uint32_t maxSemCount = CAN_P0_QUEUE_LEN + CAN_P1_QUEUE_LEN + CAN_P2_QUEUE_LEN + CAN_P3_QUEUE_LEN;
    CAN_Msg_Semaphore = xSemaphoreCreateCounting(maxSemCount, 0);
    if (CAN_Msg_Semaphore == NULL) {
        ERROR_PRINT("Failed to create CAN semaphore\n");
        return HAL_ERROR;
    }

    return HAL_OK;
#endif
}

HAL_StatusTypeDef canStart(CAN_HandleTypeDef *hcan)
{
#ifdef BOARD_DISABLE_CAN
    return HAL_OK;
#else
#ifdef CHARGER_CAN_HANDLE
    if (hcan == &CHARGER_CAN_HANDLE) {
        if (!chargerCanStarted) {
        canStarted = true;
        return F7_canStart(hcan);
        }
    }
#endif

    if (!canStarted) {
        canStarted = true;
#if IS_BOARD_F7_FAMILY
        return F7_canStart(hcan);
#elif IS_BOARD_F0_FAMILY
        return F0_canStart(hcan);
#else
#error canStart not defined for this board type
#endif
    } else {
        return HAL_OK;
    }
#endif
}

// Are we allowed to send this CAN message now?
// There are 4 priority levels (from high priority (0) to low (3):
// 0: DTC
// 1: Control Messages
// 2: Status Messages
// 6: Debug Messages
// Depending on free mailboxes, can send certain types
HAL_StatusTypeDef canSendCANMessage(uint32_t id, CAN_HandleTypeDef *hcan)
{
    int freeMailboxes = HAL_CAN_GetTxMailboxesFreeLevel(hcan);

    // Look at the priority field of the J1939 style ID
    int priority = (id & 0x1C000000) >> 26;

    if (freeMailboxes == 0) {
        return HAL_ERROR;
    }
	else if (freeMailboxes == 1) {
        if (priority == 0 || priority == 1) {
            // Allow high priority messages to send
            return HAL_OK;
        } else {
            DEBUG_PRINT("Blocked msg id %lu, priority %d with free mbx %d\n",
                        id, priority, freeMailboxes);
            return HAL_ERROR;
        }
    } else if (freeMailboxes == 2) {
        if (priority <= 2) {
            return HAL_OK;
        } else {
            DEBUG_PRINT("Blocked msg id %lu, priority %d with free mbx %d\n",
                        id, priority, freeMailboxes);
            return HAL_ERROR;
        }
    } else {
        return HAL_OK;
    }
}

// There are 4 priority levels (from high priority (0) to low (3):
// 0: DTC
// 1: Control Messages
// 2: Status Messages
// 6: Debug Messages
HAL_StatusTypeDef sendCanMessage(uint32_t id, uint32_t length, uint8_t *data)
{
#ifdef BOARD_DISABLE_CAN
    return HAL_OK;
#else
    if (length > 8) {
        ERROR_PRINT("Attempt to send CAN message longer than 8 bytes\n");
        return HAL_ERROR;
    }

    if (data == NULL) {
        ERROR_PRINT("Null data pointer\n");
        return HAL_ERROR;
    }

    int priority = (id & 0x1C000000) >> 26;
    xQueueHandle sendQueueHandle;

    CAN_Message msg;

    msg.id = id;
    msg.len = length;
    memcpy(&msg.data, data, length);

    switch (priority) {
        case 0:
            sendQueueHandle = CAN_Priority0_Queue;
            break;
        case 1:
            sendQueueHandle = CAN_Priority1_Queue;
            break;
        case 2:
            sendQueueHandle = CAN_Priority2_Queue;
            break;
        case 6:
            sendQueueHandle = CAN_Priority3_Queue;
            break;
        // cascadia motion msgs need to have priority of 3
        case 3:
            sendQueueHandle = CAN_Priority0_Queue;
            break;
        default:
            DEBUG_PRINT("Unknown CAN message priority %d, id 0x%lX\n", priority, id);
            sendQueueHandle = CAN_Priority3_Queue;
            return HAL_ERROR;
    }

    if (xQueueSend(sendQueueHandle, &msg, pdMS_TO_TICKS(CAN_SEND_TIMEOUT_MS))
        != pdTRUE)
    {
        ERROR_PRINT("Failed to send can msg to queue\n");
        return HAL_ERROR;
    }

    if (xSemaphoreGive(CAN_Msg_Semaphore) != pdTRUE)
    {
        ERROR_PRINT("Failed to give CAN msg semaphore\n");
        return HAL_ERROR;
    }

    return HAL_OK;
#endif
}


#ifdef CHARGER_CAN_HANDLE
HAL_StatusTypeDef sendCanMessageCharger(uint32_t id, int length, uint8_t *data)
{
#ifdef BOARD_DISABLE_CAN
    return HAL_OK;
#else
#if IS_BOARD_F7_FAMILY
    return F7_sendCanMessageCharger(id, length, data);
#else
#error Send can message charger not defined for this board type
#endif
#endif
}
#endif

HAL_StatusTypeDef sendCanMessageInternal(uint32_t id, int length, uint8_t *data)
{
    HAL_StatusTypeDef rc;

#if IS_BOARD_F7_FAMILY
    rc = F7_sendCanMessage(id, length, data);
#elif IS_BOARD_F0_FAMILY
    rc = F0_sendCanMessage(id, length, data);
#else
#error Send can message not defined for this board type
#endif
    return rc;
}

HAL_StatusTypeDef sendDTCMessage(uint32_t dtcCode, int severity, uint64_t data)
{
#ifdef BOARD_DISABLE_CAN
    return HAL_OK;
#else
    DTC_Data = (float)data;
    DTC_Severity = (float)severity;
    DTC_CODE = (float)dtcCode;
    return DTC_SEND_FUNCTION();
#endif
}


void canTask(void *pvParameters)
{
#ifndef BOARD_DISABLE_CAN
    CAN_Message msg;
#endif

    while (1) {
#ifdef BOARD_DISABLE_CAN
        vTaskDelay(10000);
#else
        if (xSemaphoreTake(CAN_Msg_Semaphore, portMAX_DELAY) != pdTRUE) {
            ERROR_PRINT("Error taking CAN msg semaphore\n");
            vTaskDelay(10);
            continue;
        }
        
        /*DEBUG_PRINT("Got a CAN message\n");*/

        if (HAL_CAN_GetTxMailboxesFreeLevel(&CAN_HANDLE) == 0) {
            DEBUG_PRINT("All mailboxes full, waiting\n");
            // Give semaphore again, since we haven't sent this message
            if (xSemaphoreGive(CAN_Msg_Semaphore) != pdTRUE)
            {
                ERROR_PRINT("Failed to give CAN msg semaphore\n");
            }
            // Short delay to wait for mailbox to be empty
            vTaskDelay(1);
            continue;
        }

        /*
         * Try receive from each queue in priority order
         * If succesful, send message and start again from top to ensure we
         * send the next highest priority message
         */
        if (xQueueReceive(CAN_Priority0_Queue, &msg, 0) == pdTRUE) {
            //DEBUG_PRINT("Sending msg priority 0\n");
            if (sendCanMessageInternal(msg.id, msg.len, msg.data) != HAL_OK)
            {
                ERROR_PRINT("Failed to send CAN message\n");
            }
            continue;
        }

        if (xQueueReceive(CAN_Priority1_Queue, &msg, 0) == pdTRUE) {
            //DEBUG_PRINT("Sending msg priority 1\n");
            if (sendCanMessageInternal(msg.id, msg.len, msg.data) != HAL_OK)
            {
                ERROR_PRINT("Failed to send CAN message\n");
            }
            continue;
        }

        if (xQueueReceive(CAN_Priority2_Queue, &msg, 0) == pdTRUE) {
            //DEBUG_PRINT("Sending msg priority 2\n");
            if (sendCanMessageInternal(msg.id, msg.len, msg.data) != HAL_OK)
            {
                ERROR_PRINT("Failed to send CAN message\n");
            }
            continue;
        }

        if (xQueueReceive(CAN_Priority3_Queue, &msg, 0) == pdTRUE) {
            //DEBUG_PRINT("Sending msg priority 3\n");
            if (sendCanMessageInternal(msg.id, msg.len, msg.data) != HAL_OK)
            {
                ERROR_PRINT("Failed to send CAN message\n");
            }
            continue;
        }
#endif
    }
}

/*
 *bool sendCanMessageTimeoutMs(const uint16_t id, const uint8_t *data,
 *                             const uint8_t length, const uint32_t timeout)
 *{
 *    uint32_t beginTime = HAL_GetTick();
 *    bool success = false;
 *    while (HAL_GetTick() < beginTime + timeout && !success)
 *    {
 *        success = sendCanMessage(id, data, length);
 *    }
 *    return success;
 *}
 *
 *bool sendCanMessage(const uint16_t id, const uint8_t *data, const uint8_t length)
 *{
 *    if (length > CAN_MAX_BYTE_LEN)
 *        return false;    // Programmer error
 *    if (initStatus != HAL_OK)
 *        return false;
 *    if (canHandle.Instance->MSR == CAN_MCR_RESET)
 *        return false;
 *
 *    HAL_Delay(1);	// Wait for a previous message to flush out HW
 *
 *    CanTxMsgTypeDef txMessage =
 *    {
 *        .StdId = id,
 *        .IDE = CAN_ID_STD,
 *        .RTR = CAN_RTR_DATA,
 *        .DLC = length
 *    };
 *
 *    uint8_t i;
 *    for (i = 0; i < length; i++)
 *        txMessage.Data[i] = data[i];
 *
 *    canHandle.pTxMsg = &txMessage;
 *    HAL_StatusTypeDef status = HAL_CAN_Transmit_IT(&canHandle);
 *
 *    if (status == HAL_OK)
 *    {
 *        return true;
 *    }
 *    else
 *    {
 *        return false;
 *    }
 *}
 */
