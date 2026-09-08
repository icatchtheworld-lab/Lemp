/*
 * light_module.c
 *
 *  Created on: 2026年7月25日
 *      Author: 36315
 */

#include "light_module.h"

#define LIGHT_BULB_MAX_PERCENT    (100U)

static bool s_light_module_initialized;
static bool s_light_bulb_initialized;

fsp_err_t light_module_init(void)
{
    uint32_t wait_ms = 0U;
    fsp_err_t err;
    adc_status_t adc_status =
    {
        .state = ADC_STATE_CALIBRATION_IN_PROGRESS
    };

    if (s_light_module_initialized)
    {
        return FSP_SUCCESS;
    }

    err = R_ADC_B_Open(&g_adc0_ctrl, &g_adc0_cfg);

    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = R_ADC_B_ScanCfg(&g_adc0_ctrl, &g_adc0_scan_cfg);
    if (FSP_SUCCESS != err)
    {
        goto cleanup;
    }

    /*
     * 启动ADC自校准。
     * 校准用于修正ADC内部的增益和偏移误差，完成前不能开始采样。
     */
    err = R_ADC_B_Calibrate(&g_adc0_ctrl, NULL);
    if (FSP_SUCCESS != err)
    {
        goto cleanup;
    }

    while (1)
    {
        /* 查询ADC当前状态。 */
        err = R_ADC_B_StatusGet(&g_adc0_ctrl, &adc_status);
        if (FSP_SUCCESS != err)
        {
            goto cleanup;
        }

        /* ADC进入空闲状态，表示校准过程已经结束。 */
        if (ADC_STATE_IDLE == adc_status.state)
        {
            break;
        }

        /* 校准正常应很快完成，超过1秒认为出现异常。 */
        if (wait_ms >= 1000U)
        {
            err = FSP_ERR_TIMEOUT;
            goto cleanup;
        }

        /* 每次等待1毫秒，避免无休止地高速查询。 */
        R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
        wait_ms++;
    }

    /*
     * StatusGet在校准失败时也可能返回IDLE，
     * 因此还需要确认驱动内部确实进入READY状态。
     */
    if (ADC_B_CONVERTER_STATE_READY != g_adc0_ctrl.adc_state)
    {
        err = FSP_ERR_ABORTED;
        goto cleanup;
    }

    s_light_module_initialized = true;
    return FSP_SUCCESS;

cleanup:
    /* 初始化中途失败时关闭ADC，使后续仍有重新初始化的可能。 */
    (void) R_ADC_B_Close(&g_adc0_ctrl);
    return err;
}

fsp_err_t light_module_read_raw(uint16_t * p_value)
{
    uint32_t wait_count = 0U;
    fsp_err_t status;
    adc_status_t adc_status = {0};

    if (NULL == p_value)
    {
        return FSP_ERR_INVALID_POINTER;
    }

    if (!s_light_module_initialized)
    {
        return FSP_ERR_NOT_OPEN;
    }

    status = R_ADC_B_ScanGroupStart(&g_adc0_ctrl, ADC_GROUP_MASK_0);
    if (FSP_SUCCESS != status)
    {
        return status;
    }

    do
    {
        status = R_ADC_B_StatusGet(&g_adc0_ctrl, &adc_status);
        if (FSP_SUCCESS != status)
        {
            (void) R_ADC_B_ScanStop(&g_adc0_ctrl);
            return status;
        }

        if (ADC_STATE_IDLE == adc_status.state)
        {
            break;
        }

        /* 每次等待10 us，避免在ADC转换期间高速空转。 */
        R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MICROSECONDS);
        wait_count++;
    } while (wait_count < 100U);

    if (ADC_STATE_IDLE != adc_status.state)
    {
        (void) R_ADC_B_ScanStop(&g_adc0_ctrl);
        return FSP_ERR_TIMEOUT;
    }

    return R_ADC_B_Read(&g_adc0_ctrl, ADC_CHANNEL_1, p_value);
}

fsp_err_t light_bulb_init(void)
{
    fsp_err_t err;

    if (s_light_bulb_initialized)
    {
        return FSP_SUCCESS;
    }

    err = R_GPT_Open(&g_light_bulb_pwm_ctrl, &g_light_bulb_pwm_cfg);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    /* XML默认占空比为50%，启动前先改为0%，防止灯泡上电闪亮。 */
    err = R_GPT_DutyCycleSet(&g_light_bulb_pwm_ctrl, 0U, GPT_IO_PIN_GTIOCA);
    if (FSP_SUCCESS != err)
    {
        (void) R_GPT_Close(&g_light_bulb_pwm_ctrl);
        return err;
    }

    err = R_GPT_Start(&g_light_bulb_pwm_ctrl);
    if (FSP_SUCCESS != err)
    {
        (void) R_GPT_Close(&g_light_bulb_pwm_ctrl);
        return err;
    }

    s_light_bulb_initialized = true;
    return FSP_SUCCESS;
}

fsp_err_t light_bulb_set_brightness(uint8_t brightness_percent)
{
    uint32_t duty_counts;

    if (brightness_percent > LIGHT_BULB_MAX_PERCENT)
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    if (!s_light_bulb_initialized)
    {
        return FSP_ERR_NOT_OPEN;
    }

    /* 将0～100%的亮度换算为一个PWM周期内的高电平计数值。 */
    duty_counts = (uint32_t) (((uint64_t) g_light_bulb_pwm_cfg.period_counts * brightness_percent) /
                              LIGHT_BULB_MAX_PERCENT);

    return R_GPT_DutyCycleSet(&g_light_bulb_pwm_ctrl,
                              duty_counts,
                              GPT_IO_PIN_GTIOCA);
}
