#include "mbed.h"
#include <cstdint>
InterruptIn alarm(BUTTON1);
InterruptIn alarm1(PC_0,PullUp);
volatile int _count = 0;

#define addFlag (1UL << 0) // EventFlags參數值
#define subFlag (1UL << 1) //參數兩個Flag要設不一樣
EventFlags ef_call;

void trigger_isr()
{
    alarm.disable_irq();
    _count++;
    ef_call.set(addFlag);
}
void downtrigger_isr()
{
    alarm1.disable_irq();
    _count--;
    ef_call.set(subFlag);
}
void thread_function()
{
    uint32_t flags_read = 0;
     while(1)
     {
         flags_read = ef_call.wait_any(addFlag | subFlag);
         if (flags_read & addFlag) 
         {
            printf("Count: %d\n", _count);
         }
         if (flags_read & subFlag)
         {
             printf("Count: %d\n",_count);
         }
         ThisThread::sleep_for(500ms);
     }
}
int main()
{
    Thread thread;
    alarm.rise(&trigger_isr);//讓trigger和 Buton1有連結
    alarm1.rise(&downtrigger_isr);
    thread.start(thread_function);
    while(1)
    {
        alarm.enable_irq();//觸發中斷請求
        alarm1.enable_irq();
        ThisThread::sleep_for(500ms); 
    }
}