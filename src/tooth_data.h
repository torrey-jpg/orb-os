#pragma once
// Kevin's Orb — Tooth Explorer data. The 32 permanent teeth in Universal numbering.
//
// Numbering: #1 is the patient's upper-right third molar, counting across the upper arch to
// #16 (upper-left third molar), then down to #17 (lower-left third molar) and back across to
// #32 (lower-right third molar).
//
// Eruption ages: American Dental Association, "Permanent Teeth Eruption Chart" (MouthHealthy).
// Facts: general dental anatomy, written for this device and to be reviewed by Dr. Hollar.
// Mirror-image teeth (e.g. #3 and #14) share a type, so they share a name, age and fact.
//
// ASCII only: the on-device fonts have no fallback glyphs (see docs/adding-a-screen.md).

namespace tooth_data {

struct ToothType {
    const char *name;      // "First Molar"
    const char *erupts;    // "6-7 years"
    const char *fact;      // one or two short sentences
};

// Index by type: 0..7 = upper third molar .. upper central incisor,
//                8..15 = lower third molar .. lower central incisor.
static const ToothType kTypes[16] = {
    // ---- upper (maxillary) ----
    { "Third Molar",      "17-21 years",
      "The wisdom tooth. Third molars are the teeth most often missing from birth." },
    { "Second Molar",     "12-13 years",
      "Often called the 12-year molar, after the age it usually arrives." },
    { "First Molar",      "6-7 years",
      "The 6-year molar. It usually has three roots, and the palatal root is the largest." },
    { "Second Premolar",  "10-12 years",
      "Usually has a single root, unlike the two-rooted first premolar beside it." },
    { "First Premolar",   "10-11 years",
      "Usually has two roots: one on the cheek side and one on the palate side." },
    { "Canine",           "11-12 years",
      "The upper canine has the longest root of any tooth in the mouth." },
    { "Lateral Incisor",  "8-9 years",
      "One of the teeth most often missing from birth, after wisdom teeth and lower second premolars." },
    { "Central Incisor",  "7-8 years",
      "The widest incisor in the mouth, and the centerpiece of every smile." },
    // ---- lower (mandibular) ----
    { "Third Molar",      "17-21 years",
      "Lower wisdom teeth are the teeth most often impacted." },
    { "Second Molar",     "11-13 years",
      "Usually has two roots, one toward the front and one toward the back." },
    { "First Molar",      "6-7 years",
      "Often the first permanent tooth to arrive, and one of the most likely to need a filling." },
    { "Second Premolar",  "11-12 years",
      "After wisdom teeth, the tooth most often missing from birth." },
    { "First Premolar",   "10-12 years",
      "Its tongue-side cusp is tiny, so it can look like a small canine." },
    { "Canine",           "9-10 years",
      "Arrives around age 9-10, about two years before the upper canine." },
    { "Lateral Incisor",  "7-8 years",
      "Slightly larger than the lower central incisor, the reverse of the upper arch." },
    { "Central Incisor",  "6-7 years",
      "The smallest tooth in the permanent set." },
};

// Universal number (1..32) -> index into kTypes.
inline int typeOf(int num) {
    if (num >= 1 && num <= 8)   return num - 1;            // #1..#8  : upper right, molar -> incisor
    if (num >= 9 && num <= 16)  return 16 - num;           // #9..#16 : upper left, incisor -> molar
    if (num >= 17 && num <= 24) return 8 + (num - 17);     // #17..#24: lower left, molar -> incisor
    if (num >= 25 && num <= 32) return 8 + (32 - num);     // #25..#32: lower right, incisor -> molar
    return 0;
}

inline const char *quadrantOf(int num) {
    if (num <= 8)  return "Upper Right";
    if (num <= 16) return "Upper Left";
    if (num <= 24) return "Lower Left";
    return "Lower Right";
}

}  // namespace tooth_data
