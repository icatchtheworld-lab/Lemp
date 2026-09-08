/*********************************************************************************************************************
* RA8P1KFLCAC Opensourec Library 即（RA8P1KFLCAC 开源库）是一个基于官方 SDK 接口的第三方开源库
* Copyright (c) 2022 SEEKFREE 逐飞科技
* 
* 本文件是 RA8P1KFLCAC 开源库的一部分
* 
* RA8P1KFLCAC 开源库 是免费软件
* 您可以根据自由软件基金会发布的 GPL（GNU General Public License，即 GNU通用公共许可证）的条款
* 即 GPL 的第3版（即 GPL3.0）或（您选择的）任何后来的版本，重新发布和/或修改它
* 
* 本开源库的发布是希望它能发挥作用，但并未对其作任何的保证
* 甚至没有隐含的适销性或适合特定用途的保证
* 更多细节请参见 GPL
* 
* 您应该在收到本开源库的同时收到一份 GPL 的副本
* 如果没有，请参阅<https://www.gnu.org/licenses/>
* 
* 额外注明：
* 本开源库使用 GPL3.0 开源许可证协议 以上许可申明为译文版本
* 许可申明英文版在 libraries/doc 文件夹下的 GPL3_permission_statement.txt 文件中
* 许可证副本在 libraries 文件夹下 即该文件夹下的 LICENSE 文件
* 欢迎各位使用并传播本程序 但修改内容时必须保留逐飞科技的版权声明（即本声明）
* 
* 文件名称          zf_device_scc8660
* 公司名称          成都逐飞科技有限公司
* 版本信息          查看 libraries/doc 文件夹内 version 文件 版本说明
* 开发环境          Renesas e² studio Version: 2025-12 (25.12.0)
* 适用平台          RA8P1KFLCAC
* 店铺链接          https://seekfree.taobao.com/
* 
* 修改记录
* 日期              作者                备注
* 2026-05-19        SeekFree            first version
********************************************************************************************************************/

#include "zf_driver/zf_driver_soft_iic.h"
#include "zf_device/zf_device_config.h"

#include "zf_device_scc8660.h"


// Hardware Interface
#define SCC8660_SOFTIIC_SCL     BSP_IO_PORT_04_PIN_10
#define SCC8660_SOFTIIC_SDA     BSP_IO_PORT_04_PIN_09

#define SCC8660_DELAY_MS(x)     R_BSP_SoftwareDelay(x, BSP_DELAY_UNITS_MILLISECONDS)

vuint8 scc8660_finish_flag;
uint16 scc8660_image[SCC8660_H][SCC8660_W]
    BSP_PLACE_IN_SECTION(".ram_nocache") BSP_ALIGN_VARIABLE(32);
/* 为0时CEU停止采集；摄像头芯片本身仍保持已经完成的寄存器配置。 */
static volatile uint8_t scc8660_capture_enabled;

#define SCC8660_PIN_PROBE_SAMPLES (2000000UL)
#define SCC8660_REUSE_PCLK_MIN_EDGES (1000UL)

static uint8_t scc8660_existing_stream_probe (void)
{
    const uint16_t vsync_mask = (uint16_t) (1U << 2U); /* PB02 */
    const uint16_t pclk_mask  = (uint16_t) (1U << 4U); /* PB04 */
    uint16_t previous = R_PORT11->PIDR;
    uint32_t vsync_edges = 0U;
    uint32_t pclk_edges = 0U;

    for (uint32_t sample = 0U; sample < SCC8660_PIN_PROBE_SAMPLES; sample++)
    {
        uint16_t current = R_PORT11->PIDR;
        uint16_t changed = (uint16_t) (current ^ previous);

        if (0U != (changed & vsync_mask))
        {
            vsync_edges++;
        }
        if (0U != (changed & pclk_mask))
        {
            pclk_edges++;
        }
        previous = current;
    }

    return (uint8_t) ((vsync_edges > 0U) && (pclk_edges >= SCC8660_REUSE_PCLK_MIN_EDGES));
}

