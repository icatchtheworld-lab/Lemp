/*
 * drv_gpt_timer.c
 *
 *  Created on: 2026年7月17日
 *      Author: 36315
 */


/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "drv_gpt_timer.h"
#include "Middlewares/lvgl/lvgl.h"

#include <stdio.h>

/**********************************************************************************************************************
 * Macro definitions
 **********************************************************************************************************************/


/**********************************************************************************************************************
 * Typedef definitions
 **********************************************************************************************************************/


/***********************************************************************************************************************
 * Private function prototypes
 **********************************************************************************************************************/
static fsp_err_t gpt_timer_init(gpt_instance_ctrl_t * p_timer_ctrl, const timer_cfg_t * p_timer_cfg);

/***********************************************************************************************************************
 * Private global variables
 **********************************************************************************************************************/
static volatile uint32_t s_system_time_ms = 0U;


/***********************************************************************************************************************
 * Functions
 **********************************************************************************************************************/

fsp_err_t drv_gpt_timer_init(void)
{
    fsp_err_t err;

    s_system_time_ms = 0U;

    /* Start GPT timer to 'Give' Semaphore periodically at 1sec for semaphore_task */
    err = gpt_timer_init(&g_timer0_ctrl, &g_timer0_cfg );
    if(FSP_SUCCESS != err)
    {
        printf ("%s %d\r\n", __FUNCTION__, __LINE__);
    }

    return err;
}

void periodic_timer0_cb(timer_callback_args_t *p_args)
{
    FSP_PARAMETER_NOT_USED(p_args);

    /* GPT0每1 ms进入一次中断，同时为应用调度和LVGL提供统一时间基准。 */
    s_system_time_ms++;
    lv_tick_inc(1);

}

uint32_t drv_gpt_timer_get_ms(void)
{
    /* 32位计数自然回绕，调用方使用无符号减法即可正确计算时间差。 */
    return s_system_time_ms;
}

static fsp_err_t gpt_timer_init(gpt_instance_ctrl_t * p_timer_ctrl, const timer_cfg_t * p_timer_cfg)
{
    fsp_err_t fsp_err = FSP_SUCCESS;

    /* Open GPT timer instance */
    fsp_err = R_GPT_Open(p_timer_ctrl, p_timer_cfg);
    /* Handle error */
    if ( FSP_SUCCESS != fsp_err )
    {
        /* Print out in case of error */
        //APP_ERR_PRINT ("\r\nGPT Timer open API failed\r\n");
        return fsp_err;
    }


    /* Start GPT Timer instance */
    fsp_err = R_GPT_Start(p_timer_ctrl);
    /* Handle error */
    if (FSP_SUCCESS != fsp_err)
    {
        /* Close timer if failed to start */
        if ( FSP_SUCCESS  != R_GPT_Close(p_timer_ctrl) )
        {
            /* Print out in case of error */
            //APP_ERR_PRINT ("\r\nGPT Timer Close API failed\r\n");
        }

       // APP_ERR_PRINT ("\r\nGPT Timer Start API failed\r\n");
        return fsp_err;
    }

    return fsp_err;
}


/***********************************************************************************************************************
 * Private Functions
 **********************************************************************************************************************/
