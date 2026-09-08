/*
 * qspi_flash.c
 *
 *  Created on: 2026年7月23日
 *      Author: 36315
 */
#include "qspi_flash.h"
#include "hal_data.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define QSPI_FLASH_READY_TIMEOUT_US    (1000000U)

#define QSPI_ALARM_LEGACY_MAGIC       (0x414C524DU)
#define QSPI_ALARM_LIST_MAGIC         (0x414C4C34U)
#define QSPI_ALARM_LIST_VERSION       (2U)
/* 交替使用最后两个4 KiB扇区，更新时保留上一份有效闹钟列表。 */
#define QSPI_ALARM_SLOT_A_ADDRESS     (0x803FE000UL)
#define QSPI_ALARM_SLOT_B_ADDRESS     (0x803FF000UL)
/* W25Q Flash 的最小擦除单位为 4 KiB。 */
#define QSPI_ALARM_SECTOR_SIZE         (0x1000U)

/* 旧固件保存的单条闹钟格式，用于升级兼容。 */
typedef struct __attribute__((aligned(8)))
{
    uint32_t magic;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  enable;
    uint8_t  repeat_mask;
} alarm_flash_legacy_record_t;

/* 最多四条闹钟，记录长度为32字节并带版本、序号和CRC32。 */
typedef struct __attribute__((aligned(8)))
{
    uint32_t        magic;
    uint16_t        version;
    uint8_t         count;
    uint8_t         reserved0;
    alarm_setting_t settings[ALARM_SETTING_MAX_COUNT];
    uint32_t        sequence;
    uint32_t        crc32;
} alarm_flash_list_record_t;

_Static_assert(4U == sizeof(alarm_setting_t), "Alarm setting must remain 4 bytes");
_Static_assert(28U == offsetof(alarm_flash_list_record_t, crc32),
               "Alarm Flash CRC offset must remain 28 bytes");
_Static_assert(32U == sizeof(alarm_flash_list_record_t),
               "Alarm Flash record must remain 32 bytes");


/* 防止应用程序重复打开同一个 FSP OSPI 实例。 */
static bool s_qspi_flash_initialized = false;

