/*
 * drv_i2c_touchpad.c
 *
 *  Created on: 2026年7月15日
 *      Author: 36315
 */


/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "drv_i2c_touchpad.h"
#include <stdlib.h>
#include <string.h>

/**********************************************************************************************************************
 * Macro definitions
 **********************************************************************************************************************/
#define GT911_RESET_PIN             BSP_IO_PORT_11_PIN_00
#define GT911_INT_PIN               BSP_IO_PORT_11_PIN_01
#define GT911_I2C_TIMEOUT_MS        (200U)

//GT911 部分寄存器定义
#define GT_CTRL_REG                 0x8040  //GT911控制寄存器
#define GT_CFGS_REG                 0x8047  //GT911配置起始地址寄存器
#define GT_CHECK_REG                0x80FF  //GT911校验和寄存器
#define GT_PID_REG                  0x8140  //GT911产品ID寄存器

#define GT_GSTID_REG                0x814E  //GT911当前检测到的触摸情况
#define GT_TP1_REG                  0x814F  //第一个触摸点数据地址
#define GT_TP2_REG                  0x8157  //第二个触摸点数据地址
#define GT_TP3_REG                  0x815F  //第三个触摸点数据地址
#define GT_TP4_REG                  0x8167  //第四个触摸点数据地址
#define GT_TP5_REG                  0x816F  //第五个触摸点数据地址

#define GT911_READ_X_MAX_REG        0x8048  /* X输出最大值 */
#define GT911_READ_Y_MAX_REG        0x804a  /* X输出最大值 */

#define GT911_READ_XY_REG           0x814E  /* 坐标寄存器 */
#define GT911_CLEARBUF_REG          0x814E  /* 清除坐标寄存器 */
#define GT911_CONFIG_REG            0x8047  /* 配置参数寄存器 */
#define GT911_COMMAND_REG           0x8040  /* 实时命令 */
#define GT911_PRODUCT_ID_REG        0x8140  /* productid */
#define GT911_VENDOR_ID_REG         0x814A  /* 当前模组选项信息 */
#define GT911_CONFIG_VERSION_REG    0x8047  /* 配置文件版本号 */
#define GT911_CONFIG_CHECKSUM_REG   0x80FF  /* 配置文件校验码 */
#define GT911_FIRMWARE_VERSION_REG  0x8144  /* 固件版本号 */

/**********************************************************************************************************************
 * Typedef definitions
 **********************************************************************************************************************/

/**用于存放每一个触控点的id，坐标，大小**/
typedef struct
{
    uint8_t id;
    uint16_t x;
    uint16_t y;
    uint16_t size;
} tp_point_info_t;

/**类结构体**/
typedef struct
{
    uint8_t tp_dev_addr;
    uint16_t height;
    uint16_t width;
    tp_rotation_t rotation;
    tp_point_info_t points_info[TOUCH_POINT_TOTAL]; //用于存储五个触控点的坐标
} tp_drv_t;

/***********************************************************************************************************************
 * Private function prototypes
 **********************************************************************************************************************/
static fsp_err_t i2c2_wait_for_event(i2c_master_event_t expected_event);
static fsp_err_t gt911_hardware_reset(void);

static fsp_err_t gt911_write_reg(uint16_t reg, uint8_t *buf, uint8_t len);
static fsp_err_t gt911_read_reg(uint16_t reg, uint8_t *buf, uint8_t len);
static fsp_err_t gt911_clear_buf(void);
static fsp_err_t gt911_get_gstid(uint8_t *buf);
static fsp_err_t gt911_get_version(uint8_t *buf);
static fsp_err_t gt911_get_vendor_id(uint8_t *buf);
static fsp_err_t gt911_get_product_id(uint8_t *buf);
static fsp_err_t gt911_get_max_x(uint8_t *buf);
static fsp_err_t gt911_get_max_y(uint8_t *buf);

/***********************************************************************************************************************
 * Private global variables
 **********************************************************************************************************************/
static tp_drv_t g_tp_drv;