// 需要配置到摄像头的数据 不允许在这修改参数
static int16 scc8660_set_confing_buffer[SCC8660_CONFIG_FINISH][2]=
{
    {SCC8660_INIT,              0},                                             // 摄像头开始初始化

    {SCC8660_AUTO_EXP,          SCC8660_AUTO_EXP_DEF},                          // 自动曝光
    {SCC8660_BRIGHT,            SCC8660_BRIGHT_DEF},                            // 亮度设置
    {SCC8660_FPS,               SCC8660_FPS_DEF},                               // 图像帧率
    {SCC8660_SET_COL,           SCC8660_W},                                     // 图像列数
    {SCC8660_SET_ROW,           SCC8660_H},                                     // 图像行数
    {SCC8660_PCLK_DIV,          SCC8660_PCLK_DIV_DEF},                          // PCLK分频系数
    {SCC8660_PCLK_MODE,         SCC8660_PCLK_MODE_DEF},                         // PCLK模式
    {SCC8660_COLOR_MODE,        SCC8660_COLOR_MODE_DEF},                        // 图像色彩模式
    {SCC8660_DATA_FORMAT,       SCC8660_DATA_FORMAT_DEF},                       // 输出数据格式
#if SCC8660_IS_WB_AUTO
    {SCC8660_WB_R,              0},                                             // 自动白平衡
    {SCC8660_WB_G,              0},                                             // 自动白平衡
    {SCC8660_WB_B,              0},                                             // 自动白平衡
#else   
    {SCC8660_WB_R,              SCC8660_MANUAL_WB_R},                           // 自动白平衡
    {SCC8660_WB_G,              SCC8660_MANUAL_WB_G},                           // 自动白平衡
    {SCC8660_WB_B,              SCC8660_MANUAL_WB_B},                           // 自动白平衡
#endif
};

//-------------------------------------------------------------------------------------------------------------------
// 函数简介     单独设置图像亮度
// 参数说明     data            需要设置的亮度值
// 返回参数     uint8           1-失败 0-成功
// 使用示例     scc8660_set_bright(data);                                       // 通过该函数设置的参数，不会被51单片机保存
// 备注信息     调用该函数前请先初始化摄像头配置串口
//-------------------------------------------------------------------------------------------------------------------
uint8 scc8660_set_brightness (uint16 data)
{
    return scc8660_sccb_set_brightness(data);
}

//-------------------------------------------------------------------------------------------------------------------
// 函数简介     单独设置白平衡
// 参数说明     data            需要设置的白平衡 可选参数为：[0,0x65-0xa0] 0：关闭手动白平衡，启用自动白平衡    其他：手动白平衡 手动白平衡时 参数范围0x65-0xa0
// 返回参数     uint8           1-失败 0-成功
// 使用示例     scc8660_set_white_balance(data);                // 调用该函数前请先初始化摄像头配置串口
// 备注信息     通过该函数设置的参数，不会被51单片机保存
//-------------------------------------------------------------------------------------------------------------------
uint8 scc8660_set_white_balance (uint16 wb_r, uint16 wb_g, uint16 wb_b)
{
    (void)wb_g;
    (void)wb_b;
    return scc8660_sccb_set_manual_wb(wb_r);
}

//-------------------------------------------------------------------------------------------------------------------
// 函数简介     对摄像头内部寄存器进行写操作
// 参数说明     addr            摄像头内部寄存器地址
// 参数说明     data            需要写入的数据
// 返回参数     uint8           1-失败 0-成功
// 使用示例     scc8660_set_reg(addr, data);                    // 调用该函数前请先初始化串口
// 备注信息     
//-------------------------------------------------------------------------------------------------------------------
uint8 scc8660_set_reg (uint8 addr, uint16 data)
{
    return scc8660_sccb_set_reg(addr, data);
}

//-------------------------------------------------------------------------------------------------------------------
//  @brief      SCC8660摄像头采集完成中断函数
//  @param      NULL
//  @return     void					
//  @since      v1.0
//  Sample usage:	
//  @note       该函数由isr.c中的CSI_IRQHandler函数调用
//-------------------------------------------------------------------------------------------------------------------
void g_ceu0_user_callback (capture_callback_args_t * p_args)
{
    if (0U == (p_args->event & CEU_EVENT_FRAME_END))
    {
        return;
    }

    scc8660_finish_flag = 1;
    if (0U != scc8660_capture_enabled)
    {
        (void) scc8660_capture_start();
    }
}

