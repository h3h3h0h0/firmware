#include "debug.h"
#include "bsp.h"
#include "stdio.h"
#include "string.h"
#include "FreeRTOS_CLI.h"
#include "task.h"
#include "userCan.h"
#ifndef DISABLE_CAN_FEATURES
#include "canHeartbeat.h"
#endif // DISABLE_CAN_FEATURES

// Send a CLI string to the uart to be printed. Only for use by the CLI
// buf must be of length PRINT_QUEUE_STRING_SIZE (this is always true for CLI
// output buffer)
#define CONSOLE_SEND(buf) \
    do { \
        xQueueSendFromISR(printQueue, buf, NULL); \
        vTaskDelay(1); \
    } while (0)

// Buffer to receive uart characters (1 byte)
uint8_t uartDMA_rxBuffer = '\000';

uint8_t isUartOverCanEnabled = 0;

QueueHandle_t printQueue;
QueueHandle_t uartRxQueue;

// Note: printf should only be used for printing before RTOS starts, and error
// cases where rtos has probably failed. (If used after rtos starts, it may
// cause errors in calling non-reentrant hal functions)
int _write(int file, char* data, int len) {
    HAL_UART_Transmit(&DEBUG_UART_HANDLE, (uint8_t*)data, len, UART_PRINT_TIMEOUT);
    return len;
}

HAL_StatusTypeDef uartStartReceiving(UART_HandleTypeDef *huart) {
    __HAL_UART_FLUSH_DRREGISTER(huart); // Clear the buffer to prevent overrun
    return HAL_UART_Receive_DMA(huart, &uartDMA_rxBuffer, 1);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    BaseType_t xHigherPriorityTaskWoken;

    xHigherPriorityTaskWoken = pdFALSE;

    xQueueSendFromISR( uartRxQueue, &uartDMA_rxBuffer, &xHigherPriorityTaskWoken );

    if( xHigherPriorityTaskWoken )
    {
        portYIELD();
    }
}

#define INPUT_BUFFER_SIZE (100)
#define OUTPUT_BUFFER_SIZE (configCOMMAND_INT_MAX_OUTPUT_SIZE)
static char rxString[INPUT_BUFFER_SIZE];
static int rxIndex = 0;
#ifdef BOARD_NAME
    __weak char PS1[] = STRINGIZE(BOARD_NAME) " > ";
#else
    __weak char PS1[] = "CLI > "; // Can override this in project to change PS1
#endif

void cliTask(void *pvParameters)
{
    BaseType_t xMoreDataToFollow;
    char *outputBuffer = FreeRTOS_CLIGetOutputBuffer();
    CONSOLE_PRINT("CLI Started. Enter command, or help for more info\n");
    CONSOLE_PRINT("%s", PS1);

    while (1) {
        char rxBuffer;
        if (xQueueReceive(uartRxQueue, &rxBuffer, portMAX_DELAY) != pdTRUE) {
            ERROR_PRINT("Error Receiving from UART Rx Queue\n");
            handleError();
        }

        if( rxBuffer == '\n' )
        {
            /* A newline character was received, so the input command string is
            complete and can be processed.  Transmit a line separator, just to
            make the output easier to read. */
            CONSOLE_PRINT("\r\n");

            /* The command interpreter is called repeatedly until it returns
            pdFALSE.  See the "Implementing a command" documentation for an
            exaplanation of why this is. */
            do
            {
                /* Send the command string to the command interpreter.  Any
                output generated by the command interpreter will be placed in the
                outputBuffer buffer. */
                xMoreDataToFollow = FreeRTOS_CLIProcessCommand
                              (
                                  rxString,   /* The command string.*/
                                  outputBuffer,  /* The output buffer. */
                                  OUTPUT_BUFFER_SIZE/* The size of the output buffer. */
                              );

                /* Write the output generated by the command interpreter to the
                console. */
                CONSOLE_SEND(outputBuffer);

            } while( xMoreDataToFollow != pdFALSE );

            CONSOLE_PRINT("%s", PS1);

            /* All the strings generated by the input command have been sent.
            Processing of the command is complete.  Clear the input string ready
            to receive the next command. */
            rxIndex = 0;
            memset( rxString, 0x00, INPUT_BUFFER_SIZE );
            memset( outputBuffer, 0x00, OUTPUT_BUFFER_SIZE );
        }
        else
        {
            /* The if() clause performs the processing after a newline character
            is received.  This else clause performs the processing if any other
            character is received. */

            if( rxBuffer == '\r' )
            {
                /* Ignore carriage returns. */
            }
            else if( rxBuffer == '\b' )
            {
                /* Backspace was pressed.  Erase the last character in the input
                buffer - if there are any. */
                if( rxIndex > 0 )
                {
                    rxIndex--;
                    rxString[ rxIndex ] = '\0';
                }
            }
            else
            {
                /* A character was entered.  It was not a new line, backspace
                or carriage return, so it is accepted as part of the input and
                placed into the input buffer.  When a \n is entered the complete
                string will be passed to the command interpreter. */
                if( rxIndex < INPUT_BUFFER_SIZE )
                {
                    rxString[ rxIndex ] = rxBuffer;
                    rxIndex++;
                } else {
                    ERROR_PRINT("Rx string buffer overflow\n");
                }
            }
        }
    }
}

