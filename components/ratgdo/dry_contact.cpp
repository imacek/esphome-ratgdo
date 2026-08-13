
#ifdef PROTOCOL_DRYCONTACT

#include "dry_contact.h"
#include "esphome/core/gpio.h"
#include "esphome/core/log.h"
#include "esphome/core/scheduler.h"
#include "ratgdo.h"

namespace esphome::ratgdo {
namespace dry_contact {

    static const char* const TAG = "ratgdo_dry_contact";
    static const char* const MOTION_WATCHDOG_TIMEOUT = "dc_motion_watchdog";

    void DryContact::setup(RATGDOComponent* ratgdo, Scheduler* scheduler, InternalGPIOPin* rx_pin, InternalGPIOPin* tx_pin)
    {
        this->ratgdo_ = ratgdo;
        this->scheduler_ = scheduler;
        this->tx_pin_ = tx_pin;
        this->rx_pin_ = rx_pin;

        this->limits_.open_limit_reached = 0;
        this->limits_.last_open_limit = 0;
        this->limits_.close_limit_reached = 0;
        this->limits_.last_close_limit = 0;
        this->door_state_ = DoorState::UNKNOWN;
    }

    void DryContact::loop()
    {
    }

    void DryContact::dump_config()
    {
        ESP_LOGCONFIG(TAG, "  Protocol: dry contact");
    }

    void DryContact::sync()
    {
        ESP_LOG1(TAG, "Ignoring sync action");
    }

    void DryContact::set_open_limit(bool state)
    {
        ESP_LOGD(TAG, "Set open_limit_reached to %d", state);
        this->limits_.last_open_limit = this->limits_.open_limit_reached;
        this->limits_.last_close_limit = false;
        this->limits_.open_limit_reached = state;
        this->send_door_state();
    }

    void DryContact::set_close_limit(bool state)
    {
        ESP_LOGD(TAG, "Set close_limit_reached to %d", state);
        this->limits_.last_close_limit = this->limits_.close_limit_reached;
        this->limits_.last_open_limit = false;
        this->limits_.close_limit_reached = state;
        this->send_door_state();
    }

    void DryContact::send_door_state()
    {
        if (this->limits_.open_limit_reached) {
            this->door_state_ = DoorState::OPEN;
        } else if (this->limits_.close_limit_reached) {
            this->door_state_ = DoorState::CLOSED;
        } else if (!this->limits_.close_limit_reached && !this->limits_.open_limit_reached) {
            if (this->limits_.last_close_limit) {
                this->door_state_ = DoorState::OPENING;
            }

            if (this->limits_.last_open_limit) {
                this->door_state_ = DoorState::CLOSING;
            }
        }

        if (this->door_state_ == DoorState::OPENING || this->door_state_ == DoorState::CLOSING) {
            this->arm_motion_watchdog_();
        } else {
            // travel confirmed or idle
            this->ratgdo_->cancel_timeout(MOTION_WATCHDOG_TIMEOUT);
        }

        this->ratgdo_->received(this->door_state_);
    }

    void DryContact::arm_motion_watchdog_()
    {
        float duration = this->door_state_ == DoorState::OPENING
            ? *this->ratgdo_->opening_duration
            : *this->ratgdo_->closing_duration;
        if (duration <= 0) {
            return; // travel durations not configured; watchdog disabled
        }

        // Re-arming replaces a pending timeout of the same name; confirmed or
        // intentionally ended travel cancels it outright.
        static constexpr uint32_t MOTION_WATCHDOG_MARGIN_MS = 3000;
        uint32_t timeout_ms = static_cast<uint32_t>(duration * 1000) + MOTION_WATCHDOG_MARGIN_MS;
        this->ratgdo_->set_timeout(MOTION_WATCHDOG_TIMEOUT, timeout_ms, [this] {
            ESP_LOGW(TAG, "No limit switch reached within expected travel time; door state unknown");
            this->door_state_ = DoorState::UNKNOWN;
            this->ratgdo_->received(this->door_state_);
        });
    }

    void DryContact::light_action(LightAction action)
    {
        ESP_LOG1(TAG, "Ignoring light action: %s", LOG_STR_ARG(LightAction_to_string(action)));
        return;
    }

