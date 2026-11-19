#pragma once
// heldmesh - "has the HELD item's mesh changed under the same item?" (2026-11-19, INV-751, trace
// docs/traces/20261119_1840_weapon_stage_rekey_c1.md). Pure logic, no engine reads: unit-tested in tests/heldmesh_logic_test.cpp.
//
// The game's durability stage swap (clean -> _dm_a -> _dm_b, a repair back) keeps the item object AND its InventoryItem (INV-706)
// while the engine builds a NEW mesh (Mac CModelObject::InitMesh) - so every cache keyed on the item alone keeps the OLD mesh's
// element numbers. The key here is the MESH NAME (CModelObject::SetMeshName's string). It deliberately carries NO element address:
// an item model has two element buffers chosen per THREAD (INV-178), so an address would differ between the capture thread and the
// writer thread.
//
// The engine sets the new name and builds the new mesh in ONE call. A read that lands between the two sees the new name on the
// old mesh, so an in-place change re-resolves TWICE: on the update the name changed and on the next one (`again`).
namespace heldmesh {

constexpr unsigned long long kUnread = 0x9E3779B97F4A7C15ull;   // the key of an unreadable / empty name

// FNV-1a 64 of the name; nullptr or "" -> kUnread.
inline unsigned long long KeyOf(const char* name)
{
    if (!name || !name[0]) { return kUnread; }
    unsigned long long h = 1469598103934665603ull;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p) {
        h ^= *p;
        h *= 1099511628211ull;
    }
    return h == kUnread ? kUnread + 1 : h;
}

struct Track {
    unsigned long long key = 0;     // 0 = never stepped
    bool again = false;             // an in-place change was seen last update: re-resolve once more
};

// One update of one cache. Returns true when the cache must re-resolve its element numbers THIS update.
//   itemChanged: the cache's own item test fired (a new item / InventoryItem) - today's path, one pass, no `again`.
//   keyNow     : KeyOf(the held mesh's name) read this update.
inline bool Step(Track& t, unsigned long long keyNow, bool itemChanged)
{
    if (itemChanged || t.key == 0) {
        t.key = keyNow;
        t.again = false;
        return true;
    }
    if (keyNow != t.key) {          // the SAME item now carries another mesh: re-resolve now and on the next update
        t.key = keyNow;
        t.again = true;
        return true;
    }
    if (t.again) {
        t.again = false;
        return true;
    }
    return false;
}

// The loose-part writer's refusal: write only a list built on the mesh held NOW, and never while an in-place change is settling.
inline bool WriterMayWrite(const Track& t, unsigned long long keyNow)
{
    return t.key != 0 && t.key == keyNow && !t.again;
}

}  // namespace heldmesh
