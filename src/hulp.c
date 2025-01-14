#include <string.h>
#include <inttypes.h>

#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "esp_idf_version.h"
#include "esp32/rom/ets_sys.h"
#include "driver/gpio.h"
#if ESP_IDF_VERSION_MAJOR >= 5
#   include "esp_private/rtc_ctrl.h"
#else
#   include "driver/rtc_cntl.h"
#endif
#include "driver/rtc_io.h"
#include "soc/rtc.h"

#include "hulp.h"
#include "hulp_compat.h"
#include "hulp_config.h"

static const char* TAG = "HULP";

esp_err_t hulp_configure_pin(gpio_num_t pin, rtc_gpio_mode_t mode, gpio_pull_mode_t pull_mode, uint32_t level)
{
    if(
        ESP_OK != rtc_gpio_set_direction(pin, RTC_GPIO_MODE_DISABLED) ||
        ESP_OK != rtc_gpio_init(pin) ||
        ESP_OK != gpio_set_pull_mode(pin, pull_mode) ||
        ESP_OK != rtc_gpio_set_level(pin, level) ||
        ESP_OK != rtc_gpio_set_direction(pin, mode)
    )
    {
        ESP_LOGE(TAG, "[%s] error - %d (%d, %d, %" PRIu32 ")", __func__, pin, mode, pull_mode, level);
        return ESP_FAIL;
    }
    return ESP_OK;
}

#define RTCIO_FUNC_RTC_I2C 0x3

esp_err_t hulp_configure_i2c_pins(gpio_num_t scl_pin, gpio_num_t sda_pin, bool scl_pullup, bool sda_pullup)
{
    if( !(scl_pin == GPIO_NUM_2 || scl_pin == GPIO_NUM_4) )
    {
        ESP_LOGE(TAG, "invalid i2c hw SCL pin %d, must be 2 or 4", scl_pin);
        return ESP_ERR_INVALID_ARG;
    }
    if( !(sda_pin == GPIO_NUM_0 || sda_pin == GPIO_NUM_15) )
    {
        ESP_LOGE(TAG, "invalid i2c hw SDA pin %d, must be 0 or 15", sda_pin);
        return ESP_ERR_INVALID_ARG;
    }

    const int scl_rtcio_num = rtc_io_number_get(scl_pin);
    ESP_ERROR_CHECK(hulp_configure_pin(scl_pin, RTC_GPIO_MODE_INPUT_ONLY, scl_pullup ? GPIO_PULLUP_ONLY : GPIO_FLOATING, 0));
    SET_PERI_REG_BITS(rtc_io_desc[scl_rtcio_num].reg, RTC_IO_TOUCH_PAD1_FUN_SEL_V, RTCIO_FUNC_RTC_I2C, rtc_io_desc[scl_rtcio_num].func);
    const int sda_rtcio_num = rtc_io_number_get(sda_pin);
    ESP_ERROR_CHECK(hulp_configure_pin(sda_pin, RTC_GPIO_MODE_INPUT_ONLY, sda_pullup ? GPIO_PULLUP_ONLY : GPIO_FLOATING, 0));
    SET_PERI_REG_BITS(rtc_io_desc[sda_rtcio_num].reg, RTC_IO_TOUCH_PAD1_FUN_SEL_V, RTCIO_FUNC_RTC_I2C, rtc_io_desc[sda_rtcio_num].func);

    REG_SET_FIELD(RTC_IO_SAR_I2C_IO_REG, RTC_IO_SAR_I2C_SCL_SEL, scl_pin == GPIO_NUM_4 ? 0 : 1);
    REG_SET_FIELD(RTC_IO_SAR_I2C_IO_REG, RTC_IO_SAR_I2C_SDA_SEL, sda_pin == GPIO_NUM_0 ? 0 : 1);
    return ESP_OK;
}

