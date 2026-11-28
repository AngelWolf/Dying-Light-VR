#pragma once
#include <cstdint>

namespace dlvr::rigwork {

enum class Identity { Unknown, Player, Other };

// Identity belongs to the matrix storage, never to the transient job pointer.
// A capture change or failed read preserves the old behavior. In particular,
// Unknown must not be treated as Other during startup, model changes or unload.
struct Observation {
    uintptr_t player = 0;
    uintptr_t matrices = 0;
    bool stable = false;

    Identity Classify(uintptr_t currentPlayer) const {
        if (!stable || !player || !matrices || currentPlayer != player) {
            return Identity::Unknown;
        }
        return matrices == player ? Identity::Player : Identity::Other;
    }
    bool Apply(uintptr_t currentPlayer) const {
        return Classify(currentPlayer) != Identity::Other;
    }
    bool ApplyAfter(const Observation& after, uintptr_t currentPlayer) const {
        // The native call can change a binding or trigger model recapture.
        // Only a stable foreign buffer on BOTH sides can suppress the exit work.
        return Apply(currentPlayer) || after.Apply(currentPlayer)
            || after.player != player || after.matrices != matrices;
    }
};

template<class ReadPtr, class ReadPlayer>
Observation ObserveOwner(uintptr_t job, ReadPtr read, ReadPlayer player) {
    Observation o;
    o.player = player();
    o.matrices = job ? read(job + 0x1938) : 0;
    o.stable = o.player == player();
    return o;
}

template<class ReadPtr, class ReadPlayer>
Observation ObserveWriter(uintptr_t ctx, ReadPtr read, ReadPlayer player) {
    Observation o;
    o.player = player();
    const uintptr_t binding = ctx ? read(ctx + 0x40) : 0;
    o.matrices = binding ? read(binding + 0x1938) : 0;
    o.stable = o.player == player();
    return o;
}

} // namespace dlvr::rigwork
