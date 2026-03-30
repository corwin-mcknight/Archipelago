#pragma once

struct tracking_value {
    int value          = 0;
    bool move_observed = false;

    tracking_value()   = default;
    explicit tracking_value(int v) : value(v) {}

    tracking_value(const tracking_value&)            = default;
    tracking_value& operator=(const tracking_value&) = default;

    tracking_value(tracking_value&& other) noexcept : value(other.value), move_observed(true) {
        other.value         = -1;
        other.move_observed = true;
    }

    tracking_value& operator=(tracking_value&& other) noexcept {
        value               = other.value;
        move_observed       = true;
        other.value         = -1;
        other.move_observed = true;
        return *this;
    }
};