esp_err_t hulp_configure_i2c_controller(const hulp_i2c_controller_config_t *config)
{
    if(!config)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(
        (config->scl_low > RTC_I2C_SCL_LOW_PERIOD_V) ||
        (config->scl_high > RTC_I2C_SCL_HIGH_PERIOD_V) ||
        (config->sda_duty > RTC_I2C_SDA_DUTY_V) ||
        (config->scl_start > RTC_I2C_SCL_START_PERIOD_V) ||
        (config->scl_stop > RTC_I2C_SCL_STOP_PERIOD_V) ||
        (config->timeout > RTC_I2C_TIMEOUT_V)
    )
    {
        return ESP_ERR_INVALID_ARG;
    }

    REG_SET_FIELD(RTC_I2C_CTRL_REG, RTC_I2C_RX_LSB_FIRST, config->rx_lsbfirst ? 1 : 0);
    REG_SET_FIELD(RTC_I2C_CTRL_REG, RTC_I2C_TX_LSB_FIRST, config->tx_lsbfirst ? 1 : 0);
    REG_SET_FIELD(RTC_I2C_CTRL_REG, RTC_I2C_SCL_FORCE_OUT, config->scl_pushpull ? 1 : 0);
    REG_SET_FIELD(RTC_I2C_CTRL_REG, RTC_I2C_SDA_FORCE_OUT, config->sda_pushpull ? 1 : 0);

    REG_SET_FIELD(RTC_I2C_SCL_LOW_PERIOD_REG, RTC_I2C_SCL_LOW_PERIOD, config->scl_low);
    REG_SET_FIELD(RTC_I2C_SCL_HIGH_PERIOD_REG, RTC_I2C_SCL_HIGH_PERIOD, config->scl_high);
    REG_SET_FIELD(RTC_I2C_SDA_DUTY_REG, RTC_I2C_SDA_DUTY, config->sda_duty);
    REG_SET_FIELD(RTC_I2C_SCL_START_PERIOD_REG, RTC_I2C_SCL_START_PERIOD, config->scl_start);
    REG_SET_FIELD(RTC_I2C_SCL_STOP_PERIOD_REG, RTC_I2C_SCL_STOP_PERIOD, config->scl_stop);
    REG_SET_FIELD(RTC_I2C_TIMEOUT_REG, RTC_I2C_TIMEOUT, config->timeout);
    REG_SET_FIELD(RTC_I2C_CTRL_REG, RTC_I2C_MS_MODE, 1);
    return ESP_OK;
}

esp_err_t hulp_register_i2c_slave(uint8_t index, uint8_t address)
{
    if(index > 7)
    {
        ESP_LOGE(TAG, "invalid i2c slave index (%u), range 0-7", index);
        return ESP_ERR_INVALID_ARG;
    }
    SET_PERI_REG_BITS(SENS_SAR_SLAVE_ADDR1_REG + (index / 2) * sizeof(uint32_t), SENS_I2C_SLAVE_ADDR0, address, (index % 2) ? SENS_I2C_SLAVE_ADDR1_S : SENS_I2C_SLAVE_ADDR0_S);
    return ESP_OK;
}

void hulp_tsens_configure(uint8_t clk_div)
{
    REG_SET_FIELD(SENS_SAR_TSENS_CTRL_REG, SENS_TSENS_CLK_DIV, clk_div);
    REG_SET_FIELD(SENS_SAR_MEAS_WAIT2_REG, SENS_FORCE_XPD_SAR, SENS_FORCE_XPD_SAR_PU);
    REG_CLR_BIT(SENS_SAR_TSENS_CTRL_REG, SENS_TSENS_POWER_UP);
    REG_CLR_BIT(SENS_SAR_TSENS_CTRL_REG, SENS_TSENS_DUMP_OUT);
    REG_CLR_BIT(SENS_SAR_TSENS_CTRL_REG, SENS_TSENS_POWER_UP_FORCE);
}