/* 
 * Common CLI commands
 * For help on implementing more commands, see:
 * https://www.freertos.org/FreeRTOS-Plus/FreeRTOS_Plus_CLI/FreeRTOS_Plus_CLI_Implementing_A_Command.shtml
 */
BaseType_t heapUsageCommand(char *writeBuffer, size_t writeBufferLength,
                       const char *commandString)
{
    BaseType_t paramLen;
    const char * param = FreeRTOS_CLIGetParameter(commandString, 1, &paramLen);

    if (STR_EQ(param, "cur", paramLen)) {
        COMMAND_OUTPUT("Current free heap (bytes): %d\n", xPortGetFreeHeapSize()); 
    } else if (STR_EQ(param, "min", paramLen)) {
        COMMAND_OUTPUT("Minimum free heap (bytes): %d\n", xPortGetMinimumEverFreeHeapSize()); 
    } else {
        COMMAND_OUTPUT("Unknown parameter\n");
    }

    return pdFALSE;
}

static const CLI_Command_Definition_t heapCommandDefinition =
{
    "heap",
    "heap <min|cur>:\r\n  Outputs the <min|cur> free heap space\r\n",
    heapUsageCommand,
    1 /* Number of parameters */
};

#ifndef DISABLE_CAN_FEATURES

BaseType_t generalHeartbeatCommand(char *writeBuffer, size_t writeBufferLength,
                       const char *commandString)
{
#if !BOARD_IS_WSB(BOARD_ID)
    BaseType_t paramLen;
    const char * param = FreeRTOS_CLIGetParameter(commandString, 1, &paramLen);

    if (STR_EQ(param, "on", paramLen)) {
        COMMAND_OUTPUT("Turning can heartbeat on (Boards need to be turned on as well)\n");
        heartbeatEnabled = true;
    } else if (STR_EQ(param, "off", paramLen)) {
        COMMAND_OUTPUT("Turning can heartbeat off\n");
        heartbeatEnabled = false;
    } else {
        COMMAND_OUTPUT("Unknown parameter\n");
    }
#else
    COMMAND_OUTPUT("WSB don't have a heartbeat\n");
#endif

    return pdFALSE;
}

static const CLI_Command_Definition_t generalHeartbeatCommandDefinition =
{
    "heartbeat",
    "heartbeat <on|off>:\r\n  Turn on/off CAN heartbeat\r\n",
    generalHeartbeatCommand,
    1 /* Number of parameters */
};

