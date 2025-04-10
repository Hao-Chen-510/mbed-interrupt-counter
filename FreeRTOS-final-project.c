#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"
#include "event_groups.h"
#include "message_buffer.h"

enum State {
    eAvailable,
    eOccupied
};

struct Parking_Lot_t {
    uint32_t ulCarID;
    TickType_t xEntryTime;
    TickType_t xExitTime;
    enum State eState;
};

struct Parking_Area_t {
    struct Parking_Lot_t xLot[24];
    SemaphoreHandle_t xParkingMutex;
    EventGroupHandle_t xParkingEventGroup;
} xArea;

QueueHandle_t xParkingQueue;
MessageBufferHandle_t xParkingMessageBuffer;
TimerHandle_t xProxyTimer;

// Periodic Task: 模擬進場中斷
static void ulEntryInterruptHandler(void);
static void vPeriodicTaskEntry(void* pvParameters)
{
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000 + (rand() % 2000)));
        ulEntryInterruptHandler();
    }
}

// 模擬進場中斷
void ulEntryInterruptHandler(void)
{
    uint32_t ulCarID = (rand() % 10000);
    TickType_t xEntryTime = xTaskGetTickCount();

    struct Parking_Lot_t xLot;
    xLot.ulCarID = ulCarID;
    xLot.xEntryTime = xEntryTime;
    xLot.xExitTime = 0;
    xLot.eState = eOccupied;

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (xQueueSendFromISR(xParkingQueue, &xLot, &xHigherPriorityTaskWoken) != pdPASS)
    {
        printf("[Error] 無法將車輛資訊加入隊列 (Queue Full)\n");
    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// 將車輛資訊放入停車場陣列
void vEntry_Handler(struct Parking_Lot_t* xLot)
{
    if (xSemaphoreTake(xArea.xParkingMutex, portMAX_DELAY) == pdTRUE)
    {
        // 隨機選擇車位號碼
        int i = rand() % 24;  // 產生隨機車位號碼

        // 嘗試找到一個空閒車位
        int attempts = 0;
        while (xArea.xLot[i].eState == eOccupied && attempts < 24) {
            i = (i + 1) % 24;  // 如果車位已佔用，繼續嘗試下一個車位
            attempts++;
        }

        // 如果找到空閒車位，將車輛資訊放入該車位
        if (xArea.xLot[i].eState == eAvailable)
        {
            xArea.xLot[i] = *xLot;  // 將車輛資訊放入停車場陣列
            printf("[Entry] 車位號碼: %d, 車牌號碼: %04u, 進場時間: %u ticks, 狀態: Occupied\n",
                i, xLot->ulCarID, (unsigned int)xLot->xEntryTime);
        }
        else
        {
            printf("[Entry] 無可用車位, 車牌號碼: %04u\n", xLot->ulCarID);
        }

        xSemaphoreGive(xArea.xParkingMutex);
    }
    else
    {
        printf("[Error] 無法取得停車位的互斥鎖\n");
    }
}


// 定時隨機進行繳費
static void vPeriodicTaskTolling(void* pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(10000));
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000 + (rand() % 2000)));
        if (xSemaphoreTake(xArea.xParkingMutex, portMAX_DELAY) == pdTRUE)
        {
            for (int i = 0; i < 24; i++)
            {
                if (xArea.xLot[i].eState == eOccupied)
                {
                    xEventGroupSetBits(xArea.xParkingEventGroup, (1 << i));
                    printf("[Tolling] 繳費完成, 車位號碼: %d\n", i);
                    break;
                }
            }
            xSemaphoreGive(xArea.xParkingMutex);
        }
        else
        {
            printf("[Error] 無法取得停車位的互斥鎖進行繳費\n");
        }
    }
}