static volatile i2c_master_event_t g_i2c2_event = (i2c_master_event_t) 0;

/***********************************************************************************************************************
 * Functions
 **********************************************************************************************************************/

fsp_err_t drv_i2c_touchpad_init(void)
{
    fsp_err_t err;
    uint8_t buf[4] = {0};

    /* 初始化I2C驱动 */
    err = g_i2c_master1.p_api->open (g_i2c_master1.p_ctrl, g_i2c_master1.p_cfg);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = gt911_hardware_reset ();
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    touchpad_set_rotation (TP_ROT_NONE);

    /* 读ID */
    // 厂商标识id
    err = gt911_get_vendor_id (buf);
    if (FSP_SUCCESS != err)
    {
        return err;
    }
    printf ("gt911 vendor id = %02x %02x %02x %02x\r\n", buf[0], buf[1], buf[2], buf[3]);

    // 产品id
    err = gt911_get_product_id (buf);
    if (FSP_SUCCESS != err)
    {
        return err;
    }
    printf ("gt911 product id = %02x %02x %02x %02x\r\n", buf[0], buf[1], buf[2], buf[3]);

    // 触摸芯片固件版本
    err = gt911_get_version (buf);
    if (FSP_SUCCESS != err)
    {
        return err;
    }
    printf ("version = 0x%x\r\n", buf[0]);

    err = gt911_get_max_x (buf);
    if (FSP_SUCCESS != err)
    {
        return err;
    }
    g_tp_drv.width = (uint16_t) ((buf[1] << 8) | buf[0]);
    printf ("touchpad max x = %d\r\n", g_tp_drv.width);

    err = gt911_get_max_y (buf);
    if (FSP_SUCCESS != err)
    {
        return err;
    }
    g_tp_drv.height = (uint16_t) ((buf[1] << 8) | buf[0]);
    printf ("touchpad max y = %d\r\n", g_tp_drv.height);

    return FSP_SUCCESS;
}

fsp_err_t touchpad_is_touched(void)
{
    fsp_err_t err;
    uint8_t touched_state, touch_num, buffer_status;

    err = gt911_get_gstid (&touched_state);
    if (FSP_SUCCESS != err)
    {
        return err;
    }
    touch_num = touched_state & 0xf;            //触点数量
    buffer_status = (touched_state >> 7) & 1;   // 帧状态
    //printf("touch_num: %d\r\n", touch_num);

    if (buffer_status == 1 && (touch_num <= TOUCH_POINT_TOTAL) && (touch_num > 0))
    {
        uint16_t pointers_regs[TOUCH_POINT_TOTAL] =
        { GT_TP1_REG, GT_TP2_REG, GT_TP3_REG, GT_TP4_REG, GT_TP5_REG };
        // 获取每个触控点的坐标值并保存
        for (int i = 0; i < touch_num; ++i)
        {
            uint8_t point_info_p[7] = {0};
            uint8_t point_info_per_size = (uint8_t) sizeof(point_info_p);
            err = gt911_read_reg (pointers_regs[i], point_info_p, point_info_per_size);
            if (FSP_SUCCESS != err)
            {
                return err;
            }

            g_tp_drv.points_info[i].id = point_info_p[0];
            uint16_t raw_x = (uint16_t) (point_info_p[1] + (point_info_p[2] << 8));
            uint16_t raw_y = (uint16_t) (point_info_p[3] + (point_info_p[4] << 8));
            g_tp_drv.points_info[i].x = (raw_x < g_tp_drv.width) ? raw_x : (uint16_t) (g_tp_drv.width - 1U);
            g_tp_drv.points_info[i].y = (raw_y < g_tp_drv.height) ? raw_y : (uint16_t) (g_tp_drv.height - 1U);
            g_tp_drv.points_info[i].size = (uint16_t) (point_info_p[5] + (point_info_p[6] << 8));

            //旋转方向
            uint16_t temp;
            switch (g_tp_drv.rotation)
            {
                case TP_ROT_NONE:
                    g_tp_drv.points_info[i].x = (uint16_t) (g_tp_drv.width - 1U - g_tp_drv.points_info[i].x);
                    g_tp_drv.points_info[i].y = (uint16_t) (g_tp_drv.height - 1U - g_tp_drv.points_info[i].y);
                break;
                case TP_ROT_270:
                    temp = g_tp_drv.points_info[i].x;
                    g_tp_drv.points_info[i].x = (g_tp_drv.points_info[i].y < g_tp_drv.width) ?
                                                (uint16_t) (g_tp_drv.width - 1U - g_tp_drv.points_info[i].y) : 0U;
                    g_tp_drv.points_info[i].y = temp;
                break;
                case TP_ROT_180:
                    //g_tp_drv.points_info[i].x = g_tp_drv.points_info[i].x;
                    //g_tp_drv.points_info[i].y = g_tp_drv.points_info[i].y;
                break;
                case TP_ROT_90:
                    temp = g_tp_drv.points_info[i].x;
                    g_tp_drv.points_info[i].x = g_tp_drv.points_info[i].y;
                    g_tp_drv.points_info[i].y = (uint16_t) (g_tp_drv.height - 1U - temp);
                break;
                default:
                break;
            }

            if (g_tp_drv.points_info[i].x >= g_tp_drv.width)
            {
                g_tp_drv.points_info[i].x = (uint16_t) (g_tp_drv.width - 1U);
            }
            if (g_tp_drv.points_info[i].y >= g_tp_drv.height)
            {
                g_tp_drv.points_info[i].y = (uint16_t) (g_tp_drv.height - 1U);
            }
        }
        err = gt911_clear_buf ();
        if (FSP_SUCCESS != err)
        {
            return err;
        }
        return FSP_SUCCESS;
    }
    //必须给GT911_POINT_INFO缓冲区置0,不然读取的数据一直为128！！！！
    err = gt911_clear_buf ();
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    return FSP_ERR_INVALID_DATA;
}

