// 停車場車輛進出自動管理系統
#include <stdio.h>
#include <stdlib.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"
#include "event_groups.h"
#include "message_buffer.h"

// 定義車位的狀態
enum State
{
    eAvailable,  // 車位未占用
    eOccupied    // 車位已佔用
};

// 定義單個車位資訊
struct Parking_Lot_t
{
    uint32_t ulCarID;           // 車牌號碼(4位數)
    TickType_t xEntryTime;      // 進場時間
    TickType_t xExitTime;       // 離場時間
    enum State eState;          // 車位狀態
};

// 定義一個有24個停車位的停車場結構體
struct Parking_Area_t
{
    struct Parking_Lot_t xLot[24];       // 用來記錄單個車位的使用狀況
    SemaphoreHandle_t xParkingMutex;     // 用來控管 xLots[] 互斥存取
    EventBits_t xParkingEventGroup;      // 以一 Event Group 對應各車位的繳費狀況
} xArea;

// 定義Queue和Timer相關句柄
QueueHandle_t xParkingQueue;
MessageBufferHandle_t xParkingMessageBuffer;
TimerHandle_t xProxyTimer;

// 設計一 Periodic Task vPeriodicTaskEntry，每 1~3 秒產生一 Software Interrupt 
static void ulEntryInterruptHandler(void);
static void vPeriodicTaskEntry(void* pvParameters)
{
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000 + (rand() % 2000))); // 延遲1~3秒
        ulEntryInterruptHandler();
    }
}

// 進場ISR: 處理進場車輛
void ulEntryInterruptHandler(void)
{
    uint32_t ulCarID = (rand() % 10000);                  // 產生四位車牌號碼
    TickType_t xEntryTime = xTaskGetTickCount();          // 獲取目前的Tick Count

    // 封裝車輛資訊
    struct Parking_Lot_t xLot;
    xLot.ulCarID = ulCarID;
    xLot.xEntryTime = xEntryTime;
    xLot.xExitTime = 0; // 進場時離場時間為0
    xLot.eState = eOccupied;

    // 從中斷發送到隊列
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (xQueueSendFromISR(xParkingQueue, &xLot, &xHigherPriorityTaskWoken) != pdPASS)
    {
        printf("[Error] 無法將車輛資訊加入隊列 (Queue Full)\n");
    }

    // 如果需要，執行任務切換
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// vEntry_Handler: 處理進場車輛
void vEntry_Handler(struct Parking_Lot_t* xLot)
{
    if (xSemaphoreTake(xArea.xParkingMutex, portMAX_DELAY) == pdTRUE)
    {
        for (int i = 0; i < 24; i++)
        {
            if (xArea.xLot[i].eState == eAvailable)
            {
                xArea.xLot[i] = *xLot;
                printf("[Entry] 車位號碼: %d, 車牌號碼: %04u, 進場時間: %u ticks, 狀態: Occupied\n",
                    i, xLot->ulCarID, (unsigned int)xLot->xEntryTime);
                xSemaphoreGive(xArea.xParkingMutex);//釋放互斥鎖
                return;
            }
        }
        printf("[Entry] 無可用車位, 車牌號碼: %04u\n", xLot->ulCarID);
        xSemaphoreGive(xArea.xParkingMutex);//釋放互斥鎖
    }
    else
    {
        printf("[Error] 無法取得停車位的互斥鎖\n");
    }
}

// 繳費任務
static void vPeriodicTaskTolling(void* pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(10000)); // 延遲10秒
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000 + (rand() % 2000))); // 每1~3秒隨機延遲
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
            xSemaphoreGive(xArea.xParkingMutex);//釋放互斥鎖
        }
        else
        {
            printf("[Error] 無法取得停車位的互斥鎖進行繳費\n");
        }
    }
}

// 離場任務
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

                    //更新車位資訊
                    xArea.xLot[i].eState = eAvailable;
                    xArea.xLot[i].ulCarID = 0;
                    xArea.xLot[i].xEntryTime = 0;
                    xArea.xLot[i].xExitTime = 0;

                    xSemaphoreGive(xArea.xParkingMutex);//釋放互斥鎖
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

// Auto-Reload Timer 回調函數
void prvProxyTimerCallback(TimerHandle_t xTimer)
{
    struct Parking_Lot_t xLot;
    while (xQueueReceive(xParkingQueue, &xLot, 0) == pdPASS)
    {
        char message[128];
        snprintf(message, sizeof(message), "車位號碼: 未知, 車牌號碼: %04u, 進場時間: %u ticks, 離場時間: %u ticks",
            xLot.ulCarID, (unsigned int)xLot.xEntryTime, (unsigned int)xLot.xExitTime);
        xMessageBufferSend(xParkingMessageBuffer, message, strlen(message) + 1, portMAX_DELAY);
    }
}

// 伺服器任務
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
    // 初始化struct
    for (int i = 0; i < 24; i++)
    {
        xArea.xLot[i].eState = eAvailable;
        xArea.xLot[i].ulCarID = 0;
        xArea.xLot[i].xEntryTime = 0;
        xArea.xLot[i].xExitTime = 0;
    }

    xArea.xParkingMutex = xSemaphoreCreateMutex();
    if (xArea.xParkingMutex == NULL)
    {
        printf("[Error] 無法創建互斥鎖\n");
        return -1;
    }

    xArea.xParkingEventGroup = xEventGroupCreate();
    if (xArea.xParkingEventGroup == NULL)
    {
        printf("[Error] 無法創建事件組\n");
        return -1;
    }

    xParkingQueue = xQueueCreate(5, sizeof(struct Parking_Lot_t));
    if (xParkingQueue == NULL)
    {
        printf("[Error] 無法創建隊列\n");
        return -1;
    }

    xParkingMessageBuffer = xMessageBufferCreate(256);
    if (xParkingMessageBuffer == NULL)
    {
        printf("[Error] 無法創建訊息緩衝區\n");
        return -1;
    }

    xProxyTimer = xTimerCreate("ProxyTimer", pdMS_TO_TICKS(5000), pdTRUE, 0, prvProxyTimerCallback);
    if (xProxyTimer == NULL || xTimerStart(xProxyTimer, 0) != pdPASS)
    {
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