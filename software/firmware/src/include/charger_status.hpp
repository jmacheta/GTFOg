#ifndef CHARGER_STATUS_HPP_
#define CHARGER_STATUS_HPP_

#include "error_codes.hpp"

#include <expected>

/**
 * Charger status
 * The values are encoded STAT1 and STAT2 pin states
 */
enum class ChargerStatus {
    off               = 0b00,
    recoverable_fault = 0b01,
    charging          = 0b10,
    charge_completed  = 0b11,
};


[[nodiscard]] auto get_charger_status() noexcept -> std::expected<ChargerStatus, error_code>;


#endif