void touchpad_set_rotation(tp_rotation_t rotation)
{
    g_tp_drv.rotation = rotation;
}

void touchpad_get_pos(uint16_t *x, uint16_t *y, int index)
{
    *x = g_tp_drv.points_info[index].x;
    *y = g_tp_drv.points_info[index].y;
}

void i2c_master2_callback(i2c_master_callback_args_t *p_args)
{
    if (NULL != p_args)
    {
        g_i2c2_event = p_args->event;
    }
}

/***********************************************************************************************************************
 * Private Functions
 **********************************************************************************************************************/
static fsp_err_t i2c2_wait_for_event(i2c_master_event_t expected_event)
{
    for (uint32_t elapsed_ms = 0; elapsed_ms < GT911_I2C_TIMEOUT_MS; elapsed_ms++)
    {
        i2c_master_event_t event = g_i2c2_event;

        if (expected_event == event)
        {
            g_i2c2_event = (i2c_master_event_t) 0;
            return FSP_SUCCESS;
        }

        if (I2C_MASTER_EVENT_ABORTED == event)
        {
            g_i2c2_event = (i2c_master_event_t) 0;
            return FSP_ERR_ABORTED;
        }

        R_BSP_SoftwareDelay (1, BSP_DELAY_UNITS_MILLISECONDS);
    }

    (void) g_i2c_master1.p_api->abort (g_i2c_master1.p_ctrl);
    g_i2c2_event = (i2c_master_event_t) 0;

    return FSP_ERR_TIMEOUT;
}