    void DryContact::lock_action(LockAction action)
    {
        ESP_LOG1(TAG, "Ignoring lock action: %s", LOG_STR_ARG(LockAction_to_string(action)));
        return;
    }

    void DryContact::door_action(DoorAction action)
    {
        // Encoder builds do not have physical limit switches
        // so `limits_` is never updated. Instead, we rely on the derived `door_state`
        // which correctly reflects the software-calibrated limits.
#ifdef RATGDO_USE_ENCODER
        auto current_state = *this->ratgdo_->door_state;
        if (action == DoorAction::OPEN && current_state == DoorState::OPEN) {
            ESP_LOGW(TAG, "The door is already fully open. Ignoring door action: %s", LOG_STR_ARG(DoorAction_to_string(action)));
            return;
        }
        if (action == DoorAction::CLOSE && current_state == DoorState::CLOSED) {
            ESP_LOGW(TAG, "The door is already fully closed. Ignoring door action: %s", LOG_STR_ARG(DoorAction_to_string(action)));
            return;
        }
#else
        if (action == DoorAction::OPEN && this->limits_.open_limit_reached) {
            ESP_LOGW(TAG, "The door is already fully open. Ignoring door action: %s", LOG_STR_ARG(DoorAction_to_string(action)));
            return;
        }
        if (action == DoorAction::CLOSE && this->limits_.close_limit_reached) {
            ESP_LOGW(TAG, "The door is already fully closed. Ignoring door action: %s", LOG_STR_ARG(DoorAction_to_string(action)));
            return;
        }
#endif

        ESP_LOG1(TAG, "Door action: %s", LOG_STR_ARG(DoorAction_to_string(action)));

        // OPEN/CLOSE go exclusively to the discrete pins — on gate operators
        // with discrete command inputs (e.g. LiftMaster LA400UL expansion
        // board) also pulsing SBC would send two conflicting commands at
        // once. SBC serves TOGGLE and STOP. Discrete commands are
        // direction-absolute and honored even mid-travel (the operator
        // reverses immediately), but a reversal produces no limit switch
        // event — report the commanded direction optimistically and let the
        // limits (or the watchdog) settle the final state.
        if (action == DoorAction::OPEN && this->discrete_open_pin_ != nullptr) {
            this->discrete_open_pin_->digital_write(1);
            this->ratgdo_->set_timeout(500, [this] {
                this->discrete_open_pin_->digital_write(0);
            });
            if (this->door_state_ != DoorState::OPENING) {
                this->door_state_ = DoorState::OPENING;
                this->arm_motion_watchdog_();
                this->ratgdo_->received(this->door_state_);
            }
        } else if (action == DoorAction::CLOSE && this->discrete_close_pin_ != nullptr) {
            this->discrete_close_pin_->digital_write(1);
            this->ratgdo_->set_timeout(500, [this] {
                this->discrete_close_pin_->digital_write(0);
            });
            if (this->door_state_ != DoorState::CLOSING) {
                this->door_state_ = DoorState::CLOSING;
                this->arm_motion_watchdog_();
                this->ratgdo_->received(this->door_state_);
            }
        } else {
            this->tx_pin_->digital_write(1); // Single button control
            this->ratgdo_->set_timeout(500, [this] {
                this->tx_pin_->digital_write(0);
            });

            // SBC stops a moving operator (STOP explicitly, TOGGLE by cycle
            // semantics). The limit switches cannot observe a mid-travel stop,
            // but this one is self-issued — report STOPPED so the core freezes
            // the position estimate at this instant instead of overrunning it.
            if ((action == DoorAction::STOP || action == DoorAction::TOGGLE)
                && (this->door_state_ == DoorState::OPENING || this->door_state_ == DoorState::CLOSING)) {
                this->door_state_ = DoorState::STOPPED;
                this->ratgdo_->cancel_timeout(MOTION_WATCHDOG_TIMEOUT); // travel intentionally ended
                this->ratgdo_->received(this->door_state_);
            }
        }
    }

    Result DryContact::call(Args args)
    {
        return { };
    }

} // namespace dry_contact
} // namespace esphome::ratgdo

#endif // PROTOCOL_DRYCONTACT
