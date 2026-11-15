// coopnet.h - the co-op arm-pose channel's EOS transport ([COOPNET]). Plan docs/superpowers/plans/2026-11-13-coop-arm-pose-channel.md
// Task 3; spec docs/superpowers/specs/2026-11-13-coop-arm-pose-channel-design.md sections 4.2 / 5a item 5 / 5b; trace
// docs/traces/20261113_1900_coopnet_transport.md. Our own packets over the game's EOS P2P link, on our own socket name and channel -
// the game's packets are only observed, never changed.
#pragma once

#include <cstdint>

#include "coopnet_logic.h"

namespace dlvr {

extern bool CoopPose;   // coop_pose - see the dlvr settings document; OFF = nothing installed, nothing sent

// Rig tick: installs the two IAT hooks once while coop_pose is ON. Idempotent; OFF = one bool test. Returns true once live.
bool CoopNetStart();
// Any thread: the newest local sample to send (latest wins). Sent on the next EOS tick to every known peer.
void CoopNetQueueSend(const coop::PoseSample& s);
// Any thread: up to `cap` peers (EOS ProductUserId handles as integers); returns the count. Index = the peer's slot for CoopNetLatest.
int CoopNetPeers(uint64_t* puidOut, int cap);
// The receiver clock the buffers are fed with (QueryPerformanceCounter, ms). Pass it to CoopNetLatest.
double CoopNetNowMs();
// Rig tick: the peer's buffered pose for `nowMs` (CoopNetNowMs) and its follow weight. False = no data for that peer.
bool CoopNetLatest(int peer, double nowMs, coop::PoseSample* out, float* weight);
// This build's id (our module's PE link timestamp) - what a peer must match.
uint32_t CoopNetBuildId();

}  // namespace dlvr