static fsp_err_t gt911_hardware_reset(void)
{
    fsp_err_t err;
    uint32_t int_pin_cfg;

    if (0x14U == g_i2c_master1_cfg.slave)
    {
        int_pin_cfg = IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_HIGH;
    }
    else if (0x5DU == g_i2c_master1_cfg.slave)
    {
        int_pin_cfg = IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW;
    }
    else
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    err = g_ioport.p_api->pinCfg (g_ioport.p_ctrl, GT911_RESET_PIN,
                                  IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = g_ioport.p_api->pinCfg (g_ioport.p_ctrl, GT911_INT_PIN, int_pin_cfg);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    R_BSP_SoftwareDelay (10, BSP_DELAY_UNITS_MILLISECONDS);

    err = g_ioport.p_api->pinWrite (g_ioport.p_ctrl, GT911_RESET_PIN, BSP_IO_LEVEL_HIGH);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    R_BSP_SoftwareDelay (5, BSP_DELAY_UNITS_MILLISECONDS);

    err = g_ioport.p_api->pinWrite (g_ioport.p_ctrl, GT911_INT_PIN, BSP_IO_LEVEL_LOW);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    R_BSP_SoftwareDelay (50, BSP_DELAY_UNITS_MILLISECONDS);

    err = g_ioport.p_api->pinCfg (g_ioport.p_ctrl, GT911_INT_PIN, IOPORT_CFG_PORT_DIRECTION_INPUT);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    R_BSP_SoftwareDelay (50, BSP_DELAY_UNITS_MILLISECONDS);

    return FSP_SUCCESS;
}

static fsp_err_t gt911_clear_buf(void)
{
    uint8_t buf[1] =
    { 0 };
    return gt911_write_reg (GT911_CLEARBUF_REG, buf, 1);
}

static fsp_err_t gt911_write_reg(uint16_t reg, uint8_t *buf, uint8_t len)
{
    fsp_err_t err;

    uint8_t regl = (uint8_t) (reg & 0xff);
    uint8_t regh = (uint8_t) (reg >> 8);
    uint8_t *write_package = malloc ((len + 2) * sizeof(uint8_t));
    if (NULL == write_package)
    {
        return FSP_ERR_OUT_OF_MEMORY;
    }
    memcpy (write_package, &regh, 1);
    memcpy (write_package + 1, &regl, 1);
    memcpy (write_package + 2, buf, len);

    g_i2c2_event = (i2c_master_event_t) 0;
    err = g_i2c_master1.p_api->write (g_i2c_master1.p_ctrl, write_package, len + 2U, false);
    if (FSP_SUCCESS != err)
    {
        free (write_package);
        return err;
    }

    err = i2c2_wait_for_event (I2C_MASTER_EVENT_TX_COMPLETE);
    free (write_package);

    return err;
}

static fsp_err_t gt911_read_reg(uint16_t reg, uint8_t *buf, uint8_t len)
{
    fsp_err_t err;
    uint8_t tmpbuf[2];

    tmpbuf[0] = (uint8_t) (reg >> 8);
    tmpbuf[1] = (uint8_t) (reg & 0xff);

    g_i2c2_event = (i2c_master_event_t) 0;
    err = g_i2c_master1.p_api->write (g_i2c_master1.p_ctrl, tmpbuf, 2, true);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = i2c2_wait_for_event (I2C_MASTER_EVENT_TX_COMPLETE);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    g_i2c2_event = (i2c_master_event_t) 0;
    err = g_i2c_master1.p_api->read (g_i2c_master1.p_ctrl, buf, len, false);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = i2c2_wait_for_event (I2C_MASTER_EVENT_RX_COMPLETE);

    return err;
}

static fsp_err_t gt911_get_max_x(uint8_t *buf)
{
    return gt911_read_reg (GT911_READ_X_MAX_REG, buf, 2);
}

static fsp_err_t gt911_get_max_y(uint8_t *buf)
{
    return gt911_read_reg (GT911_READ_Y_MAX_REG, buf, 2);
}

static fsp_err_t gt911_get_product_id(uint8_t *buf)
{
    return gt911_read_reg (GT911_PRODUCT_ID_REG, buf, 4);
}

static fsp_err_t gt911_get_vendor_id(uint8_t *buf)
{
    return gt911_read_reg (GT911_VENDOR_ID_REG, buf, 4);
}

static fsp_err_t gt911_get_version(uint8_t *buf)
{
    return gt911_read_reg (GT911_CONFIG_VERSION_REG, buf, 1);
}

static fsp_err_t gt911_get_gstid(uint8_t *buf)
{
    return gt911_read_reg (GT_GSTID_REG, buf, 1);
}