static void hulp_set_start_delay(void)
{
    /*
        ULP is not officially supported if RTC peripherals domain is powered on, however this is often desirable.
        The only observed bug is that, in deep sleep, the ULP may return to sleep very soon after starting up (typically after
        just the first instruction), resulting in an apparent doubled wakeup period.
        To fix this, the ULP start wait needs to be increased slightly (from the default 0x10).
        Note that ulp_set_wakeup_period adjusts for this setting so timing should be unaffected. There should also, therefore,
        be no side effects of setting this when unnecessary (ie. RTC peripherals not forced on).
    */
    REG_SET_FIELD(RTC_CNTL_TIMER2_REG, RTC_CNTL_ULPCP_TOUCH_START_WAIT, 0x20);
}

void hulp_peripherals_on(void)
{
    hulp_set_start_delay();
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
}

static uint64_t hulp_us_to_ticks(uint64_t time_us)
{
    return rtc_time_us_to_slowclk(time_us, esp_clk_slowclk_cal_get());
}

uint16_t hulp_ms_to_ulp_ticks_with_shift(uint32_t time_ms, uint8_t shift)
{
    return (uint16_t)((hulp_us_to_ticks(1000ULL * time_ms) >> shift) & 0xFFFF);
}

uint16_t hulp_ms_to_ulp_ticks(uint32_t time_ms)
{
    return hulp_ms_to_ulp_ticks_with_shift(time_ms, hulp_ms_to_ulp_tick_shift(time_ms));
}

uint16_t hulp_get_current_ulp_ticks(uint8_t shift)
{
    return (uint16_t)((rtc_time_get() >> shift) & 0xFFFF);
}

uint8_t hulp_ms_to_ulp_tick_shift(uint32_t time_ms)
{
    uint64_t rtc_slow_ticks = hulp_us_to_ticks(1000ULL * time_ms);
    if(rtc_slow_ticks == 0)
    {
        return 1;
    }

    uint8_t high_bit = 63 - __builtin_clzll(rtc_slow_ticks);
    if(high_bit >= 32)
    {
        // All 16 bits of upper register [47:32]
        return 32;
    }
    else if(high_bit < 16)
    {
        // Lower 16 bits
        // Note: tick count is updated every 2 ticks, so bit 0 is not interesting, therefore [16:1] rather than [15:0]
        return 1;
    }
    else
    {
        // [31:16] - [16:1]
        return high_bit - 15;
    }
    // ESP_LOGI(TAG, "%s ms: %u, ticks: %"PRIu64", range: [%u:%u], overflow: %"PRIu64" ms, resolution: %"PRIu64" uS", __func__, time_ms, rtc_slow_ticks, high_bit, high_bit - 15, rtc_time_slowclk_to_us((1ULL << (high_bit+1))-1,esp_clk_slowclk_cal_get()) / 1000, rtc_time_slowclk_to_us(1ULL << (high_bit-15),esp_clk_slowclk_cal_get()));
}

uint16_t hulp_get_label_pc(uint16_t label, const ulp_insn_t *program)
{
    uint16_t pc = 0;

    while(pc < HULP_ULP_RESERVE_MEM)
    {
        if(program->macro.opcode == OPCODE_MACRO)
        {
            if(program->macro.sub_opcode == SUB_OPCODE_MACRO_LABEL && program->macro.label == label)
            {
                ESP_LOGD(TAG, "label %u at pc %u", label, pc);
                return pc;
            }
        }
        else
        {
            ++pc;
        }
        ++program;
    }

    ESP_LOGE(TAG, "label %u not found", label);
    abort();
}

static uint32_t periph_sel_to_reg_base(uint32_t sel) {
    if(sel == 0) {
        return DR_REG_RTCCNTL_BASE;
    } else if (sel == 1) {
        return DR_REG_RTCIO_BASE;
    } else if (sel == 2) {
        return DR_REG_SENS_BASE;
    } else /* if (sel == 3) */ {
        return DR_REG_RTC_I2C_BASE;
    }
}

