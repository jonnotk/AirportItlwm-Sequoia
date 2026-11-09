/* Native full-authentication reassociation context. No borrowed node pointers. */
#ifndef _NET80211_IEEE80211_NATIVE_REASSOC_H_
#define _NET80211_IEEE80211_NATIVE_REASSOC_H_

#include <stdint.h>

struct ieee80211_native_reassoc_context {
    uint64_t generation;
    uint8_t previous_bssid[6];
    uint8_t target_bssid[6];
    uint8_t valid;
    uint8_t authenticated;
};

static inline void
ieee80211_native_reassoc_context_clear(struct ieee80211_native_reassoc_context *context)
{
    context->generation = 0;
    for (unsigned i = 0; i < 6; ++i)
        context->previous_bssid[i] = context->target_bssid[i] = 0;
    context->valid = context->authenticated = 0;
}

static inline int
ieee80211_native_reassoc_address_valid(const uint8_t *address)
{
    if (!address || (address[0] & 1))
        return 0;
    uint8_t any = 0;
    for (unsigned i = 0; i < 6; ++i)
        any |= address[i];
    return any != 0;
}

static inline int
ieee80211_native_reassoc_context_prepare(struct ieee80211_native_reassoc_context *context,
    uint64_t generation, const uint8_t *previous, const uint8_t *target)
{
    if (!generation || !ieee80211_native_reassoc_address_valid(previous) ||
        !ieee80211_native_reassoc_address_valid(target))
        return 0;
    uint8_t different = 0;
    for (unsigned i = 0; i < 6; ++i)
        different |= previous[i] ^ target[i];
    if (!different)
        return 0;
    struct ieee80211_native_reassoc_context copy = {0};
    copy.generation = generation;
    for (unsigned i = 0; i < 6; ++i) {
        copy.previous_bssid[i] = previous[i];
        copy.target_bssid[i] = target[i];
    }
    copy.valid = 1;
    *context = copy;
    return 1;
}

static inline int
ieee80211_native_reassoc_context_matches(const struct ieee80211_native_reassoc_context *context,
    uint64_t generation, const uint8_t *target)
{
    if (!context->valid || !generation || generation != context->generation || !target)
        return 0;
    for (unsigned i = 0; i < 6; ++i)
        if (context->target_bssid[i] != target[i])
            return 0;
    return 1;
}

static inline int
ieee80211_native_reassoc_context_authenticate(struct ieee80211_native_reassoc_context *context,
    uint64_t generation, const uint8_t *target)
{
    if (!ieee80211_native_reassoc_context_matches(context, generation, target))
        return 0;
    context->authenticated = 1;
    return 1;
}

static inline int
ieee80211_native_reassoc_context_current_ap(const struct ieee80211_native_reassoc_context *context,
    uint64_t generation, const uint8_t *target, uint8_t *address)
{
    if (!address || !context->authenticated ||
        !ieee80211_native_reassoc_context_matches(context, generation, target))
        return 0;
    for (unsigned i = 0; i < 6; ++i)
        address[i] = context->previous_bssid[i];
    return 1;
}

#endif
