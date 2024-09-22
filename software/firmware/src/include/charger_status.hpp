#ifndef CHARGER_STATUS_HPP_
#define CHARGER_STATUS_HPP_

/**
 * Charger status
 * The values are encoded STAT1 and STAT2 pin states
 */
enum class charger_status {
    off               = 0b00,
    recoverable_fault = 0b01,
    charging          = 0b10,
    charge_completed  = 0b11,
};

namespace events {
    struct charger_status_changed {
        charger_status new_status;
    };

} // namespace events

#endif