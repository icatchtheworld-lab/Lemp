/*
 * app_light_bulb_test.c
 *
 *  Created on: 2026年8月1日
 *      Author: 36315
 */

#include "app.h"
#include "Light_Module/light_module.h"

 void Light_bulb_test(void)
{
     static uint8_t const duty_levels[]=
     {
      0,25,50,75,100
     };
     fsp_err_t err;

     err = light_bulb_init();
     if (FSP_SUCCESS != err)
     {
         app_fatal_error("Light bulb PWM initialization", err);
     }

     printf("Light bulb PWM ready: 20 kHz, duty=0%%.\r\n");

     while (1)
        {
            /*
             * sizeof(duty_levels)得到数组总字节数；
             * 除以单个元素大小后，得到数组元素数量。
             */
            for (uint32_t i = 0U;
                 i < (sizeof(duty_levels) / sizeof(duty_levels[0]));
                 i++)
            {
                err = light_bulb_set_brightness(duty_levels[i]);
                if (FSP_SUCCESS != err)
                {
                    app_fatal_error("Light bulb PWM duty setting", err);
                }

                printf("Light bulb duty: %u%%\r\n",
                       (unsigned int) duty_levels[i]);

                /* 每个亮度保持1秒，便于肉眼观察。 */
                R_BSP_SoftwareDelay(1000U,
                                    BSP_DELAY_UNITS_MILLISECONDS);
            }
        }

}