BaseType_t boardHeartbeatCommand(char *writeBuffer, size_t writeBufferLength,
                       const char *commandString)
{
#if !BOARD_IS_WSB(BOARD_ID)
    BaseType_t paramLen;
    const char * onOffParam = FreeRTOS_CLIGetParameter(commandString, 2, &paramLen);

    bool onOff = false;
    if (STR_EQ(onOffParam, "on", paramLen)) {
        onOff = true;
    } else if (STR_EQ(onOffParam, "off", paramLen)) {
        onOff = false;
    } else {
        COMMAND_OUTPUT("Unknown parameter\n");
        return pdFALSE;
    }

    const char * boardParam = FreeRTOS_CLIGetParameter(commandString, 1, &paramLen);

    if (STR_EQ(boardParam, "BMU", paramLen)) {
        COMMAND_OUTPUT("Turning can heartbeat %s for BMU\n", onOff?"on":"off");
        BMU_heartbeatEnabled = onOff;
    } else if (STR_EQ(boardParam, "DCU", paramLen)) {
        COMMAND_OUTPUT("Turning can heartbeat %s for DCU\n", onOff?"on":"off");
        DCU_heartbeatEnabled = onOff;
    } else if (STR_EQ(boardParam, "PDU", paramLen)) {
        COMMAND_OUTPUT("Turning can heartbeat %s for PDU\n", onOff?"on":"off");
        PDU_heartbeatEnabled = onOff;
    } else if (STR_EQ(boardParam, "VCU_F7", paramLen)) {
        COMMAND_OUTPUT("Turning can heartbeat %s for VCU_F7\n", onOff?"on":"off");
        VCU_F7_heartbeatEnabled = onOff;
    } else {
        COMMAND_OUTPUT("Unknown parameter\n");
    }
#else
    COMMAND_OUTPUT("WSB don't have a heartbeat\n");
#endif

    return pdFALSE;
}

static const CLI_Command_Definition_t boardHeartbeatCommandDefinition =
{
    "heartbeatForBoard",
    "heartbeatForBoard <BMU|PDU|DCU|VCU_F7> <on|off>:\r\n  Turn on/off CAN heartbeat for a board\r\n",
    boardHeartbeatCommand,
    2 /* Number of parameters */
};

BaseType_t boardHeartbeatInfoCommand(char *writeBuffer, size_t writeBufferLength,
                       const char *commandString)
{
#if !BOARD_IS_WSB(BOARD_ID)
    printHeartbeatStatus();
#else 
	COMMAND_OUTPUT("WSB does not have support for reading heartbeats");
#endif
    return pdFALSE;
}

static const CLI_Command_Definition_t boardHeartbeatInfoCommandDefinition =
{
    "heartbeatInfo",
    "heartbeatInfo:\r\n  Display heartbeat info\r\n",
    boardHeartbeatInfoCommand,
    0 /* Number of parameters */
};

#endif // DISABLE_CAN_FEATURES

#define TASK_LIST_NUM_BYTES_PER_TASK 50
char *taskListBuffer = NULL; // A buffer to store taskList string in,
                            // should be on first call, and never freed
BaseType_t taskListCommand(char *writeBuffer, size_t writeBufferLength,
                       const char *commandString)
{
    static char *currentStringPointer = NULL;

    if (taskListBuffer == NULL) {
        UBaseType_t numTasks = uxTaskGetNumberOfTasks();
        taskListBuffer = (char *)pvPortMalloc(numTasks*TASK_LIST_NUM_BYTES_PER_TASK);
        if (taskListBuffer == NULL) {
            COMMAND_OUTPUT("Failed to malloc taskListBuffer!\n");
            return pdFALSE;
        }
    }

    if (currentStringPointer == NULL) {
        // We haven't created any output yet, so gather the stats to output
        vTaskList(taskListBuffer);

        // Init string pointer
        currentStringPointer = taskListBuffer;

        // Output the Column headers on the first call
        COMMAND_OUTPUT("Name\tState\tPriority\tFreeStack (min)\tNum\r\n");
        return pdTRUE;
    }

    int charWritten = snprintf(writeBuffer, writeBufferLength, "%s", currentStringPointer);

    if (charWritten < writeBufferLength) {
        // All the string has been written
        currentStringPointer = NULL;
        return pdFALSE;
    } else {
        // Only part of the string was written, advance pointer by write buffer
        // length, subtracting one for the null terminator
        currentStringPointer += (writeBufferLength - 1);
        return pdTRUE;
    }
}

static const CLI_Command_Definition_t taskListCommandDefinition =
{
    "taskList",
    "taskList:\r\n  Outputs the freeRTOS task list and stats\r\n",
    taskListCommand,
    0 /* Number of parameters */
};

#define STATS_LIST_NUM_BYTES_PER_TASK 50
char *statsListBuffer = NULL; // A buffer to store taskList string in,
                            // should be on first call, and never freed