int hulp_print_instruction(const ulp_insn_t *ins)
{
    switch(ins->b.opcode)
    {
        case OPCODE_WR_REG:
        {
            return printf("I_WR_REG(0x%08X, %u, %u, %u)",
                (unsigned int)(periph_sel_to_reg_base(ins->wr_reg.periph_sel) + ins->wr_reg.addr * sizeof(uint32_t)),
                ins->wr_reg.low,
                ins->wr_reg.high,
                ins->wr_reg.data
            );
        }
        case OPCODE_RD_REG:
        {
            return printf("I_RD_REG(0x%08X, %u, %u)",
                (unsigned int)(periph_sel_to_reg_base(ins->rd_reg.periph_sel) + ins->rd_reg.addr * sizeof(uint32_t)),
                ins->rd_reg.low,
                ins->rd_reg.high
            );
        }
        case OPCODE_I2C:
        {
            return printf("I_I2C_RW(%u, %u, %u, %u, %u, %u)",
                ins->i2c.i2c_addr,
                ins->i2c.data,
                ins->i2c.low_bits,
                ins->i2c.high_bits,
                ins->i2c.i2c_sel,
                ins->i2c.rw
            );
        }
        case OPCODE_DELAY:
        {
            return printf("I_DELAY(%u)",
                ins->delay.cycles
            );
        }
        case OPCODE_ADC:
        {
            return printf("I_ADC(R%u, %u, %u)",
                ins->adc.dreg,
                ins->adc.sar_sel,
                ins->adc.mux - 1
            );
        }
        case OPCODE_ST:
        {
            return printf("I_ST(R%u, R%u, %u)",
                ins->st.dreg,
                ins->st.sreg,
                ins->st.offset
            );
        }
        case OPCODE_ALU:
        {
            switch(ins->alu_reg.sub_opcode)
            {
                case SUB_OPCODE_ALU_REG:
                {
                    switch(ins->alu_reg.sel)
                    {
                        case ALU_SEL_ADD:
                        {
                            return printf("I_ADDR(R%u, R%u, R%u)",
                                ins->alu_reg.dreg,
                                ins->alu_reg.sreg,
                                ins->alu_reg.treg
                            );
                        }
                        case ALU_SEL_SUB:
                        {
                            return printf("I_SUBR(R%u, R%u, R%u)",
                                ins->alu_reg.dreg,
                                ins->alu_reg.sreg,
                                ins->alu_reg.treg
                            );
                        }
                        case ALU_SEL_AND:
                        {
                            return printf("I_ANDR(R%u, R%u, R%u)",
                                ins->alu_reg.dreg,
                                ins->alu_reg.sreg,
                                ins->alu_reg.treg
                            );
                        }
                        case ALU_SEL_OR:
                        {
                            return printf("I_ORR(R%u, R%u, R%u)",
                                ins->alu_reg.dreg,
                                ins->alu_reg.sreg,
                                ins->alu_reg.treg
                            );
                        }
                        case ALU_SEL_MOV:
                        {
                            return printf("I_MOVR(R%u, R%u)",
                                ins->alu_reg.dreg,
                                ins->alu_reg.sreg
                            );
                        }
                        case ALU_SEL_LSH:
                        {
                            return printf("I_LSHR(R%u, R%u, R%u)",
                                ins->alu_reg.dreg,
                                ins->alu_reg.sreg,
                                ins->alu_reg.treg
                            );
                        }
                        case ALU_SEL_RSH:
                        {
                            return printf("I_RSHR(R%u, R%u, R%u)",
                                ins->alu_reg.dreg,
                                ins->alu_reg.sreg,
                                ins->alu_reg.treg
                            );
                        }
                        default:
                            break;
                    }
                    break;
                }
                case SUB_OPCODE_ALU_IMM:
                {
                    switch(ins->alu_imm.sel)
                    {
                        case ALU_SEL_ADD:
                        {
                            return printf("I_ADDI(R%u, R%u, %u)",
                                ins->alu_imm.dreg,
                                ins->alu_imm.sreg,
                                ins->alu_imm.imm
                            );
                        }
                        case ALU_SEL_SUB:
                        {
                            return printf("I_SUBI(R%u, R%u, %u)",
                                ins->alu_imm.dreg,
                                ins->alu_imm.sreg,
                                ins->alu_imm.imm
                            );
                        }
                        case ALU_SEL_AND:
                        {
                            return printf("I_ANDI(R%u, R%u, %u)",
                                ins->alu_imm.dreg,
                                ins->alu_imm.sreg,
                                ins->alu_imm.imm
                            );
                        }
                        case ALU_SEL_OR:
                        {
                            return printf("I_ORI(R%u, R%u, %u)",
                                ins->alu_imm.dreg,
                                ins->alu_imm.sreg,
                                ins->alu_imm.imm
                            );
                        }
                        case ALU_SEL_MOV:
                        {
                            return printf("I_MOVI(R%u, %u)",
                                ins->alu_imm.dreg,
                                ins->alu_imm.imm
                            );
                        }
                        case ALU_SEL_LSH:
                        {
                            return printf("I_LSHI(R%u, R%u, %u)",
                                ins->alu_imm.dreg,
                                ins->alu_imm.sreg,
                                ins->alu_imm.imm
                            );
                        }
                        case ALU_SEL_RSH:
                        {
                            return printf("I_RSHI(R%u, R%u, %u)",
                                ins->alu_imm.dreg,
                                ins->alu_imm.sreg,
                                ins->alu_imm.imm
                            );
                        }
                        default:
                            break;
                    }
                    break;
                }
                case SUB_OPCODE_ALU_CNT:
                {
                    switch(ins->alu_reg_s.sel)
                    {
                        case ALU_SEL_SINC:
                        {
                            return printf("I_STAGE_INC(%u)",
                                ins->alu_reg_s.imm
                            );
                        }
                        case ALU_SEL_SDEC:
                        {
                            return printf("I_STAGE_DEC(%u)",
                                ins->alu_reg_s.imm
                            );
                        }
                        case ALU_SEL_SRST:
                        {
                            return printf("I_STAGE_RST()");
                        }
                        default:
                            break;
                    }
                    break;
                }
                default:
                    break;
            }
            break;
        }
        case OPCODE_BRANCH:
        {
            switch(ins->b.sub_opcode)
            {
                case SUB_OPCODE_BX:
                {
                    switch(ins->bx.type)
                    {
                        case BX_JUMP_TYPE_DIRECT:
                        {
                            if(ins->bx.reg)
                            {
                                return printf("I_BXR(R%u)",
                                    ins->bx.dreg
                                );
                            }
                            else
                            {
                                return printf("I_BXI(%u)",
                                    ins->bx.addr
                                );
                            }
                        }
                        case BX_JUMP_TYPE_ZERO:
                        {
                            if(ins->bx.reg)
                            {
                                return printf("I_BXZR(R%u)",
                                    ins->bx.dreg
                                );
                            }
                            else
                            {
                                return printf("I_BXZI(%u)",
                                    ins->bx.addr
                                );
                            }
                        }
                        case BX_JUMP_TYPE_OVF:
                        {
                            if(ins->bx.reg)
                            {
                                return printf("I_BXFR(R%u)",
                                    ins->bx.dreg
                                );
                            }
                            else
                            {
                                return printf("I_BXFI(%u)",
                                    ins->bx.addr
                                );
                            }
                        }
                        default:
                            break;
                    }
                    break;
                }
                case SUB_OPCODE_BR:
                {
                    if(ins->b.cmp == B_CMP_L)
                    {
                        return printf("I_BL(%s%u, %u)",
                            ins->b.sign ? "-" : "",
                            ins->b.offset,
                            ins->b.imm
                        );
                    }
                    else
                    {
                        return printf("I_BGE(%s%u, %u)",
                            ins->b.sign ? "-" : "",
                            ins->b.offset,
                            ins->b.imm
                        );
                    }
                }
                case SUB_OPCODE_BS:
                {
                    return printf("I_JUMPS(%s%u, %u, %s)",
                        ins->bs.sign ? "-" : "",
                        ins->bs.offset,
                        ins->bs.imm,
                            (   ins->bs.cmp == JUMPS_LT ?   "JUMPS_LT" :
                            (   ins->bs.cmp == JUMPS_GE ?   "JUMPS_GE" :
                                                            "JUMPS_LE"
                            ))
                    );
                }
                default:
                    break;
            }
            break;
        }
        case OPCODE_END:
        {
            switch(ins->end.sub_opcode)
            {
                case SUB_OPCODE_END:
                {
                    return printf("I_WAKE()");
                }
                case SUB_OPCODE_SLEEP:
                {
                    return printf("I_SLEEP_CYCLE_SEL(%u)",
                        ins->sleep.cycle_sel
                    );
                }
                default:
                    break;
            }
            break;
        }
        case OPCODE_TSENS:
        {
            return printf("I_TSENS(R%u, %u)",
                ins->tsens.dreg,
                ins->tsens.wait_delay
            );
        }
        case OPCODE_HALT:
        {
            return printf("I_HALT()");
        }
        case OPCODE_LD:
        {
            return printf("I_LD(R%u, R%u, %u)",
                ins->ld.dreg,
                ins->ld.sreg,
                ins->ld.offset
            );
        }
        default:
            break;
    }
    return -1;
}

