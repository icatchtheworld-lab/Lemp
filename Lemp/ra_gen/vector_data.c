/* generated vector source file - do not edit */
#include "bsp_api.h"
/* Do not build these data structures if no interrupts are currently allocated because IAR will have build errors. */
#if VECTOR_DATA_IRQ_COUNT > 0
        BSP_DONT_REMOVE const fsp_vector_t g_vector_table[BSP_ICU_VECTOR_NUM_ENTRIES] BSP_PLACE_IN_SECTION(BSP_SECTION_APPLICATION_VECTORS) =
        {
                        [0] = sci_b_uart_rxi_isr, /* SCI0 RXI (Receive data full) */
            [1] = sci_b_uart_txi_isr, /* SCI0 TXI (Transmit data empty) */
            [2] = sci_b_uart_tei_isr, /* SCI0 TEI (Transmit end) */
            [3] = sci_b_uart_eri_isr, /* SCI0 ERI (Receive error) */
            [4] = ssi_rxi_isr, /* SSI1 RXI (Receive data full/Transmit data empty) */
            [5] = ssi_int_isr, /* SSI1 INT (Error interrupt) */
            [6] = iic_master_rxi_isr, /* IIC1 RXI (Receive data full) */
            [7] = iic_master_txi_isr, /* IIC1 TXI (Transmit data empty) */
            [8] = iic_master_tei_isr, /* IIC1 TEI (Transmit end) */
            [9] = iic_master_eri_isr, /* IIC1 ERI (Transfer error) */
            [10] = rm_ethosu_isr, /* NPU IRQ (NPU IRQ) */
            [11] = ceu_isr, /* CEU CEUI (CEU interrupt) */
            [12] = sci_b_uart_rxi_isr, /* SCI9 RXI (Receive data full) */
            [13] = sci_b_uart_txi_isr, /* SCI9 TXI (Transmit data empty) */
            [14] = sci_b_uart_tei_isr, /* SCI9 TEI (Transmit end) */
            [15] = sci_b_uart_eri_isr, /* SCI9 ERI (Receive error) */
            [16] = rtc_carry_isr, /* RTC CARRY (Carry interrupt) */
            [17] = spi_b_rxi_isr, /* SPI1 RXI (Receive buffer full) */
            [18] = spi_b_tei_isr, /* SPI1 TEI (Transmission complete event) */
            [19] = spi_b_eri_isr, /* SPI1 ERI (Error) */
            [20] = dmac_int_isr, /* DMAC1 INT (DMAC1 transfer end) */
            [21] = gpt_counter_overflow_isr, /* GPT0 COUNTER OVERFLOW (Overflow) */
            [22] = adc_b_limclpi_isr, /* ADC LIMCLPI (Limiter clip interrupt with the limit table 0 to 7) */
            [23] = adc_b_err0_isr, /* ADC ERR0 (A/D converter unit 0 Error) */
            [24] = adc_b_err1_isr, /* ADC ERR1 (A/D converter unit 1 Error) */
            [25] = adc_b_resovf0_isr, /* ADC RESOVF0 (A/D conversion overflow on A/D converter unit 0) */
            [26] = adc_b_resovf1_isr, /* ADC RESOVF1 (A/D conversion overflow on A/D converter unit 1) */
            [27] = adc_b_calend0_isr, /* ADC CALEND0 (End of calibration of A/D converter unit 0) */
            [28] = adc_b_calend1_isr, /* ADC CALEND1 (End of calibration of A/D converter unit 1) */
            [29] = adc_b_adi0_isr, /* ADC ADI0 (End of A/D scanning operation(Gr.0)) */
            [30] = adc_b_adi1_isr, /* ADC ADI1 (End of A/D scanning operation(Gr.1)) */
            [31] = adc_b_adi2_isr, /* ADC ADI2 (End of A/D scanning operation(Gr.2)) */
            [32] = adc_b_adi3_isr, /* ADC ADI3 (End of A/D scanning operation(Gr.3)) */
            [33] = adc_b_adi4_isr, /* ADC ADI4 (End of A/D scanning operation(Gr.4)) */
            [34] = adc_b_fifoovf_isr, /* ADC FIFOOVF (FIFO data overflow) */
            [35] = adc_b_fiforeq0_isr, /* ADC FIFOREQ0 (FIFO data read request interrupt(Gr.0)) */
            [36] = adc_b_fiforeq1_isr, /* ADC FIFOREQ1 (FIFO data read request interrupt(Gr.1)) */
            [37] = adc_b_fiforeq2_isr, /* ADC FIFOREQ2 (FIFO data read request interrupt(Gr.2)) */
            [38] = adc_b_fiforeq3_isr, /* ADC FIFOREQ3 (FIFO data read request interrupt(Gr.3)) */
            [39] = adc_b_fiforeq4_isr, /* ADC FIFOREQ4 (FIFO data read request interrupt(Gr.4)) */
            [40] = sci_b_uart_rxi_isr, /* SCI1 RXI (Receive data full) */
            [41] = sci_b_uart_txi_isr, /* SCI1 TXI (Transmit data empty) */
            [42] = sci_b_uart_tei_isr, /* SCI1 TEI (Transmit end) */
            [43] = sci_b_uart_eri_isr, /* SCI1 ERI (Receive error) */
            [44] = ssi_txi_isr, /* SSI0 TXI (Transmit data empty) */
            [45] = ssi_int_isr, /* SSI0 INT (Error interrupt) */
        };
        #if BSP_FEATURE_ICU_HAS_IELSR
        const bsp_interrupt_event_t g_interrupt_event_link_select[BSP_ICU_VECTOR_NUM_ENTRIES] =
        {
            [0] = BSP_PRV_VECT_ENUM(EVENT_SCI0_RXI,GROUP0), /* SCI0 RXI (Receive data full) */
            [1] = BSP_PRV_VECT_ENUM(EVENT_SCI0_TXI,GROUP1), /* SCI0 TXI (Transmit data empty) */
            [2] = BSP_PRV_VECT_ENUM(EVENT_SCI0_TEI,GROUP2), /* SCI0 TEI (Transmit end) */
            [3] = BSP_PRV_VECT_ENUM(EVENT_SCI0_ERI,GROUP3), /* SCI0 ERI (Receive error) */
            [4] = BSP_PRV_VECT_ENUM(EVENT_SSI1_RXI,GROUP4), /* SSI1 RXI (Receive data full/Transmit data empty) */
            [5] = BSP_PRV_VECT_ENUM(EVENT_SSI1_INT,GROUP5), /* SSI1 INT (Error interrupt) */
            [6] = BSP_PRV_VECT_ENUM(EVENT_IIC1_RXI,GROUP6), /* IIC1 RXI (Receive data full) */
            [7] = BSP_PRV_VECT_ENUM(EVENT_IIC1_TXI,GROUP7), /* IIC1 TXI (Transmit data empty) */
            [8] = BSP_PRV_VECT_ENUM(EVENT_IIC1_TEI,GROUP0), /* IIC1 TEI (Transmit end) */
            [9] = BSP_PRV_VECT_ENUM(EVENT_IIC1_ERI,GROUP1), /* IIC1 ERI (Transfer error) */
            [10] = BSP_PRV_VECT_ENUM(EVENT_NPU_IRQ,GROUP2), /* NPU IRQ (NPU IRQ) */
            [11] = BSP_PRV_VECT_ENUM(EVENT_CEU_CEUI,GROUP3), /* CEU CEUI (CEU interrupt) */
            [12] = BSP_PRV_VECT_ENUM(EVENT_SCI9_RXI,GROUP4), /* SCI9 RXI (Receive data full) */
            [13] = BSP_PRV_VECT_ENUM(EVENT_SCI9_TXI,GROUP5), /* SCI9 TXI (Transmit data empty) */
            [14] = BSP_PRV_VECT_ENUM(EVENT_SCI9_TEI,GROUP6), /* SCI9 TEI (Transmit end) */
            [15] = BSP_PRV_VECT_ENUM(EVENT_SCI9_ERI,GROUP7), /* SCI9 ERI (Receive error) */
            [16] = BSP_PRV_VECT_ENUM(EVENT_RTC_CARRY,GROUP0), /* RTC CARRY (Carry interrupt) */
            [17] = BSP_PRV_VECT_ENUM(EVENT_SPI1_RXI,GROUP1), /* SPI1 RXI (Receive buffer full) */
            [18] = BSP_PRV_VECT_ENUM(EVENT_SPI1_TEI,GROUP2), /* SPI1 TEI (Transmission complete event) */
            [19] = BSP_PRV_VECT_ENUM(EVENT_SPI1_ERI,GROUP3), /* SPI1 ERI (Error) */
            [20] = BSP_PRV_VECT_ENUM(EVENT_DMAC1_INT,GROUP4), /* DMAC1 INT (DMAC1 transfer end) */
            [21] = BSP_PRV_VECT_ENUM(EVENT_GPT0_COUNTER_OVERFLOW,GROUP5), /* GPT0 COUNTER OVERFLOW (Overflow) */
            [22] = BSP_PRV_VECT_ENUM(EVENT_ADC_LIMCLPI,GROUP6), /* ADC LIMCLPI (Limiter clip interrupt with the limit table 0 to 7) */
            [23] = BSP_PRV_VECT_ENUM(EVENT_ADC_ERR0,GROUP7), /* ADC ERR0 (A/D converter unit 0 Error) */
            [24] = BSP_PRV_VECT_ENUM(EVENT_ADC_ERR1,GROUP0), /* ADC ERR1 (A/D converter unit 1 Error) */
            [25] = BSP_PRV_VECT_ENUM(EVENT_ADC_RESOVF0,GROUP1), /* ADC RESOVF0 (A/D conversion overflow on A/D converter unit 0) */
            [26] = BSP_PRV_VECT_ENUM(EVENT_ADC_RESOVF1,GROUP2), /* ADC RESOVF1 (A/D conversion overflow on A/D converter unit 1) */
            [27] = BSP_PRV_VECT_ENUM(EVENT_ADC_CALEND0,GROUP3), /* ADC CALEND0 (End of calibration of A/D converter unit 0) */
            [28] = BSP_PRV_VECT_ENUM(EVENT_ADC_CALEND1,GROUP4), /* ADC CALEND1 (End of calibration of A/D converter unit 1) */
            [29] = BSP_PRV_VECT_ENUM(EVENT_ADC_ADI0,GROUP5), /* ADC ADI0 (End of A/D scanning operation(Gr.0)) */
            [30] = BSP_PRV_VECT_ENUM(EVENT_ADC_ADI1,GROUP6), /* ADC ADI1 (End of A/D scanning operation(Gr.1)) */
            [31] = BSP_PRV_VECT_ENUM(EVENT_ADC_ADI2,GROUP7), /* ADC ADI2 (End of A/D scanning operation(Gr.2)) */
            [32] = BSP_PRV_VECT_ENUM(EVENT_ADC_ADI3,FIXED), /* ADC ADI3 (End of A/D scanning operation(Gr.3)) */
            [33] = BSP_PRV_VECT_ENUM(EVENT_ADC_ADI4,FIXED), /* ADC ADI4 (End of A/D scanning operation(Gr.4)) */
            [34] = BSP_PRV_VECT_ENUM(EVENT_ADC_FIFOOVF,FIXED), /* ADC FIFOOVF (FIFO data overflow) */
            [35] = BSP_PRV_VECT_ENUM(EVENT_ADC_FIFOREQ0,FIXED), /* ADC FIFOREQ0 (FIFO data read request interrupt(Gr.0)) */
            [36] = BSP_PRV_VECT_ENUM(EVENT_ADC_FIFOREQ1,FIXED), /* ADC FIFOREQ1 (FIFO data read request interrupt(Gr.1)) */
            [37] = BSP_PRV_VECT_ENUM(EVENT_ADC_FIFOREQ2,FIXED), /* ADC FIFOREQ2 (FIFO data read request interrupt(Gr.2)) */
            [38] = BSP_PRV_VECT_ENUM(EVENT_ADC_FIFOREQ3,FIXED), /* ADC FIFOREQ3 (FIFO data read request interrupt(Gr.3)) */
            [39] = BSP_PRV_VECT_ENUM(EVENT_ADC_FIFOREQ4,FIXED), /* ADC FIFOREQ4 (FIFO data read request interrupt(Gr.4)) */
            [40] = BSP_PRV_VECT_ENUM(EVENT_SCI1_RXI,FIXED), /* SCI1 RXI (Receive data full) */
            [41] = BSP_PRV_VECT_ENUM(EVENT_SCI1_TXI,FIXED), /* SCI1 TXI (Transmit data empty) */
            [42] = BSP_PRV_VECT_ENUM(EVENT_SCI1_TEI,FIXED), /* SCI1 TEI (Transmit end) */
            [43] = BSP_PRV_VECT_ENUM(EVENT_SCI1_ERI,FIXED), /* SCI1 ERI (Receive error) */
            [44] = BSP_PRV_VECT_ENUM(EVENT_SSI0_TXI,FIXED), /* SSI0 TXI (Transmit data empty) */
            [45] = BSP_PRV_VECT_ENUM(EVENT_SSI0_INT,FIXED), /* SSI0 INT (Error interrupt) */
        };
        #endif
        #endif
