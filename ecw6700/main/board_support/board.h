#ifndef MOCE_BOARD_SUPPORT_H
#define MOCE_BOARD_SUPPORT_H
/* Frozen signal identities; values are NOT GPIO numbers. */
enum moce_board_signal {
    MOCE_SIGNAL_I2C_SCL,
    MOCE_SIGNAL_I2C_SDA,
    MOCE_SIGNAL_COUNT
};
unsigned moce_board_pin(enum moce_board_signal signal);
unsigned moce_board_instance(enum moce_board_signal signal);
/* SDK open/close and errors retain their native contract. The registered
 * recipe binds chip_i2c_pinmux_init on every open/reopen to this Board. */
#endif