BaseType_t statsListCommand(char *writeBuffer, size_t writeBufferLength,
                       const char *commandString)
{
    static char *currentStringPointer = NULL;

    if (statsListBuffer == NULL) {
        UBaseType_t numTasks = uxTaskGetNumberOfTasks();
        statsListBuffer = (char *)pvPortMalloc(numTasks*STATS_LIST_NUM_BYTES_PER_TASK);
        if (statsListBuffer == NULL) {
            COMMAND_OUTPUT("Failed to malloc statsListBuffer!\n");
            return pdFALSE;
        }
    }

    if (currentStringPointer == NULL) {
        // We haven't created any output yet, so gather the stats to output
        vTaskGetRunTimeStats(statsListBuffer);

        // Init string pointer
        currentStringPointer = statsListBuffer;

        // Output the Column headers on the first call
        COMMAND_OUTPUT("Name\tTicks runtime\t\tPercentage runtime\r\n");
        return pdTRUE;
    }

    int charWritten = snprintf(writeBuffer, writeBufferLength, "%s", currentStringPointer);

    if (charWritten < writeBufferLength) {
        // All the string has been written
        currentStringPointer = NULL;
        return pdFALSE;
    } else {
        // Only part of the string was written, advance pointer by write buffer
        // length, subtracting one for the null terminator
        currentStringPointer += (writeBufferLength - 1);
        return pdTRUE;
    }
}

static const CLI_Command_Definition_t statsListCommandDefinition =
{
    "stats",
    "stats:\r\n  Outputs the freeRTOS run time stats\r\n",
    statsListCommand,
    0 /* Number of parameters */
};

BaseType_t resetCLICommand(char *writeBuffer, size_t writeBufferLength,
                       const char *commandString)
{
    NVIC_SystemReset();
    return pdFALSE;
}

static const CLI_Command_Definition_t resetCLICommandDefinition =
{
    "reset",
    "reset:\r\n  Reset the processor\r\n",
    resetCLICommand,
    0 /* Number of parameters */
};

BaseType_t versionCLICommand(char *writeBuffer, size_t writeBufferLength,
                       const char *commandString)
{
	static int versionCLIwriting = 0;
	if(versionCLIwriting == 0){
	    	COMMAND_OUTPUT("Compiled on %s, Branch: %s\r\n", CUR_DATE, CUR_TOP_BRANCH);
		versionCLIwriting = 1;
		vTaskDelay(1);
		return pdTRUE;
	}else if (versionCLIwriting == 1){
    		COMMAND_OUTPUT("Commit Hash: %s\r\n", CUR_HASH);
		versionCLIwriting = 2;
		vTaskDelay(1);
	    	return pdTRUE;
	} else if (versionCLIwriting == 2) {
		COMMAND_OUTPUT("Release Notes: ");
		versionCLIwriting = 3;
		vTaskDelay(1);
		return pdTRUE;
	} else if (versionCLIwriting == 3){
		COMMAND_OUTPUT(RELEASE_NOTES);
		vTaskDelay(1);
		versionCLIwriting = 4;
    		return pdTRUE;
	} else {
		COMMAND_OUTPUT("\n");
		versionCLIwriting = 0;
		return pdFALSE;
	}
}

static const CLI_Command_Definition_t versionCLICommandDefinition =
{
    "version",
    "version:\r\n  Get the current firmware version\r\n",
    versionCLICommand,
    0 /* Number of parameters */
};


HAL_StatusTypeDef debugInit()
{
    printQueue = xQueueCreate(PRINT_QUEUE_LENGTH, PRINT_QUEUE_STRING_SIZE);
    if (!printQueue)
    {
        return HAL_ERROR;
    }

    uartRxQueue = xQueueCreate(UART_RX_QUEUE_LENGTH, 1);
    if (!uartRxQueue)
    {
        return HAL_ERROR;
    }

    /* Register common commands */
    if (FreeRTOS_CLIRegisterCommand(&heapCommandDefinition) != pdPASS) {
        return HAL_ERROR;
    }
    if (FreeRTOS_CLIRegisterCommand(&taskListCommandDefinition) != pdPASS) {
        return HAL_ERROR;
    }
    if (FreeRTOS_CLIRegisterCommand(&statsListCommandDefinition) != pdPASS) {
        return HAL_ERROR;
    }
#ifndef DISABLE_CAN_FEATURES
    if (FreeRTOS_CLIRegisterCommand(&generalHeartbeatCommandDefinition) != pdPASS) {
        return HAL_ERROR;
    }
    if (FreeRTOS_CLIRegisterCommand(&boardHeartbeatCommandDefinition) != pdPASS) {
        return HAL_ERROR;
    }
    if (FreeRTOS_CLIRegisterCommand(&boardHeartbeatInfoCommandDefinition) != pdPASS) {
        return HAL_ERROR;
    }
#endif // DISABLE_CAN_FEATURES
    if (FreeRTOS_CLIRegisterCommand(&versionCLICommandDefinition) != pdPASS) {
        return HAL_ERROR;
    }
    if (FreeRTOS_CLIRegisterCommand(&resetCLICommandDefinition) != pdPASS) {
        return HAL_ERROR;
    }
    return HAL_OK;
}