fsp_err_t scc8660_capture_start (void)
{
    return g_ceu0.p_api->captureStart(g_ceu0.p_ctrl, (uint8_t *)scc8660_image);
}

/**
 * @brief 暂停CEU图像采集，释放帧搬运和帧结束中断占用。
 */
fsp_err_t scc8660_capture_stop (void)
{
    fsp_err_t err;

    /* 先阻止帧结束回调再次启动下一帧，再关闭当前CEU采集。 */
    scc8660_capture_enabled = 0U;
    scc8660_finish_flag = 0U;
    err = g_ceu0.p_api->close(g_ceu0.p_ctrl);
    if (FSP_ERR_NOT_OPEN == err)
    {
        return FSP_SUCCESS;
    }

    if (FSP_SUCCESS != err)
    {
        /* 关闭失败时恢复自动续帧标志，保持软件状态与硬件一致。 */
        scc8660_capture_enabled = 1U;
    }
    return err;
}

/**
 * @brief 重新打开CEU并从下一次场同步开始采集。
 */
fsp_err_t scc8660_capture_resume (void)
{
    fsp_err_t err;

    if (0U != scc8660_capture_enabled)
    {
        return FSP_SUCCESS;
    }

    err = g_ceu0.p_api->open(g_ceu0.p_ctrl, g_ceu0.p_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        return err;
    }

    scc8660_finish_flag = 0U;
    scc8660_capture_enabled = 1U;
    err = scc8660_capture_start();
    if (FSP_SUCCESS != err)
    {
        scc8660_capture_enabled = 0U;
        (void) g_ceu0.p_api->close(g_ceu0.p_ctrl);
    }
    return err;
}

//-------------------------------------------------------------------------------------------------------------------
// 函数简介     SCC8660 摄像头初始化
// 参数说明     void
// 返回参数     uint8           1-失败 0-成功
// 使用示例     scc8660_init();
// 备注信息     
//-------------------------------------------------------------------------------------------------------------------
uint8 scc8660_init (void)
{
    uint8 return_state = 1;
    do
    {
        uint8_t existing_stream = scc8660_existing_stream_probe();

        if (0U != existing_stream)
        {
            /* The MCU reset input does not remove power from the camera.
             * If frame timing is already present, replaying the closed-source
             * SCCB configuration can stop VSYNC.  Reattach the freshly reset
             * CEU to the existing stream instead. */
            fsp_err_t err = g_ceu0.p_api->open(g_ceu0.p_ctrl, g_ceu0.p_cfg);
            if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
            {
                return_state = 2U;
                break;
            }

            scc8660_capture_enabled = 1U;
            err = scc8660_capture_start();
            if (FSP_SUCCESS != err)
            {
                scc8660_capture_enabled = 0U;
                return_state = 3U;
                break;
            }

            return_state = 0U;
            break;
        }

        soft_iic_info_struct scc8660_iic_struct;
        soft_iic_init(&scc8660_iic_struct, 0, SCC8660_COF_IIC_DELAY, SCC8660_SOFTIIC_SCL, SCC8660_SOFTIIC_SDA);
        SCC8660_DELAY_MS(200);

        if(!scc8660_sccb_check_id(&scc8660_iic_struct))
        {
            /* Match the official driver: the SCC8660 command processor is
             * configured once, then the CEU is started.  A nonzero return is
             * a readback mismatch counter, not a request to replay the whole
             * closed-source state machine. */
            (void) scc8660_sccb_set_config(scc8660_set_confing_buffer);

            fsp_err_t err = g_ceu0.p_api->open(g_ceu0.p_ctrl, g_ceu0.p_cfg);
            if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
            {
                return_state = 2U;
                break;
            }

            scc8660_capture_enabled = 1U;
            err = scc8660_capture_start();
            if (FSP_SUCCESS != err)
            {
                scc8660_capture_enabled = 0U;
                return_state = 3U;
                break;
            }

            return_state = 0;
            break;
        }
    }while(0);

    return return_state;
}
