#include "signaling.hpp"


static signaling_state& operator|=(signaling_state& lhs, signaling_state const& rhs) {
    lhs = static_cast<signaling_state>(static_cast<unsigned>(lhs) | static_cast<unsigned>(rhs));
    return lhs;
}

static signaling_state operator&(signaling_state const& lhs, signaling_state const& rhs) {
    return static_cast<signaling_state>(static_cast<unsigned>(lhs) & static_cast<unsigned>(rhs));
}

class signaling {
    auto current = signaling_state::off;



};