void printTask(void *pvParameters)
{
    char buffer[PRINT_QUEUE_STRING_SIZE] = {0};

    for ( ;; )
    {
        if (xQueueReceive(printQueue, (uint8_t*) buffer, portMAX_DELAY) == pdTRUE)
        {
            uint64_t len = strlen(buffer);
            HAL_UART_Transmit(&DEBUG_UART_HANDLE, (uint8_t*) buffer, len, UART_PRINT_TIMEOUT);

        #ifndef DISABLE_CAN_FEATURES
            if(isUartOverCanEnabled)
            {
                // send message length
                UartOverCanRX = len;
                sendCAN_UartOverCanRx();
                
                // Send CLI response to the CAN
                const uint16_t chunkLen = 4; // bytes per CAN message
				for(uint16_t a = 0; a < len; a += chunkLen)
				{
                    UartOverCanRX = buffer[a];
                    for (uint16_t i = 0; i < chunkLen && a + i < len; i++)
                    {
                        UartOverCanRX |= buffer[a + i] << (i * 8);
                    }
					sendCAN_UartOverCanRx();
				}
            }
        #endif // DISABLE_CAN_FEATURES
        }
    }
}


#ifdef STATS_TIM_HANDLE
/*
 * Run time stats timer setup
 * A 16 bit timer with clock source APB1 should be configured in cube
 */

uint32_t counterVal = 0; // store the counter value to help protect againts 16 bit overflow
uint16_t lastCounterVal = 0;

// stat timer frequency is this value times tick freqency
#define STAT_TIMER_TICK_FREQUENCY_MULTIPLIER 20

__weak void configureTimerForRunTimeStats(void) {
    // Compute the right prescaler to set timer frequency
    RCC_ClkInitTypeDef      clkconfig;
    uint32_t                uwTimclock, uwAPB1Prescaler = 0U;
    uint32_t                uwPrescalerValue = 0U;
    uint32_t                timerFrequency;
    uint32_t                pFLatency;

    /* Get clock configuration */
    HAL_RCC_GetClockConfig(&clkconfig, &pFLatency);

    /* Get APB1 prescaler */
    uwAPB1Prescaler = clkconfig.APB1CLKDivider;

    /* Compute timer clock */
    if (uwAPB1Prescaler == RCC_HCLK_DIV1) 
    {
    uwTimclock = HAL_RCC_GetPCLK1Freq();
    }
    else
    {
    uwTimclock = 2*HAL_RCC_GetPCLK1Freq();
    }

    timerFrequency = STAT_TIMER_TICK_FREQUENCY_MULTIPLIER * configTICK_RATE_HZ;

    /* Compute the prescaler value to have TIM5 counter clock equal to desired
     * freqeuncy*/
    uwPrescalerValue = (uint32_t) ((uwTimclock / timerFrequency) - 1U);

    __HAL_TIM_SET_PRESCALER(&STATS_TIM_HANDLE, uwPrescalerValue);

    if (HAL_TIM_Base_Start(&STATS_TIM_HANDLE) != HAL_OK)
    {
        DEBUG_PRINT("Failed to start stats timer\n");
        Error_Handler();
    }
}

uint32_t getRunTimeCounterValue()
{
    uint64_t curCounterVal;
    uint16_t val, elapsed;

    portDISABLE_INTERRUPTS();

    val = __HAL_TIM_GET_COUNTER(&STATS_TIM_HANDLE);

    elapsed = val - lastCounterVal;

    counterVal += elapsed;

    lastCounterVal = val;
    curCounterVal = counterVal;

    portENABLE_INTERRUPTS();

    return curCounterVal;
}
#endif
