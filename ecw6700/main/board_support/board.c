/* Minimal ECW6700F B02P12 Board binding. Pins/ports below are verbatim
 * published Board data, not SDK demo wiring or a second GPIO table. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "board.h"
#include "chip_pinmux.h"
#include "chip_smuaon.h"

/* Published bsp_ecw6700.h: logical PWM backend marker, not a pin. */
#define ECW6700_PWM_CCU6 0x100u
#include "pins.inc"
#define MOCE_BOARD_PORT(id, caps, label, ...) id,
enum moce_board_port {
#include "ports.inc"
};
#undef MOCE_BOARD_PORT
struct moce_port { unsigned instance; const uint8_t *pins; unsigned pin_count; bool output, initial_high; };
#define MOCE_BOARD_PORT(id, caps, label, ...) [id] = { __VA_ARGS__ },
static const struct moce_port ports[] = {
#include "ports.inc"
};
#undef MOCE_BOARD_PORT
struct moce_signal { unsigned port, channel; };
#define MOCE_BOUND_SIGNAL(signal, port, channel) [signal] = { port, channel },
static const struct moce_signal signals[] = {
#include "signals.inc"
};
#undef MOCE_BOUND_SIGNAL

unsigned moce_board_pin(enum moce_board_signal signal)
{
    if ((unsigned)signal >= MOCE_SIGNAL_COUNT) return UINT32_MAX;
    const struct moce_signal *s = &signals[signal];
    const struct moce_port *p = &ports[s->port];
    return s->channel < p->pin_count ? p->pins[s->channel] : UINT32_MAX;
}
unsigned moce_board_instance(enum moce_board_signal signal)
{
    return (unsigned)signal < MOCE_SIGNAL_COUNT ? ports[signals[signal].port].instance : UINT32_MAX;
}

/* GNU ld --wrap intercepts the SDK bus-init call, so vendor default pins
 * are never transiently configured for a selected bus. Bus resets, transfer
 * errors and ownership remain in the SDK; reopening uses this same function. */
extern int __real_chip_i2c_pinmux_init(unsigned int number);
int __wrap_chip_i2c_pinmux_init(unsigned int number)
{
    const struct moce_port *selected = NULL;
#define MOCE_BOUND_I2C(port) if (ports[port].instance == number) selected = &ports[port];
#include "i2c.inc"
#undef MOCE_BOUND_I2C
    if (!selected) return __real_chip_i2c_pinmux_init(number);
    if (selected->pin_count != 2 || number > 1) return -1;
    unsigned scl = selected->pins[0], sda = selected->pins[1];
    if (scl > 30 || sda > 30 || scl == sda) return -1;
    set_pin_function(scl, number == 0 ? FUNC_I2C0_SCL : FUNC_I2C1_SCL);
    set_pin_function(sda, number == 0 ? FUNC_I2C0_SDA : FUNC_I2C1_SDA);
    /* Same RMW pull-up semantics as B02P12 chip_i2c_pinmux_init, applied
     * only to the selected Board pins. Unrelated pull/pin state is retained. */
    uint32_t mask = (UINT32_C(1) << scl) | (UINT32_C(1) << sda);
    WRITE_REG(MEM_BASE_SMU_AON + 0x18, READ_REG(MEM_BASE_SMU_AON + 0x18) | mask);
    WRITE_REG(MEM_BASE_SMU_AON + 0x10, READ_REG(MEM_BASE_SMU_AON + 0x10) | mask);
    return 0;
}
