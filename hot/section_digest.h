#ifndef HOT_HOT_SECTION_DIGEST_H
#define HOT_HOT_SECTION_DIGEST_H

#include <stdint.h>

// hot_section_digest.h — one trusted cold-payload descriptor.
//
// Single Class Per File Law: HotSectionDigest.
//
// A SLOT RECORD in the call sense, not in the storage sense: it is the row a
// caller hands to Hot_updateCold to say "this exact file, by this exact hash,
// is what I expected". It has no behavior and no owner of its own — the
// caller's array owns it, and the loader only reads it while the call is live.
//
// Filenames are exact. The digests come from a trusted local builder or
// release source, never from model output (the Loader Trust Boundary Law).

typedef struct HotSectionDigest {
    const char *filename;
    u8 sha256[32];
} HotSectionDigest;

#endif