// 離場處理任務
void vExitHandler(void* pvParameters)
{
    EventBits_t uxBits;
    for (;;)
    {
        uxBits = xEventGroupWaitBits(xArea.xParkingEventGroup, 0xFFFFFF, pdTRUE, pdFALSE, portMAX_DELAY);
        for (int i = 0; i < 24; i++)
        {
            if (uxBits & (1 << i))
            {
                if (xSemaphoreTake(xArea.xParkingMutex, portMAX_DELAY) == pdTRUE)
                {
                    xArea.xLot[i].xExitTime = xTaskGetTickCount();
                    printf("[Exit] 車位號碼: %d, 車牌號碼: %04u, 進場時間: %u ticks, 離場時間: %u ticks, 狀態: Available\n",
                        i, xArea.xLot[i].ulCarID, (unsigned int)xArea.xLot[i].xEntryTime, (unsigned int)xArea.xLot[i].xExitTime);

                    // 新增訊息發送到 ServerTask
                    char message[128];
                    snprintf(message, sizeof(message),
                        "【離場】車位號碼: %d, 車牌號碼: %04u, 進場時間: %u ticks, 離場時間: %u ticks",
                        i, xArea.xLot[i].ulCarID,
                        (unsigned int)xArea.xLot[i].xEntryTime,
                        (unsigned int)xArea.xLot[i].xExitTime);
                    xMessageBufferSend(xParkingMessageBuffer, message, strlen(message) + 1, portMAX_DELAY);

                    // 清空該車位
                    xArea.xLot[i].eState = eAvailable;
                    xArea.xLot[i].ulCarID = 0;
                    xArea.xLot[i].xEntryTime = 0;
                    xArea.xLot[i].xExitTime = 0;

                    xSemaphoreGive(xArea.xParkingMutex);
                    break;
                }
                else
                {
                    printf("[Error] 無法取得停車位的互斥鎖進行離場操作\n");
                }
            }
        }
    }
}

// Proxy Timer 回呼：處理進場資訊傳給 Server
void prvProxyTimerCallback(TimerHandle_t xTimer)
{
    struct Parking_Lot_t xLot;
    while (xQueueReceive(xParkingQueue, &xLot, 0) == pdPASS)
    {
        vEntry_Handler(&xLot); // 進場處理
        char message[128];
        snprintf(message, sizeof(message),
            "【進場】車牌號碼: %04u, 進場時間: %u ticks",
            xLot.ulCarID, (unsigned int)xLot.xEntryTime);
        xMessageBufferSend(xParkingMessageBuffer, message, strlen(message) + 1, portMAX_DELAY);
    }
}

// Server Task: 負責接收訊息
void vServerTask(void* pvParameters)
{
    char message[128];
    for (;;)
    {
        if (xMessageBufferReceive(xParkingMessageBuffer, message, sizeof(message), portMAX_DELAY) > 0)
        {
            printf("[Server] %s\n", message);
        }
    }
}

int main(void)
{
    for (int i = 0; i < 24; i++)
    {
        xArea.xLot[i].eState = eAvailable;
        xArea.xLot[i].ulCarID = 0;
        xArea.xLot[i].xEntryTime = 0;
        xArea.xLot[i].xExitTime = 0;
    }

    xArea.xParkingMutex = xSemaphoreCreateMutex();
    if (xArea.xParkingMutex == NULL) {
        printf("[Error] 無法創建互斥鎖\n");
        return -1;
    }

    xArea.xParkingEventGroup = xEventGroupCreate();
    if (xArea.xParkingEventGroup == NULL) {
        printf("[Error] 無法創建事件組\n");
        return -1;
    }

    xParkingQueue = xQueueCreate(5, sizeof(struct Parking_Lot_t));
    if (xParkingQueue == NULL) {
        printf("[Error] 無法創建隊列\n");
        return -1;
    }

    xParkingMessageBuffer = xMessageBufferCreate(256);
    if (xParkingMessageBuffer == NULL) {
        printf("[Error] 無法創建訊息緩衝區\n");
        return -1;
    }

    xProxyTimer = xTimerCreate("ProxyTimer", pdMS_TO_TICKS(5000), pdTRUE, 0, prvProxyTimerCallback);
    if (xProxyTimer == NULL || xTimerStart(xProxyTimer, 0) != pdPASS) {
        printf("[Error] 無法創建或啟動計時器\n");
        return -1;
    }

    xTaskCreate(vPeriodicTaskEntry, "EntryTask", configMINIMAL_STACK_SIZE, NULL, 2, NULL);
    xTaskCreate(vPeriodicTaskTolling, "TollingTask", configMINIMAL_STACK_SIZE, NULL, 2, NULL);
    xTaskCreate(vExitHandler, "ExitTask", configMINIMAL_STACK_SIZE, NULL, 2, NULL);
    xTaskCreate(vServerTask, "ServerTask", configMINIMAL_STACK_SIZE, NULL, 2, NULL);

    vTaskStartScheduler();
    for (;;);
}
