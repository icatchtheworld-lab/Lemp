/*
 * app_adc_light_test.c
 *
 *  Created on: 2026年7月26日
 *      Author: 36315
 */


#include "app.h"
#include "Light_Module/light_module.h"

void app_adc_light_test(void)
{
        fsp_err_t err;
       uint16_t light_raw = 0U;
       err=light_module_init();
       if(FSP_SUCCESS != err)
       {
           /* 输出FSP错误码，便于判断初始化失败的位置。 */
           printf("Light ADC init failed: %d\r\n", (int) err);

           /* 初始化失败后停止运行，避免继续使用未准备好的ADC。 */
           while (1)
           {
           }
       }
       printf("Light ADC ready.\r\n");
       while (1)
        {
           /* 触发一次ADC扫描并读取AN001的原始值。 */
               err = light_module_read_raw(&light_raw);

               if (FSP_SUCCESS == err)
               {
                   /* uint16_t会参与整数提升，转换为unsigned int后配合%u输出。 */
                   printf("Light ADC raw: %u\r\n", (unsigned int) light_raw);
               }
               else
               {
                   /* 输出错误码，便于定位采样失败原因。 */
                   printf("Light ADC read failed: %d\r\n", (int) err);
               }


        }
}