void hulp_print_program(const ulp_insn_t *program, size_t num_instructions)
{
    for(size_t i = 0; i < num_instructions; ++i)
    {
        if(hulp_print_instruction(&program[i]) < 0)
        {
            printf("I_INVALID(0x%08X)", (unsigned int)program[i].instruction);
        }
        printf(",\n");
    }
}

esp_err_t hulp_ulp_run(uint32_t entry_point)
{
    hulp_set_start_delay();
    return ulp_run(entry_point);
}

esp_err_t hulp_ulp_run_once(uint32_t entry_point)
{
    hulp_set_start_delay();
    // disable ULP timer
    CLEAR_PERI_REG_MASK(RTC_CNTL_STATE0_REG, RTC_CNTL_ULP_CP_SLP_TIMER_EN);
    // wait for at least 1 RTC_SLOW_CLK cycle
    ets_delay_us(10);
    // set entry point
    REG_SET_FIELD(SENS_SAR_START_FORCE_REG, SENS_PC_INIT, entry_point);
    // enable SW start
    SET_PERI_REG_MASK(SENS_SAR_START_FORCE_REG, SENS_ULP_CP_FORCE_START_TOP);
    // make sure voltage is raised when RTC 8MCLK is enabled
    SET_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_BIAS_I2C_FOLW_8M);
    SET_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_BIAS_CORE_FOLW_8M);
    SET_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_BIAS_SLEEP_FOLW_8M);
    // start
    CLEAR_PERI_REG_MASK(SENS_SAR_START_FORCE_REG, SENS_ULP_CP_START_TOP);
    SET_PERI_REG_MASK(SENS_SAR_START_FORCE_REG, SENS_ULP_CP_START_TOP);
    return ESP_OK;
}

