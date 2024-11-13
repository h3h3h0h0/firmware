#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "stm32f4xx_hal.h"
#include "bsp.h"
#include "cmsis_os.h"
#include "debug.h"
#include "detectWSB.h"
#include "main.h"
#include "multiSensorADC.h"
#include "task.h"
#if BOARD_ID == ID_WSBFR
#include "wsbfr_can.h"
#endif

#define BRAKE_PRESSURE_TASK_PERIOD 1000

/*
 * CubeMX changes:
 *
 * new FreeRTOS task:
 * - name: brakePressureSensorName
 * - Priority: osPriorityLow
 * - Stack Size: 256
 * - Entry Function: BrakePressureSensorTask
 * - Code Generation Options: As external
 */

uint16_t getPressure() {
    //todo: requires a voltage divider circuit
    //signal voltage without voltage divider is 0.5V - 4.5V
    return get_sensor3_V();
}

void BrakePressureSensorTask(void const * argument) {
    deleteWSBTask(WSBFR);
    DEBUG_PRINT("Starting BrakePressureSensorTask\n");

    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (1) {
        uint16_t brake_pressure = getPresure();

        //todo: send can message

        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(BRAKE_PRESSURE_TASK_PERIOD));
    }
}