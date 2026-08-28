#ifndef ASTRA_ERROR_HPP
#define ASTRA_ERROR_HPP

#include <astra/export.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace astra {

enum class SchedulerCreationError : std::uint8_t {
    FinalizationStarted = 1,
};

class ASTRA_EXPORT scheduler_creation_rejected : public std::runtime_error {
public:
    explicit scheduler_creation_rejected(SchedulerCreationError reason)
        : std::runtime_error(format_message(reason)), reason_(reason) {}

    [[nodiscard]] SchedulerCreationError reason() const noexcept {
        return reason_;
    }

private:
    static const char* format_message(SchedulerCreationError reason) noexcept {
        switch (reason) {
            case SchedulerCreationError::FinalizationStarted:
                return "Scheduler creation rejected: process Finalization has already started";
            default:
                return "Scheduler creation rejected";
        }
    }

    SchedulerCreationError reason_;
};

}  // namespace astra

#endif  // ASTRA_ERROR_HPP