esp_err_t hulp_ulp_load(const ulp_insn_t *program, size_t size_of_program, uint32_t period_us, uint32_t entry_point)
{
    size_t num_words = size_of_program / sizeof(ulp_insn_t);
    esp_err_t err = ulp_process_macros_and_load(entry_point, program, &num_words);
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "[%s] load error (0x%x)", __func__, err);
        return err;
    }
    hulp_set_start_delay();
    ulp_set_wakeup_period(0, period_us);
    return ESP_OK;
}

void hulp_ulp_end(void)
{
    CLEAR_PERI_REG_MASK(RTC_CNTL_STATE0_REG, RTC_CNTL_ULP_CP_SLP_TIMER_EN);
}

bool hulp_is_deep_sleep_wakeup(void)
{
    return (esp_reset_reason() == ESP_RST_DEEPSLEEP);
}

bool hulp_is_ulp_wakeup(void)
{
    return (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_ULP);
}

esp_err_t hulp_ulp_isr_register(intr_handler_t handler, void* handler_arg)
{
    #if ESP_IDF_VERSION_MAJOR >= 5
        return rtc_isr_register(handler, handler_arg, RTC_CNTL_SAR_INT_ST_M, 0);
    #else
        return rtc_isr_register(handler, handler_arg, RTC_CNTL_SAR_INT_ST_M);
    #endif
}

