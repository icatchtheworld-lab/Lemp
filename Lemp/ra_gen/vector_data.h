/* generated vector header file - do not edit */
#ifndef VECTOR_DATA_H
#define VECTOR_DATA_H
#ifdef __cplusplus
        extern "C" {
        #endif
/* Number of interrupts allocated */
#ifndef VECTOR_DATA_IRQ_COUNT
#define VECTOR_DATA_IRQ_COUNT    (46)
#endif
/* ISR prototypes */
void sci_b_uart_rxi_isr(void);
void sci_b_uart_txi_isr(void);
void sci_b_uart_tei_isr(void);
void sci_b_uart_eri_isr(void);
void ssi_rxi_isr(void);
void ssi_int_isr(void);
void iic_master_rxi_isr(void);
void iic_master_txi_isr(void);
void iic_master_tei_isr(void);
void iic_master_eri_isr(void);
void rm_ethosu_isr(void);
void ceu_isr(void);
void rtc_carry_isr(void);
void spi_b_rxi_isr(void);
void spi_b_tei_isr(void);
void spi_b_eri_isr(void);
void dmac_int_isr(void);
void gpt_counter_overflow_isr(void);
void adc_b_limclpi_isr(void);
void adc_b_err0_isr(void);
void adc_b_err1_isr(void);
void adc_b_resovf0_isr(void);
void adc_b_resovf1_isr(void);
void adc_b_calend0_isr(void);
void adc_b_calend1_isr(void);
void adc_b_adi0_isr(void);
void adc_b_adi1_isr(void);
void adc_b_adi2_isr(void);
void adc_b_adi3_isr(void);
void adc_b_adi4_isr(void);
void adc_b_fifoovf_isr(void);
void adc_b_fiforeq0_isr(void);
void adc_b_fiforeq1_isr(void);
void adc_b_fiforeq2_isr(void);
void adc_b_fiforeq3_isr(void);
void adc_b_fiforeq4_isr(void);
void ssi_txi_isr(void);

/* Vector table allocations */
#define VECTOR_NUMBER_SCI0_RXI ((IRQn_Type) 0) /* SCI0 RXI (Receive data full) */
#define SCI0_RXI_IRQn          ((IRQn_Type) 0) /* SCI0 RXI (Receive data full) */
#define VECTOR_NUMBER_SCI0_TXI ((IRQn_Type) 1) /* SCI0 TXI (Transmit data empty) */
#define SCI0_TXI_IRQn          ((IRQn_Type) 1) /* SCI0 TXI (Transmit data empty) */
#define VECTOR_NUMBER_SCI0_TEI ((IRQn_Type) 2) /* SCI0 TEI (Transmit end) */
#define SCI0_TEI_IRQn          ((IRQn_Type) 2) /* SCI0 TEI (Transmit end) */
#define VECTOR_NUMBER_SCI0_ERI ((IRQn_Type) 3) /* SCI0 ERI (Receive error) */
#define SCI0_ERI_IRQn          ((IRQn_Type) 3) /* SCI0 ERI (Receive error) */
#define VECTOR_NUMBER_SSI1_RXI ((IRQn_Type) 4) /* SSI1 RXI (Receive data full/Transmit data empty) */
#define SSI1_RXI_IRQn          ((IRQn_Type) 4) /* SSI1 RXI (Receive data full/Transmit data empty) */
#define VECTOR_NUMBER_SSI1_INT ((IRQn_Type) 5) /* SSI1 INT (Error interrupt) */
#define SSI1_INT_IRQn          ((IRQn_Type) 5) /* SSI1 INT (Error interrupt) */
#define VECTOR_NUMBER_IIC1_RXI ((IRQn_Type) 6) /* IIC1 RXI (Receive data full) */
#define IIC1_RXI_IRQn          ((IRQn_Type) 6) /* IIC1 RXI (Receive data full) */
#define VECTOR_NUMBER_IIC1_TXI ((IRQn_Type) 7) /* IIC1 TXI (Transmit data empty) */
#define IIC1_TXI_IRQn          ((IRQn_Type) 7) /* IIC1 TXI (Transmit data empty) */
#define VECTOR_NUMBER_IIC1_TEI ((IRQn_Type) 8) /* IIC1 TEI (Transmit end) */
#define IIC1_TEI_IRQn          ((IRQn_Type) 8) /* IIC1 TEI (Transmit end) */
#define VECTOR_NUMBER_IIC1_ERI ((IRQn_Type) 9) /* IIC1 ERI (Transfer error) */
#define IIC1_ERI_IRQn          ((IRQn_Type) 9) /* IIC1 ERI (Transfer error) */
#define VECTOR_NUMBER_NPU_IRQ ((IRQn_Type) 10) /* NPU IRQ (NPU IRQ) */
#define NPU_IRQ_IRQn          ((IRQn_Type) 10) /* NPU IRQ (NPU IRQ) */
#define VECTOR_NUMBER_CEU_CEUI ((IRQn_Type) 11) /* CEU CEUI (CEU interrupt) */
#define CEU_CEUI_IRQn          ((IRQn_Type) 11) /* CEU CEUI (CEU interrupt) */
#define VECTOR_NUMBER_SCI9_RXI ((IRQn_Type) 12) /* SCI9 RXI (Receive data full) */
#define SCI9_RXI_IRQn          ((IRQn_Type) 12) /* SCI9 RXI (Receive data full) */
#define VECTOR_NUMBER_SCI9_TXI ((IRQn_Type) 13) /* SCI9 TXI (Transmit data empty) */
#define SCI9_TXI_IRQn          ((IRQn_Type) 13) /* SCI9 TXI (Transmit data empty) */
#define VECTOR_NUMBER_SCI9_TEI ((IRQn_Type) 14) /* SCI9 TEI (Transmit end) */
#define SCI9_TEI_IRQn          ((IRQn_Type) 14) /* SCI9 TEI (Transmit end) */
#define VECTOR_NUMBER_SCI9_ERI ((IRQn_Type) 15) /* SCI9 ERI (Receive error) */
#define SCI9_ERI_IRQn          ((IRQn_Type) 15) /* SCI9 ERI (Receive error) */
#define VECTOR_NUMBER_RTC_CARRY ((IRQn_Type) 16) /* RTC CARRY (Carry interrupt) */
#define RTC_CARRY_IRQn          ((IRQn_Type) 16) /* RTC CARRY (Carry interrupt) */
#define VECTOR_NUMBER_SPI1_RXI ((IRQn_Type) 17) /* SPI1 RXI (Receive buffer full) */
#define SPI1_RXI_IRQn          ((IRQn_Type) 17) /* SPI1 RXI (Receive buffer full) */
#define VECTOR_NUMBER_SPI1_TEI ((IRQn_Type) 18) /* SPI1 TEI (Transmission complete event) */
#define SPI1_TEI_IRQn          ((IRQn_Type) 18) /* SPI1 TEI (Transmission complete event) */
#define VECTOR_NUMBER_SPI1_ERI ((IRQn_Type) 19) /* SPI1 ERI (Error) */
#define SPI1_ERI_IRQn          ((IRQn_Type) 19) /* SPI1 ERI (Error) */
#define VECTOR_NUMBER_DMAC1_INT ((IRQn_Type) 20) /* DMAC1 INT (DMAC1 transfer end) */
#define DMAC1_INT_IRQn          ((IRQn_Type) 20) /* DMAC1 INT (DMAC1 transfer end) */
#define VECTOR_NUMBER_GPT0_COUNTER_OVERFLOW ((IRQn_Type) 21) /* GPT0 COUNTER OVERFLOW (Overflow) */
#define GPT0_COUNTER_OVERFLOW_IRQn          ((IRQn_Type) 21) /* GPT0 COUNTER OVERFLOW (Overflow) */
#define VECTOR_NUMBER_ADC_LIMCLPI ((IRQn_Type) 22) /* ADC LIMCLPI (Limiter clip interrupt with the limit table 0 to 7) */
#define ADC_LIMCLPI_IRQn          ((IRQn_Type) 22) /* ADC LIMCLPI (Limiter clip interrupt with the limit table 0 to 7) */
#define VECTOR_NUMBER_ADC_ERR0 ((IRQn_Type) 23) /* ADC ERR0 (A/D converter unit 0 Error) */
#define ADC_ERR0_IRQn          ((IRQn_Type) 23) /* ADC ERR0 (A/D converter unit 0 Error) */
#define VECTOR_NUMBER_ADC_ERR1 ((IRQn_Type) 24) /* ADC ERR1 (A/D converter unit 1 Error) */
#define ADC_ERR1_IRQn          ((IRQn_Type) 24) /* ADC ERR1 (A/D converter unit 1 Error) */
#define VECTOR_NUMBER_ADC_RESOVF0 ((IRQn_Type) 25) /* ADC RESOVF0 (A/D conversion overflow on A/D converter unit 0) */
#define ADC_RESOVF0_IRQn          ((IRQn_Type) 25) /* ADC RESOVF0 (A/D conversion overflow on A/D converter unit 0) */
#define VECTOR_NUMBER_ADC_RESOVF1 ((IRQn_Type) 26) /* ADC RESOVF1 (A/D conversion overflow on A/D converter unit 1) */
#define ADC_RESOVF1_IRQn          ((IRQn_Type) 26) /* ADC RESOVF1 (A/D conversion overflow on A/D converter unit 1) */
#define VECTOR_NUMBER_ADC_CALEND0 ((IRQn_Type) 27) /* ADC CALEND0 (End of calibration of A/D converter unit 0) */
#define ADC_CALEND0_IRQn          ((IRQn_Type) 27) /* ADC CALEND0 (End of calibration of A/D converter unit 0) */
#define VECTOR_NUMBER_ADC_CALEND1 ((IRQn_Type) 28) /* ADC CALEND1 (End of calibration of A/D converter unit 1) */
#define ADC_CALEND1_IRQn          ((IRQn_Type) 28) /* ADC CALEND1 (End of calibration of A/D converter unit 1) */
#define VECTOR_NUMBER_ADC_ADI0 ((IRQn_Type) 29) /* ADC ADI0 (End of A/D scanning operation(Gr.0)) */
#define ADC_ADI0_IRQn          ((IRQn_Type) 29) /* ADC ADI0 (End of A/D scanning operation(Gr.0)) */
#define VECTOR_NUMBER_ADC_ADI1 ((IRQn_Type) 30) /* ADC ADI1 (End of A/D scanning operation(Gr.1)) */
#define ADC_ADI1_IRQn          ((IRQn_Type) 30) /* ADC ADI1 (End of A/D scanning operation(Gr.1)) */
#define VECTOR_NUMBER_ADC_ADI2 ((IRQn_Type) 31) /* ADC ADI2 (End of A/D scanning operation(Gr.2)) */
#define ADC_ADI2_IRQn          ((IRQn_Type) 31) /* ADC ADI2 (End of A/D scanning operation(Gr.2)) */
#define VECTOR_NUMBER_ADC_ADI3 ((IRQn_Type) 32) /* ADC ADI3 (End of A/D scanning operation(Gr.3)) */
#define ADC_ADI3_IRQn          ((IRQn_Type) 32) /* ADC ADI3 (End of A/D scanning operation(Gr.3)) */
#define VECTOR_NUMBER_ADC_ADI4 ((IRQn_Type) 33) /* ADC ADI4 (End of A/D scanning operation(Gr.4)) */
#define ADC_ADI4_IRQn          ((IRQn_Type) 33) /* ADC ADI4 (End of A/D scanning operation(Gr.4)) */
#define VECTOR_NUMBER_ADC_FIFOOVF ((IRQn_Type) 34) /* ADC FIFOOVF (FIFO data overflow) */
#define ADC_FIFOOVF_IRQn          ((IRQn_Type) 34) /* ADC FIFOOVF (FIFO data overflow) */
#define VECTOR_NUMBER_ADC_FIFOREQ0 ((IRQn_Type) 35) /* ADC FIFOREQ0 (FIFO data read request interrupt(Gr.0)) */
#define ADC_FIFOREQ0_IRQn          ((IRQn_Type) 35) /* ADC FIFOREQ0 (FIFO data read request interrupt(Gr.0)) */
#define VECTOR_NUMBER_ADC_FIFOREQ1 ((IRQn_Type) 36) /* ADC FIFOREQ1 (FIFO data read request interrupt(Gr.1)) */
#define ADC_FIFOREQ1_IRQn          ((IRQn_Type) 36) /* ADC FIFOREQ1 (FIFO data read request interrupt(Gr.1)) */
#define VECTOR_NUMBER_ADC_FIFOREQ2 ((IRQn_Type) 37) /* ADC FIFOREQ2 (FIFO data read request interrupt(Gr.2)) */
#define ADC_FIFOREQ2_IRQn          ((IRQn_Type) 37) /* ADC FIFOREQ2 (FIFO data read request interrupt(Gr.2)) */
#define VECTOR_NUMBER_ADC_FIFOREQ3 ((IRQn_Type) 38) /* ADC FIFOREQ3 (FIFO data read request interrupt(Gr.3)) */
#define ADC_FIFOREQ3_IRQn          ((IRQn_Type) 38) /* ADC FIFOREQ3 (FIFO data read request interrupt(Gr.3)) */
#define VECTOR_NUMBER_ADC_FIFOREQ4 ((IRQn_Type) 39) /* ADC FIFOREQ4 (FIFO data read request interrupt(Gr.4)) */
#define ADC_FIFOREQ4_IRQn          ((IRQn_Type) 39) /* ADC FIFOREQ4 (FIFO data read request interrupt(Gr.4)) */
#define VECTOR_NUMBER_SCI1_RXI ((IRQn_Type) 40) /* SCI1 RXI (Receive data full) */
#define SCI1_RXI_IRQn          ((IRQn_Type) 40) /* SCI1 RXI (Receive data full) */
#define VECTOR_NUMBER_SCI1_TXI ((IRQn_Type) 41) /* SCI1 TXI (Transmit data empty) */
#define SCI1_TXI_IRQn          ((IRQn_Type) 41) /* SCI1 TXI (Transmit data empty) */
#define VECTOR_NUMBER_SCI1_TEI ((IRQn_Type) 42) /* SCI1 TEI (Transmit end) */
#define SCI1_TEI_IRQn          ((IRQn_Type) 42) /* SCI1 TEI (Transmit end) */
#define VECTOR_NUMBER_SCI1_ERI ((IRQn_Type) 43) /* SCI1 ERI (Receive error) */
#define SCI1_ERI_IRQn          ((IRQn_Type) 43) /* SCI1 ERI (Receive error) */
#define VECTOR_NUMBER_SSI0_TXI ((IRQn_Type) 44) /* SSI0 TXI (Transmit data empty) */
#define SSI0_TXI_IRQn          ((IRQn_Type) 44) /* SSI0 TXI (Transmit data empty) */
#define VECTOR_NUMBER_SSI0_INT ((IRQn_Type) 45) /* SSI0 INT (Error interrupt) */
#define SSI0_INT_IRQn          ((IRQn_Type) 45) /* SSI0 INT (Error interrupt) */
/* The number of entries required for the ICU vector table. */
#define BSP_ICU_VECTOR_NUM_ENTRIES (46)

#ifdef __cplusplus
        }
        #endif
#endif /* VECTOR_DATA_H */