static uint32_t alarm_flash_crc32(uint8_t const * data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFU;

    for (uint32_t byte_index = 0U; byte_index < length; byte_index++)
    {
        crc ^= data[byte_index];
        for (uint32_t bit_index = 0U; bit_index < 8U; bit_index++)
        {
            uint32_t const mask = (uint32_t) (-(int32_t) (crc & 1U));
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }

    return ~crc;
}

static bool alarm_setting_is_valid(alarm_setting_t const * setting)
{
    return (NULL != setting) &&
           (setting->hour <= 23U) &&
           (setting->minute <= 59U) &&
           (setting->enable <= 1U) &&
           (0U == (setting->repeat_mask & 0x80U));
}

static bool alarm_settings_are_valid(alarm_setting_t const * settings, uint8_t count)
{
    if ((count > ALARM_SETTING_MAX_COUNT) ||
        ((count > 0U) && (NULL == settings)))
    {
        return false;
    }

    for (uint8_t index = 0U; index < count; index++)
    {
        if (!alarm_setting_is_valid(&settings[index]))
        {
            return false;
        }

        for (uint8_t previous = 0U; previous < index; previous++)
        {
            if ((settings[previous].hour == settings[index].hour) &&
                (settings[previous].minute == settings[index].minute))
            {
                return false;
            }
        }
    }

    return true;
}

static void alarm_flash_cache_invalidate(uintptr_t address, uint32_t length)
{
#if BSP_CFG_DCACHE_ENABLED
    SCB_InvalidateDCache_by_Addr((void *) address, (int32_t) length);
#else
    FSP_PARAMETER_NOT_USED(address);
    FSP_PARAMETER_NOT_USED(length);
#endif
}

static bool alarm_flash_record_read(uintptr_t address,
                                    alarm_flash_list_record_t * record)
{
    uint32_t expected_crc;

    if (NULL == record)
    {
        return false;
    }

    alarm_flash_cache_invalidate(address, (uint32_t) sizeof(*record));
    memcpy(record, (void const *) address, sizeof(*record));
    expected_crc = alarm_flash_crc32(
        (uint8_t const *) record,
        (uint32_t) offsetof(alarm_flash_list_record_t, crc32));

    return (QSPI_ALARM_LIST_MAGIC == record->magic) &&
           (QSPI_ALARM_LIST_VERSION == record->version) &&
           (record->count <= ALARM_SETTING_MAX_COUNT) &&
           (0U == record->reserved0) &&
           alarm_settings_are_valid(record->settings, record->count) &&
           (expected_crc == record->crc32);
}

static bool alarm_flash_sequence_is_newer(uint32_t candidate, uint32_t reference)
{
    return ((int32_t) (candidate - reference)) > 0;
}

static fsp_err_t qspi_flash_send_simple_command(uint8_t command)
{
    spi_flash_direct_transfer_t transfer = {0};
    transfer.command=command;
    transfer.command_length=1U;

    return R_OSPI_B_DirectTransfer(g_ospi0.p_ctrl,
                                   &transfer,
                                   SPI_FLASH_DIRECT_TRANSFER_DIR_WRITE);
}
/**
 * @brief 等待 QSPI Flash 完成上一项擦除或写入操作。
 *
 * 通过读取 Flash 状态寄存器中的 WIP（Write In Progress）位判断忙状态。
 * 必须在调用擦除或写入函数后调用本函数，确保下一次操作不会在 Flash
 * 仍然忙时开始。
 *
 * @return FSP_SUCCESS Flash 已经空闲。
 * @return FSP_ERR_TIMEOUT 等待超时，Flash 可能异常或通信失败。
 * @return 其他 fsp_err_t 读取 Flash 状态失败。
 */
fsp_err_t qspi_flash_wait_ready(void)
{
    fsp_err_t err;
    spi_flash_status_t status={0};

    for(uint32_t elapsed_us=0U;elapsed_us<QSPI_FLASH_READY_TIMEOUT_US;elapsed_us++)
    {
        err=R_OSPI_B_StatusGet(g_ospi0.p_ctrl,&status);
        if(FSP_SUCCESS!=err)
        {
            return err;
        }
        if(!status.write_in_progress)
        {
            return FSP_SUCCESS;
        }
        //flash忙等1us
        R_BSP_SoftwareDelay(1U,BSP_DELAY_UNITS_MICROSECONDS);
    }
    return FSP_ERR_TIMEOUT;
}
/**
 * @brief 初始化板载 QSPI Flash。
 *
 * 初始化流程：
 * 1. 打开 FSP OSPI_B 驱动；
 * 2. 应用已验证示例中的采样时序修正；
 * 3. 发送 0x66 和 0x99，使 Flash 回到已知的 SPI 初始状态；
 * 4. 等待 Flash 空闲。
 *
 * 复位命令不会擦除或修改 Flash 中保存的数据。
 *
 * @return FSP_SUCCESS 初始化成功，Flash 可以开始读取或后续写入。
 * @return 其他 fsp_err_t 初始化或通信失败。
 */
 fsp_err_t qspi_flash_init(void)
{
    fsp_err_t err;
    if(s_qspi_flash_initialized)
    {
        return FSP_SUCCESS;
    }

    err = R_OSPI_B_Open(g_ospi0.p_ctrl, g_ospi0.p_cfg);
    if (FSP_SUCCESS != err)
    {
        return err;
    }
     /*
     * 与 qspi_cpkhmi_ra8p1_ep 示例保持一致的采样时序设置。
     * 该设置针对 CPKHMI-RA8P1 板载 QSPI Flash，先保留以保证通信稳定。
     */
    R_XSPI0->LIOCFGCS_b[0].SDRSMPMD = 1U;

    /* 0x66：Reset Enable，允许下一条 0x99 复位 Flash。 */
    err = qspi_flash_send_simple_command(0x66U);
    if (FSP_SUCCESS != err)
    {
        (void) R_OSPI_B_Close(g_ospi0.p_ctrl);
        return err;
    }

    /* 0x99：Reset Device，使 Flash 返回默认 SPI 状态。 */
    err = qspi_flash_send_simple_command(0x99U);
    if (FSP_SUCCESS != err)
    {
        (void) R_OSPI_B_Close(g_ospi0.p_ctrl);
        return err;
    }

    /* 等待 Flash 完成复位。 */
    R_BSP_SoftwareDelay(30U, BSP_DELAY_UNITS_MICROSECONDS);

    /* 确认 Flash 不处于擦写忙状态。 */
    err = qspi_flash_wait_ready();
    if (FSP_SUCCESS != err)
    {
        (void) R_OSPI_B_Close(g_ospi0.p_ctrl);
        return err;
    }
    s_qspi_flash_initialized = true;
    return FSP_SUCCESS;
}

bool alarm_settings_load(alarm_setting_t * settings, uint8_t capacity, uint8_t * count)
{
    alarm_flash_list_record_t slot_a;
    alarm_flash_list_record_t slot_b;
    alarm_flash_list_record_t const * selected = NULL;
    bool slot_a_valid;
    bool slot_b_valid;

    if ((NULL == settings) || (NULL == count) || (0U == capacity) ||
        !s_qspi_flash_initialized)
    {
        return false;
    }

    *count = 0U;
    slot_a_valid = alarm_flash_record_read(QSPI_ALARM_SLOT_A_ADDRESS, &slot_a);
    slot_b_valid = alarm_flash_record_read(QSPI_ALARM_SLOT_B_ADDRESS, &slot_b);

    if (slot_a_valid && slot_b_valid)
    {
        selected = alarm_flash_sequence_is_newer(slot_b.sequence, slot_a.sequence) ?
                   &slot_b : &slot_a;
    }
    else if (slot_a_valid)
    {
        selected = &slot_a;
    }
    else if (slot_b_valid)
    {
        selected = &slot_b;
    }

    if (NULL != selected)
    {
        if (selected->count > capacity)
        {
            return false;
        }
        memcpy(settings, selected->settings,
               (size_t) selected->count * sizeof(alarm_setting_t));
        *count = selected->count;
        return true;
    }

    /* 首次升级时导入旧版本的单条闹钟记录。 */
    alarm_flash_legacy_record_t legacy;
    alarm_flash_cache_invalidate(QSPI_ALARM_SLOT_B_ADDRESS,
                                 (uint32_t) sizeof(legacy));
    memcpy(&legacy, (void const *) QSPI_ALARM_SLOT_B_ADDRESS, sizeof(legacy));
    if (QSPI_ALARM_LEGACY_MAGIC == legacy.magic)
    {
        alarm_setting_t legacy_setting;
        legacy_setting.hour        = legacy.hour;
        legacy_setting.minute      = legacy.minute;
        legacy_setting.enable      = legacy.enable;
        legacy_setting.repeat_mask = legacy.repeat_mask;
        if (!alarm_setting_is_valid(&legacy_setting))
        {
            return false;
        }
        settings[0] = legacy_setting;
        *count = 1U;
        return true;
    }

    return false;
}

fsp_err_t alarm_settings_save(const alarm_setting_t * settings, uint8_t count)
{
    fsp_err_t err;
    alarm_flash_list_record_t record = {0};
    alarm_flash_list_record_t slot_a;
    alarm_flash_list_record_t slot_b;
    alarm_flash_list_record_t readback;
    bool slot_a_valid;
    bool slot_b_valid;
    uintptr_t target_address;
    FSP_CRITICAL_SECTION_DEFINE;

    if ((count > 0U) && (NULL == settings))
    {
        return FSP_ERR_INVALID_POINTER;
    }
    if (!s_qspi_flash_initialized)
    {
        return FSP_ERR_NOT_INITIALIZED;
    }
    if (!alarm_settings_are_valid(settings, count))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    slot_a_valid = alarm_flash_record_read(QSPI_ALARM_SLOT_A_ADDRESS, &slot_a);
    slot_b_valid = alarm_flash_record_read(QSPI_ALARM_SLOT_B_ADDRESS, &slot_b);

    record.magic     = QSPI_ALARM_LIST_MAGIC;
    record.version   = QSPI_ALARM_LIST_VERSION;
    record.count     = count;
    record.reserved0 = 0U;
    if (count > 0U)
    {
        memcpy(record.settings, settings, (size_t) count * sizeof(alarm_setting_t));
    }

    if (slot_a_valid && slot_b_valid)
    {
        bool const slot_b_is_newer =
            alarm_flash_sequence_is_newer(slot_b.sequence, slot_a.sequence);
        record.sequence = (slot_b_is_newer ? slot_b.sequence : slot_a.sequence) + 1U;
        target_address = slot_b_is_newer ? QSPI_ALARM_SLOT_A_ADDRESS :
                                           QSPI_ALARM_SLOT_B_ADDRESS;
    }
    else if (slot_a_valid)
    {
        record.sequence = slot_a.sequence + 1U;
        target_address = QSPI_ALARM_SLOT_B_ADDRESS;
    }
    else if (slot_b_valid)
    {
        record.sequence = slot_b.sequence + 1U;
        target_address = QSPI_ALARM_SLOT_A_ADDRESS;
    }
    else
    {
        record.sequence = 1U;
        target_address = QSPI_ALARM_SLOT_A_ADDRESS;
    }

    record.crc32 = alarm_flash_crc32(
        (uint8_t const *) &record,
        (uint32_t) offsetof(alarm_flash_list_record_t, crc32));

    err = R_OSPI_B_Erase(g_ospi0.p_ctrl,
                         (uint8_t *) target_address,
                         QSPI_ALARM_SECTOR_SIZE);
    if (FSP_SUCCESS != err)
    {
        return err;
    }
    err = qspi_flash_wait_ready();
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    FSP_CRITICAL_SECTION_ENTER;
    err = R_OSPI_B_Write(g_ospi0.p_ctrl,
                         (uint8_t const *) &record,
                         (uint8_t *) target_address,
                         (uint32_t) sizeof(record));
    FSP_CRITICAL_SECTION_EXIT;
    if (FSP_SUCCESS != err)
    {
        return err;
    }
    err = qspi_flash_wait_ready();
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    if (!alarm_flash_record_read(target_address, &readback) ||
        (0 != memcmp(&readback, &record, sizeof(record))))
    {
        return FSP_ERR_WRITE_FAILED;
    }
    return FSP_SUCCESS;
}

bool alarm_setting_load(alarm_setting_t * setting)
{
    alarm_setting_t settings[ALARM_SETTING_MAX_COUNT];
    uint8_t count = 0U;

    if ((NULL == setting) ||
        !alarm_settings_load(settings, ALARM_SETTING_MAX_COUNT, &count) ||
        (0U == count))
    {
        return false;
    }
    *setting = settings[0];
    return true;
}

fsp_err_t alarm_setting_save(const alarm_setting_t * setting)
{
    if (NULL == setting)
    {
        return FSP_ERR_INVALID_POINTER;
    }
    return alarm_settings_save(setting, 1U);
}