esp_err_t hulp_ulp_isr_deregister(intr_handler_t handler, void* handler_arg)
{
    return rtc_isr_deregister(handler, handler_arg);
}

void hulp_ulp_interrupt_en(void)
{
    REG_SET_BIT(RTC_CNTL_INT_ENA_REG, RTC_CNTL_ULP_CP_INT_ENA_M);
}

void hulp_ulp_interrupt_dis(void)
{
    REG_CLR_BIT(RTC_CNTL_INT_ENA_REG, RTC_CNTL_ULP_CP_INT_ENA_M);
}

esp_err_t hulp_configure_pin_int(gpio_num_t gpio_num, gpio_int_type_t intr_type)
{
    //See rtc_gpio_wakeup_enable
    int rtcio = rtc_io_number_get(gpio_num);
    if(rtcio < 0)
    {
        ESP_LOGE(TAG, "invalid rtcio (gpio %d)", gpio_num);
        return ESP_ERR_INVALID_ARG;
    }

    //edge interrupts work, however all behave as if GPIO_INTR_ANYEDGE
    if(intr_type == GPIO_INTR_POSEDGE || intr_type == GPIO_INTR_NEGEDGE)
    {
        ESP_LOGE(TAG, "POSEDGE and NEGEDGE not supported; use ANYEDGE");
        return ESP_ERR_INVALID_ARG;
    }

    REG_SET_FIELD(RTC_GPIO_PIN0_REG + rtcio * sizeof(uint32_t), RTC_GPIO_PIN0_INT_TYPE, intr_type);
    return ESP_OK;
}

ulp_state_t hulp_get_state(void)
{
    uint32_t ulp_state_bits = REG_READ(RTC_CNTL_LOW_POWER_ST_REG) & (0xF << 13);

    switch(ulp_state_bits)
    {
        case 0:
            return ULP_STATE_IDLE;
        case BIT(13) |  BIT(14):
            return ULP_STATE_RUNNING;
        case BIT(13) |  BIT(14) |             BIT(16):
            return ULP_STATE_HALTED;
        case                        BIT(15) | BIT(16):
            return ULP_STATE_SLEEPING;
        case            BIT(14) |             BIT(16):
        case            BIT(14) |   BIT(15) | BIT(16):
        case BIT(13) |  BIT(14) |   BIT(15) | BIT(16): //if sleep time ~0
            return ULP_STATE_WAKING;
        case                                  BIT(16):
            return ULP_STATE_DONE;
        default:
            ESP_LOGW(TAG, "unknown state: %" PRIu32, ulp_state_bits);
            return ULP_STATE_UNKNOWN;
    }
}

uint32_t hulp_get_fast_clk_freq(void)
{
#ifdef CONFIG_HULP_USE_APPROX_FAST_CLK
    return (uint32_t)RTC_FAST_CLK_FREQ_APPROX;
#else
    const bool clk_8m_enabled = rtc_clk_8m_enabled();
    const bool clk_8md256_enabled = rtc_clk_8md256_enabled();
    if (!clk_8m_enabled || !clk_8md256_enabled) {
        rtc_clk_8m_enable(true, true);
    }
    uint32_t ret = (uint32_t)(1000000ULL * (1 << RTC_CLK_CAL_FRACT) * 256 / rtc_clk_cal(RTC_CAL_8MD256, CONFIG_HULP_FAST_CLK_CAL_CYCLES));
    if (!clk_8m_enabled || !clk_8md256_enabled) {
        rtc_clk_8m_enable(clk_8m_enabled, clk_8md256_enabled);
    }
    return ret;
#endif
}
